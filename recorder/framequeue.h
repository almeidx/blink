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

namespace blink::recorder {

class FrameQueue {
public:
    FrameQueue();
    ~FrameQueue();
    FrameQueue(const FrameQueue &) = delete;
    FrameQueue &operator=(const FrameQueue &) = delete;

    // Producer: an empty frame to fill, or nullptr when the queue is full. A filled frame is published with push();
    // one that is abandoned must be unreferenced.
    AVFrame *slot();
    void push();

    enum class Pop { Frame, Finished, Cancelled };
    // Consumer: waits for a frame and moves it into `frame`. Finished once finish() was called and the queue is
    // drained; Cancelled as soon as cancel() was called.
    Pop pop(AVFrame *frame);

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
    uint64_t dropped_ = 0;
    bool finished_ = false;
    bool cancelled_ = false;
};

} // namespace blink::recorder
