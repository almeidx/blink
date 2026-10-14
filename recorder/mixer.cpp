#include "mixer.h"

#include <algorithm>
#include <cstdlib>

namespace blink::recorder {
namespace {

constexpr int64_t kNsPerSecond = 1'000'000'000;

} // namespace

Mixer::Mixer() : bus_(static_cast<size_t>(kCapacity * kChannels), 0.0F) {}

void Mixer::start(int64_t t0Ns)
{
    t0_.store(t0Ns, std::memory_order_release);
}

int64_t Mixer::position(int64_t ns) const
{
    return (ns - t0_.load(std::memory_order_acquire)) * kRate / kNsPerSecond;
}

void Mixer::write(int source, const float *samples, uint32_t frames, int64_t endNs)
{
    if (!started())
        return;
    const int64_t first = position(endNs) - frames;
    std::lock_guard lock(mutex_);
    auto &next = next_[static_cast<size_t>(source)];
    auto &synced = synced_[static_cast<size_t>(source)];
    // Wake-up jitter would otherwise turn into clicks: a source stays contiguous unless it really drifted.
    if (!synced || std::abs(first - next) > kMaxDrift)
        next = first;
    synced = true;
    const int64_t start = next;
    next += frames;
    const int64_t end = std::min(start + frames, readPosition_ + kCapacity);
    for (int64_t p = std::max(start, readPosition_); p < end;) {
        const int64_t index = p % kCapacity;
        const int64_t run = std::min(end - p, kCapacity - index);
        float *dst = bus_.data() + index * kChannels;
        const float *src = samples + (p - start) * kChannels;
        for (int64_t i = 0; i < run * kChannels; ++i)
            dst[i] += src[i];
        p += run;
    }
}

void Mixer::read(float *left, float *right, int frames)
{
    std::lock_guard lock(mutex_);
    int64_t out = 0;
    for (int64_t p = readPosition_, end = readPosition_ + frames; p < end;) {
        const int64_t index = p % kCapacity;
        const int64_t run = std::min(end - p, kCapacity - index);
        float *src = bus_.data() + index * kChannels;
        if (right) {
            for (int64_t i = 0; i < run; ++i) {
                left[out + i] = std::clamp(src[i * 2], -1.0F, 1.0F);
                right[out + i] = std::clamp(src[(i * 2) + 1], -1.0F, 1.0F);
            }
        } else {
            for (int64_t i = 0; i < run * kChannels; ++i)
                left[(out * kChannels) + i] = std::clamp(src[i], -1.0F, 1.0F);
        }
        std::fill_n(src, run * kChannels, 0.0F);
        out += run;
        p += run;
    }
    readPosition_ += frames;
}

int64_t Mixer::readPosition()
{
    std::lock_guard lock(mutex_);
    return readPosition_;
}

} // namespace blink::recorder
