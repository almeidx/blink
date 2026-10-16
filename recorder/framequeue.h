#pragma once

// Hands captured frames from the PipeWire thread to the video encoder thread. Bounded: when the encoder falls
// behind, new frames are dropped (and counted) instead of blocking the capture.

extern "C" {
#include <libavutil/frame.h>
}

#include <array>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>

namespace blink::recorder {

class FrameQueue {
public:
    FrameQueue();
    ~FrameQueue();
    FrameQueue(const FrameQueue &) = delete;
    FrameQueue &operator=(const FrameQueue &) = delete;

    // The negotiated stream: the size and pixel format of the frames that follow.
    struct Format {
        int width = 0;
        int height = 0;
        AVPixelFormat pixelFormat = AV_PIX_FMT_NONE;
    };
    // Producer, as soon as PipeWire negotiated a format; only the latest one not yet popped is kept.
    void setFormat(const Format &format);

    // Producer: an empty frame to fill, or nullptr when the queue is full. A filled frame is published with push();
    // one that is abandoned must be unreferenced.
    AVFrame *slot();
    void push();

    enum class Pop { Frame, Format, Finished, Cancelled };
    // Consumer: waits for a format or a frame. Format (into `format`) comes before any frame; a frame is moved into
    // `frame`. Finished once finish() was called and the queue is drained; Cancelled as soon as cancel() was called.
    Pop pop(AVFrame *frame, Format &format);

    void finish();
    void cancel();
    uint64_t dropped();

private:
    static constexpr size_t kCapacity = 3;

    std::mutex mutex_;
    std::condition_variable wakeup_;
    std::array<AVFrame *, kCapacity> slots_{};
    size_t head_ = 0;
    size_t count_ = 0;
    std::optional<Format> format_;
    uint64_t dropped_ = 0;
    bool finished_ = false;
    bool cancelled_ = false;
};

} // namespace blink::recorder
