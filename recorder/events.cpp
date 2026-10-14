#include "events.h"

#include <cstdint>
#include <cstdio>

#include <sys/eventfd.h>
#include <unistd.h>

namespace blink::recorder {

Events::Events() : fd_(eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK)) {}

Events::~Events()
{
    if (fd_ >= 0)
        close(fd_);
}

void Events::emit(std::string_view event, std::string_view text)
{
    flockfile(stdout);
    fwrite_unlocked(event.data(), 1, event.size(), stdout);
    if (!text.empty()) {
        putc_unlocked(' ', stdout);
        fwrite_unlocked(text.data(), 1, text.size(), stdout);
    }
    putc_unlocked('\n', stdout);
    fflush_unlocked(stdout);
    funlockfile(stdout);
}

void Events::fail(std::string_view message)
{
    {
        std::lock_guard lock(mutex_);
        if (failed_)
            return;
        failed_ = true;
        error_ = message;
    }
    wake();
}

void Events::sourceEnded()
{
    ended_.store(true, std::memory_order_release);
    wake();
}

void Events::wake() const
{
    const uint64_t one = 1;
    [[maybe_unused]] const ssize_t written = write(fd_, &one, sizeof(one));
}

void Events::clear() const
{
    uint64_t count = 0;
    [[maybe_unused]] const ssize_t read = ::read(fd_, &count, sizeof(count));
}

bool Events::failed(std::string &message)
{
    std::lock_guard lock(mutex_);
    if (failed_)
        message = error_;
    return failed_;
}

} // namespace blink::recorder
