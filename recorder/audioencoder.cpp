#include "audioencoder.h"

#include "events.h"
#include "mixer.h"
#include "muxer.h"

extern "C" {
#include <libavutil/channel_layout.h>
}

#include <algorithm>
#include <chrono>
#include <span>

#include <time.h>

namespace blink::recorder {
namespace {

using namespace std::chrono_literals;

constexpr int64_t kBitRate = 160'000;
// The encoder thread's tick; a fraction of the mixer latency, so the bus never comes close to overflowing.
constexpr auto kTick = 20ms;

int64_t monotonicNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * 1'000'000'000LL) + ts.tv_nsec;
}

} // namespace

AudioEncoder::AudioEncoder(Mixer &mixer, Muxer &muxer, Events &events) : mixer_(mixer), muxer_(muxer), events_(events) {}

AudioEncoder::~AudioEncoder()
{
    cancel();
    join();
}

std::string AudioEncoder::open()
{
    const AVCodec *codec = avcodec_find_encoder_by_name("aac");
    if (!codec)
        codec = avcodec_find_encoder_by_name("libopus");
    if (!codec)
        return "no AAC or Opus encoder";
    codec_.reset(avcodec_alloc_context3(codec));
    frame_.reset(av_frame_alloc());
    packet_.reset(av_packet_alloc());
    if (!codec_ || !frame_ || !packet_)
        return "out of memory";

    const void *configs = nullptr;
    int count = 0;
    avcodec_get_supported_config(codec_.get(), codec, AV_CODEC_CONFIG_SAMPLE_FORMAT, 0, &configs, &count);
    const std::span formats(static_cast<const AVSampleFormat *>(configs), configs ? static_cast<size_t>(count) : 0);
    if (formats.empty() || std::ranges::find(formats, AV_SAMPLE_FMT_FLTP) != formats.end())
        codec_->sample_fmt = AV_SAMPLE_FMT_FLTP;
    else if (std::ranges::find(formats, AV_SAMPLE_FMT_FLT) != formats.end())
        codec_->sample_fmt = AV_SAMPLE_FMT_FLT;
    else
        return std::string(codec->name) + " takes no float samples";
    planar_ = codec_->sample_fmt == AV_SAMPLE_FMT_FLTP;
    smallLastFrame_ = (codec->capabilities & AV_CODEC_CAP_SMALL_LAST_FRAME) != 0;

    codec_->sample_rate = Mixer::kRate;
    av_channel_layout_default(&codec_->ch_layout, Mixer::kChannels);
    codec_->bit_rate = kBitRate;
    codec_->time_base = {1, Mixer::kRate};
    if (muxer_.globalHeader())
        codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    int error = avcodec_open2(codec_.get(), codec, nullptr);
    if (error < 0)
        return std::string("cannot open ") + codec->name + ": " + avError(error);
    if (codec->id == AV_CODEC_ID_OPUS)
        muxer_.setExperimental();

    stream_ = muxer_.addAudioStream();
    if (!stream_)
        return "cannot create the audio stream";
    if (std::string message = muxer_.setParameters(stream_, codec_.get()); !message.empty())
        return message;

    frame_->format = codec_->sample_fmt;
    frame_->sample_rate = Mixer::kRate;
    frame_->nb_samples = codec_->frame_size > 0 ? codec_->frame_size : 1024;
    error = av_channel_layout_copy(&frame_->ch_layout, &codec_->ch_layout);
    if (error >= 0)
        error = av_frame_get_buffer(frame_.get(), 0);
    if (error < 0)
        return "cannot allocate audio frames: " + avError(error);
    return {};
}

void AudioEncoder::start()
{
    thread_ = std::thread([this] { run(); });
}

void AudioEncoder::finish(int64_t stopNs)
{
    {
        std::lock_guard lock(mutex_);
        stopNs_ = stopNs;
    }
    wakeup_.notify_one();
}

void AudioEncoder::cancel()
{
    {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
    }
    wakeup_.notify_one();
}

void AudioEncoder::join()
{
    if (thread_.joinable())
        thread_.join();
}

void AudioEncoder::run()
{
    const int frameSize = frame_->nb_samples;
    for (;;) {
        int64_t stopNs = 0;
        {
            std::unique_lock lock(mutex_);
            wakeup_.wait_for(lock, kTick, [this] { return cancelled_; });
            if (cancelled_)
                return;
            stopNs = stopNs_;
        }
        const bool stopping = stopNs != INT64_MAX;
        // The header is written after the first video frame, which also starts the mixer.
        if (!muxer_.started()) {
            if (stopping)
                return;
            continue;
        }
        const int64_t settled = monotonicNs() - Mixer::kLatencyNs;
        const int64_t end = mixer_.position(std::min(settled, stopNs));
        int64_t position = mixer_.readPosition();
        for (; end - position >= frameSize; position += frameSize) {
            if (!encode(frameSize))
                return;
        }
        if (stopping && settled >= stopNs) {
            const auto rest = static_cast<int>(end - position);
            if (rest > 0 && !encode(smallLastFrame_ ? rest : frameSize))
                return;
            const int error = avcodec_send_frame(codec_.get(), nullptr);
            if (error < 0) {
                events_.fail("cannot flush the audio encoder: " + avError(error));
                return;
            }
            drain();
            return;
        }
    }
}

bool AudioEncoder::encode(int frames)
{
    // A no-op unless the encoder still holds the previous samples.
    int error = av_frame_make_writable(frame_.get());
    if (error < 0) {
        events_.fail("cannot allocate audio frames: " + avError(error));
        return false;
    }
    frame_->pts = mixer_.readPosition();
    frame_->nb_samples = frames;
    mixer_.read(reinterpret_cast<float *>(frame_->data[0]), planar_ ? reinterpret_cast<float *>(frame_->data[1]) : nullptr, frames);
    error = avcodec_send_frame(codec_.get(), frame_.get());
    if (error < 0) {
        events_.fail("cannot encode audio: " + avError(error));
        return false;
    }
    return drain();
}

bool AudioEncoder::drain()
{
    for (;;) {
        const int error = avcodec_receive_packet(codec_.get(), packet_.get());
        if (error == AVERROR(EAGAIN) || error == AVERROR_EOF)
            return true;
        if (error < 0) {
            events_.fail("cannot encode audio: " + avError(error));
            return false;
        }
        if (std::string message = muxer_.write(packet_.get(), stream_, codec_->time_base); !message.empty()) {
            events_.fail(message);
            return false;
        }
    }
}

} // namespace blink::recorder
