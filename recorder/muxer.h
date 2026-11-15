#pragma once

// The MP4 file. The video and audio encoder threads write packets through it; the header is written once the
// video encoder knows its parameters (after the first frame), and audio waits for that.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

#include <atomic>
#include <mutex>
#include <string>

namespace blink::recorder {

class Muxer {
public:
    Muxer() = default;
    ~Muxer();
    Muxer(const Muxer &) = delete;
    Muxer &operator=(const Muxer &) = delete;

    enum class Stream { Video, Audio };

    // Creates the file and the video stream (index 0); returns an error message on failure.
    std::string open(const std::string &path);
    // Before avcodec_open2(): sets the codec flags the container needs (a global header for MP4).
    void prepare(AVCodecContext *codec) const;
    // Copy an opened encoder's parameters into the video stream, or into a new audio stream (index 1); return an
    // error message on failure.
    std::string setVideoParameters(const AVCodecContext *codec);
    std::string addAudioStream(const AVCodecContext *codec);
    // Writes the header; all streams must have their parameters.
    std::string start();
    bool started() const { return started_.load(std::memory_order_acquire); }
    // `packet` is in its encoder's time base; it is consumed.
    std::string write(AVPacket *packet, Stream stream);
    std::string finish();
    // Closes and deletes the file.
    void discard();

private:
    struct Track {
        AVStream *stream = nullptr;
        AVRational codecTimeBase{}; // avformat_write_header() replaces the stream's own time base
    };

    static std::string setParameters(Track &track, const AVCodecContext *codec);
    void close();

    std::mutex mutex_;
    std::string path_;
    AVFormatContext *context_ = nullptr;
    Track video_;
    Track audio_;
    std::atomic<bool> started_ = false;
};

} // namespace blink::recorder
