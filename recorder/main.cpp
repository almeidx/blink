// blink-recorder: records a KWin screen cast (PipeWire) to an H.264/AAC MP4 file. Driven by the tray app over
// stdin/stdout, see recorderprotocol.h.

#include "audioencoder.h"
#include "capture.h"
#include "events.h"
#include "framequeue.h"
#include "mixer.h"
#include "muxer.h"
#include "recorderprotocol.h"
#include "thumbnail.h"
#include "videoencoder.h"

extern "C" {
#include <libavutil/log.h>
}

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <poll.h>
#include <signal.h>
#include <sys/prctl.h>
#include <sys/signalfd.h>
#include <time.h>
#include <unistd.h>

namespace blink::recorder {
namespace {

using namespace std::chrono_literals;

constexpr auto kFirstFrameTimeout = 5s;

struct Arguments {
    Capture::Options capture;
    VideoEncoder::Options video;
    std::string output;
};

int64_t monotonicNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1'000'000'000LL) + ts.tv_nsec;
}

template<typename T>
bool parseNumber(std::string_view text, T &value)
{
    const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
    return error == std::errc() && end == text.data() + text.size();
}

std::optional<Arguments> parse(int argc, char **argv, std::string &error)
{
    Arguments arguments;
    bool target = false;
    for (int i = 1; i < argc; ++i) {
        const std::string_view option = argv[i];
        if (option == kOptSystemAudio) {
            arguments.capture.systemAudio = true;
            continue;
        }
        if (option == kOptMicrophone) {
            arguments.capture.microphone = true;
            continue;
        }
        if (i + 1 == argc) {
            error = "missing value for " + std::string(option);
            return std::nullopt;
        }
        const std::string_view value = argv[++i];
        bool valid = true;
        if (option == kOptSerial || option == kOptNode) {
            valid = parseNumber(value, arguments.capture.target);
            arguments.capture.bySerial = option == kOptSerial;
            target = true;
        } else if (option == kOptOutput) {
            arguments.output = value;
            valid = !value.empty();
        } else if (option == kOptFps) {
            valid = parseNumber(value, arguments.video.fps) && arguments.video.fps > 0 && arguments.video.fps <= 1000;
        } else if (option == kOptQuality) {
            if (value == kQualityLow)
                arguments.video.quality = Quality::Low;
            else if (value == kQualityMedium)
                arguments.video.quality = Quality::Medium;
            else if (value == kQualityHigh)
                arguments.video.quality = Quality::High;
            else
                valid = false;
        } else if (option == kOptEncoder) {
            if (value == kEncoderAuto)
                arguments.video.family = EncoderFamily::Auto;
            else if (value == kEncoderNvenc)
                arguments.video.family = EncoderFamily::Nvenc;
            else if (value == kEncoderVaapi)
                arguments.video.family = EncoderFamily::Vaapi;
            else if (value == kEncoderSoftware)
                arguments.video.family = EncoderFamily::Software;
            else
                valid = false;
        } else {
            error = "unknown option " + std::string(option);
            return std::nullopt;
        }
        if (!valid) {
            error = "invalid value for " + std::string(option) + ": " + std::string(value);
            return std::nullopt;
        }
    }
    if (!target) {
        error = "missing " + std::string(kOptSerial) + " or " + std::string(kOptNode);
        return std::nullopt;
    }
    if (arguments.output.empty()) {
        error = "missing " + std::string(kOptOutput);
        return std::nullopt;
    }
    arguments.capture.fps = arguments.video.fps;
    return arguments;
}

enum class Outcome { Stop, Abort, Error };

// Waits for a command, a signal, the end of the screen cast or a fatal error.
Outcome control(Events &events, int signals, std::string &error)
{
    const int64_t deadline = monotonicNs() + std::chrono::nanoseconds(kFirstFrameTimeout).count();
    std::array<pollfd, 3> fds{{{events.fd(), POLLIN, 0}, {signals, POLLIN, 0}, {STDIN_FILENO, POLLIN, 0}}};
    std::string line;
    std::array<char, 256> buffer{};
    for (;;) {
        int timeout = -1;
        if (!events.hasFrames()) {
            const int64_t left = deadline - monotonicNs();
            if (left <= 0) {
                error = "no frames from the compositor";
                return Outcome::Error;
            }
            timeout = static_cast<int>((left + 999'999) / 1'000'000);
        }
        if (poll(fds.data(), fds.size(), timeout) < 0 && errno != EINTR) {
            error = "poll failed";
            return Outcome::Error;
        }
        if (fds[0].revents) {
            events.clear();
            if (events.failed(error))
                return Outcome::Error;
            if (events.ended())
                return Outcome::Stop;
        }
        if (fds[1].revents)
            return Outcome::Stop;
        if (fds[2].revents) {
            const ssize_t size = read(STDIN_FILENO, buffer.data(), buffer.size());
            if (size <= 0) {
                if (size < 0 && (errno == EINTR || errno == EAGAIN))
                    continue;
                return Outcome::Stop;
            }
            for (const char c : std::string_view(buffer.data(), static_cast<size_t>(size))) {
                if (c != '\n') {
                    line += c;
                    continue;
                }
                while (!line.empty() && (line.back() == '\r' || line.back() == ' '))
                    line.pop_back();
                if (line == kCmdStop)
                    return Outcome::Stop;
                if (line == kCmdAbort)
                    return Outcome::Abort;
                line.clear();
            }
        }
    }
}

int record(const Arguments &arguments, int signals)
{
    Events events;
    Muxer muxer;
    FrameQueue frames;
    Mixer mixer;
    std::unique_ptr<AudioEncoder> audio;
    VideoEncoder video(arguments.video, frames, muxer, events);
    Capture capture(events, frames, mixer);
    auto discard = [&] {
        frames.cancel();
        video.join();
        if (audio) {
            audio->cancel();
            audio->join();
        }
        muxer.discard();
    };
    auto fail = [&](const std::string &message) {
        discard();
        Events::emit(kEvError, message);
        return 1;
    };

    if (std::string error = muxer.open(arguments.output); !error.empty())
        return fail(error);
    if (std::string error = capture.start(arguments.capture); !error.empty())
        return fail(error);
    if (capture.audioSources() > 0) {
        audio = std::make_unique<AudioEncoder>(mixer, muxer, events);
        if (std::string error = audio->open(); !error.empty())
            return fail(error);
    }
    // Audio is set up first: its stream must exist when the video thread writes the header.
    video.start();
    if (audio)
        audio->start();

    std::string error;
    switch (control(events, signals, error)) {
    case Outcome::Error:
        return fail(error);
    case Outcome::Abort:
        discard();
        Events::emit(kEvAborted);
        return 0;
    case Outcome::Stop:
        break;
    }

    const int64_t stopNs = monotonicNs();
    capture.stopVideo();
    video.finish(stopNs);
    if (audio) {
        audio->finish(stopNs);
        audio->join();
    }
    if (events.failed(error))
        return fail(error);
    if (!muxer.started()) {
        // Stopped before the first frame (a quick start/stop): nothing was lost, so it is an abort, not a failure.
        // The threads are joined above, so the muxer is the only thing left to drop.
        muxer.discard();
        Events::emit(kEvAborted);
        return 0;
    }
    if (error = muxer.finish(); !error.empty())
        return fail(error);
    // Diagnostics only: a few frames go while the encoder opens.
    if (const uint64_t dropped = frames.dropped(); dropped > 0)
        std::fputs(("blink-recorder: the encoder fell behind, " + std::to_string(dropped) + " frames dropped\n").c_str(), stderr);
    Events::emit(kEvFinished);
    return 0;
}

} // namespace
} // namespace blink::recorder

int main(int argc, char **argv)
{
    using namespace blink::recorder;

    av_log_set_level(AV_LOG_ERROR);
    // Before stdout is made line buffered: the thumbnail is binary.
    if (argc > 1 && std::string_view(argv[1]) == kOptThumbnail)
        return thumbnail(argc, argv);

    // The tray app owns the recording: if it dies, finish the file.
    prctl(PR_SET_PDEATHSIG, SIGTERM);
    std::setvbuf(stdout, nullptr, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);

    std::string error;
    const std::optional<Arguments> arguments = parse(argc, argv, error);
    if (!arguments) {
        Events::emit(kEvError, error);
        return 1;
    }

    // Blocked before any thread starts, so every thread inherits the mask and the signals only reach the signalfd.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);
    const int signalFd = signalfd(-1, &signals, SFD_CLOEXEC | SFD_NONBLOCK);
    if (signalFd < 0) {
        Events::emit(kEvError, "cannot watch signals");
        return 1;
    }
    const int status = record(*arguments, signalFd);
    close(signalFd);
    return status;
}
