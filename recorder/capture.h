#pragma once

// PipeWire input: KWin's screen cast stream (raw RGB over shared memory) and the optional audio sources.
// Video frames are copied into pooled AVFrames for the encoder thread and the PipeWire buffer goes straight back;
// audio is written into the mixer.

#include "ffmpeg.h"

#include <pipewire/pipewire.h>
#include <spa/buffer/meta.h>
#include <spa/param/video/raw.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <string>

namespace blink::recorder {

class Events;
class FrameQueue;
class Mixer;

class Capture {
public:
    struct Options {
        uint64_t target = 0;
        bool bySerial = true; // `target` is an object serial (64-bit), else a node id (32-bit)
        int fps = 60;
        bool systemAudio = false;
        bool microphone = false;
    };

    Capture(Events &events, FrameQueue &frames, Mixer &mixer);
    ~Capture();
    Capture(const Capture &) = delete;
    Capture &operator=(const Capture &) = delete;

    // Connects the streams and starts PipeWire's thread; returns an error message on failure. Audio sources that
    // cannot connect only produce a warning.
    std::string start(const Options &options);
    int audioSources() const { return audioSources_; }
    // No video frames after this returns.
    void stopVideo();

private:
    struct AudioSource {
        Capture *capture = nullptr;
        pw_stream *stream = nullptr;
        spa_hook listener{};
        int index = 0;
        const char *name = "";
        std::atomic<bool> usable = false; // the negotiated format is what the mixer takes; read on the data thread
    };

    std::string connectVideo(const Options &options);
    bool connectAudio(AudioSource &source, bool sink);

    static void onCoreError(void *data, uint32_t id, int seq, int res, const char *message);
    static void onGlobal(void *data, uint32_t id, uint32_t permissions, const char *type, uint32_t version, const spa_dict *props);
    static void onGlobalRemove(void *data, uint32_t id);
    static void onVideoState(void *data, pw_stream_state old, pw_stream_state state, const char *error);
    static void onVideoParam(void *data, uint32_t id, const spa_pod *param);
    static void onVideoProcess(void *data);
    static void onAudioState(void *data, pw_stream_state old, pw_stream_state state, const char *error);
    static void onAudioParam(void *data, uint32_t id, const spa_pod *param);
    static void onAudioProcess(void *data);

    void copyFrame(pw_buffer *buffer);

    Events &events_;
    FrameQueue &frames_;
    Mixer &mixer_;

    pw_thread_loop *loop_ = nullptr;
    pw_context *context_ = nullptr;
    pw_core *core_ = nullptr;
    spa_hook coreListener_{};
    pw_registry *registry_ = nullptr;
    spa_hook registryListener_{};

    // Everything below is only touched on the PipeWire thread (or under its lock).
    pw_stream *video_ = nullptr;
    spa_hook videoListener_{};
    Options options_;
    uint32_t targetNode_ = SPA_ID_INVALID;
    bool videoStopped_ = false;
    bool streaming_ = false;
    spa_video_info_raw format_{};
    AVPixelFormat pixelFormat_ = AV_PIX_FMT_NONE;
    FramePool pool_; // frames at the stream size rounded down to even (H.264 4:2:0)
    bool started_ = false;

    std::array<AudioSource, 2> audio_{};
    int audioSources_ = 0;
};

} // namespace blink::recorder
