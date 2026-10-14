#include "framequeue.h"

namespace blink::recorder {

FrameQueue::FrameQueue()
{
    for (AVFrame *&frame : slots_)
        frame = av_frame_alloc();
}

FrameQueue::~FrameQueue()
{
    for (AVFrame *&frame : slots_)
        av_frame_free(&frame);
}

AVFrame *FrameQueue::slot()
{
    std::lock_guard lock(mutex_);
    if (count_ == kCapacity || finished_ || cancelled_) {
        ++dropped_;
        return nullptr;
    }
    return slots_[(head_ + count_) % kCapacity];
}

void FrameQueue::push()
{
    {
        std::lock_guard lock(mutex_);
        ++count_;
    }
    wakeup_.notify_one();
}

FrameQueue::Pop FrameQueue::pop(AVFrame *frame)
{
    std::unique_lock lock(mutex_);
    wakeup_.wait(lock, [this] { return count_ > 0 || finished_ || cancelled_; });
    if (cancelled_)
        return Pop::Cancelled;
    if (count_ == 0)
        return Pop::Finished;
    av_frame_move_ref(frame, slots_[head_]);
    head_ = (head_ + 1) % kCapacity;
    --count_;
    return Pop::Frame;
}

void FrameQueue::finish()
{
    {
        std::lock_guard lock(mutex_);
        finished_ = true;
    }
    wakeup_.notify_one();
}

void FrameQueue::cancel()
{
    {
        std::lock_guard lock(mutex_);
        cancelled_ = true;
    }
    wakeup_.notify_one();
}

uint64_t FrameQueue::dropped()
{
    std::lock_guard lock(mutex_);
    return dropped_;
}

} // namespace blink::recorder
