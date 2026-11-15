#pragma once

#include <cstdint>

#include <time.h>

namespace blink::recorder {

inline constexpr int64_t kNsPerSecond = 1'000'000'000;

// CLOCK_MONOTONIC, the clock of every timestamp in the recorder (KWin stamps its frames with it too).
inline int64_t monotonicNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * kNsPerSecond) + ts.tv_nsec;
}

} // namespace blink::recorder
