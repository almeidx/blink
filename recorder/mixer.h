#pragma once

// Mixes the audio sources (system audio, microphone) onto one timeline that starts at the first video frame.
// Sources write from PipeWire's data thread as their buffers arrive; the audio encoder reads a fixed delay behind
// real time, so a source whose buffers come late still lands in the right place. Samples are interleaved stereo F32.

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>
#include <vector>

namespace blink::recorder {

class Mixer {
public:
    static constexpr int kRate = 48000;
    static constexpr int kChannels = 2;
    static constexpr int kMaxSources = 2;
    // How far behind real time the encoder reads; sources arriving later than this lose the late part.
    static constexpr int64_t kLatencyNs = 170'000'000;

    Mixer();

    // CLOCK_MONOTONIC time of sample 0 (the first video frame). Writes before it are dropped.
    void start(int64_t t0Ns);
    bool started() const { return t0_.load(std::memory_order_acquire) != kUnset; }
    // Sample position of a CLOCK_MONOTONIC time; requires started().
    int64_t position(int64_t ns) const;

    // `frames` samples that ended at `endNs`.
    void write(int source, const float *samples, uint32_t frames, int64_t endNs);

    // The next `frames` mixed samples, clamped to [-1, 1], planar (left, right) or interleaved (into left).
    void read(float *left, float *right, int frames);
    int64_t readPosition();

private:
    static constexpr int64_t kUnset = INT64_MIN;
    static constexpr int64_t kCapacity = 4 * kRate; // also covers the encoder start-up before the file header exists
    static constexpr int64_t kMaxDrift = kRate / 50;

    std::atomic<int64_t> t0_ = kUnset;
    std::mutex mutex_;
    std::vector<float> bus_; // ring of `capacity` frames, indexed by position % capacity
    int64_t readPosition_ = 0;
    std::array<int64_t, kMaxSources> next_{}; // where each source's next buffer goes when it stays contiguous
    std::array<bool, kMaxSources> synced_{};
};

} // namespace blink::recorder
