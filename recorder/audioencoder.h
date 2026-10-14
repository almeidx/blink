#pragma once

// Encodes the mixed audio (AAC, or Opus where FFmpeg lacks its native AAC encoder) on its own thread.

#include "ffmpeg.h"

#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

struct AVStream;

namespace blink::recorder {

class Events;
class Mixer;
class Muxer;

class AudioEncoder {
public:
    AudioEncoder(Mixer &mixer, Muxer &muxer, Events &events);
    ~AudioEncoder();
    AudioEncoder(const AudioEncoder &) = delete;
    AudioEncoder &operator=(const AudioEncoder &) = delete;

    // Opens the encoder and adds the audio stream to the file; returns an error message on failure.
    std::string open();
    void start();
    // Encodes everything up to `stopNs` (CLOCK_MONOTONIC), padding with silence, then flushes.
    void finish(int64_t stopNs);
    void cancel();
    void join();

private:
    void run();
    bool encode(int frames);
    bool drain();

    Mixer &mixer_;
    Muxer &muxer_;
    Events &events_;
    CodecContextPtr codec_;
    AVStream *stream_ = nullptr;
    FramePtr frame_;
    PacketPtr packet_;
    bool planar_ = true;
    bool smallLastFrame_ = false;

    std::mutex mutex_;
    std::condition_variable wakeup_;
    int64_t stopNs_ = INT64_MAX;
    bool cancelled_ = false;

    std::thread thread_;
};

} // namespace blink::recorder
