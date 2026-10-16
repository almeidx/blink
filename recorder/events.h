#pragma once

// stdout events for the tray app, and the conditions that wake main()'s control loop.

#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

namespace blink::recorder {

class Events {
public:
    Events();
    ~Events();
    Events(const Events &) = delete;
    Events &operator=(const Events &) = delete;

    // One line on stdout, "<event>" or "<event> <text>". Thread-safe.
    static void emit(std::string_view event, std::string_view text = {});

    // Any thread. The first fatal error wins.
    void fail(std::string_view message);
    // Any thread: the compositor closed the stream; the recording finishes normally.
    void sourceEnded();
    void frameArrived() { frameArrived_.store(true, std::memory_order_relaxed); }

    // main(): readable when one of the above happened.
    int fd() const { return fd_; }
    bool valid() const { return fd_ >= 0; } // eventfd() succeeded; without it poll() never wakes for fail()/sourceEnded()
    void clear() const;
    bool failed(std::string &message);
    bool ended() const { return ended_.load(std::memory_order_acquire); }
    bool hasFrames() const { return frameArrived_.load(std::memory_order_relaxed); }

private:
    void wake() const;

    int fd_ = -1;
    std::mutex mutex_;
    std::string error_;
    bool failed_ = false;
    std::atomic<bool> ended_ = false;
    std::atomic<bool> frameArrived_ = false;
};

} // namespace blink::recorder
