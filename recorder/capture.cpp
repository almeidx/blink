#include "capture.h"

#include "events.h"
#include "framequeue.h"
#include "mixer.h"
#include "recorderprotocol.h"

#include <spa/buffer/meta.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/format-utils.h>
#include <spa/param/video/format-utils.h>
#include <spa/pod/builder.h>

#include <algorithm>
#include <charconv>
#include <cstdlib>
#include <cstring>

#include <time.h>

namespace blink::recorder {
namespace {

constexpr int64_t kNsPerSecond = 1'000'000'000;
constexpr uint32_t kBytesPerPixel = 4;

int64_t monotonicNs()
{
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (ts.tv_sec * kNsPerSecond) + ts.tv_nsec;
}

// KWin stamps frames with CLOCK_MONOTONIC. Other producers (GStreamer's pipewiresink) use their own epoch, and may
// deliver a backlog faster than real time: their stamps are not comparable with the audio, so arrival time is used.
int64_t timestamp(const spa_meta_header *header)
{
    const int64_t now = monotonicNs();
    if (header && header->pts > 0 && std::llabs(header->pts - now) < kNsPerSecond)
        return header->pts;
    return now;
}

AVPixelFormat pixelFormat(uint32_t format)
{
    switch (format) {
    case SPA_VIDEO_FORMAT_BGRx:
        return AV_PIX_FMT_BGR0;
    case SPA_VIDEO_FORMAT_BGRA:
        return AV_PIX_FMT_BGRA;
    case SPA_VIDEO_FORMAT_RGBx:
        return AV_PIX_FMT_RGB0;
    case SPA_VIDEO_FORMAT_RGBA:
        return AV_PIX_FMT_RGBA;
    default:
        return AV_PIX_FMT_NONE;
    }
}

// Raw RGB at any size, at most `fps` frames per second. No DMA-BUF modifiers are offered, so KWin sends shared
// memory, which the encoders read without a GPU round trip through EGL.
const spa_pod *videoFormat(spa_pod_builder &b, int fps)
{
    spa_pod_frame object{};
    spa_pod_frame choice{};
    spa_pod_builder_push_object(&b, &object, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_prop(&b, SPA_FORMAT_mediaType, 0);
    spa_pod_builder_id(&b, SPA_MEDIA_TYPE_video);
    spa_pod_builder_prop(&b, SPA_FORMAT_mediaSubtype, 0);
    spa_pod_builder_id(&b, SPA_MEDIA_SUBTYPE_raw);

    spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_format, 0);
    spa_pod_builder_push_choice(&b, &choice, SPA_CHOICE_Enum, 0);
    spa_pod_builder_id(&b, SPA_VIDEO_FORMAT_BGRx); // default
    for (const uint32_t format : {SPA_VIDEO_FORMAT_BGRx, SPA_VIDEO_FORMAT_BGRA, SPA_VIDEO_FORMAT_RGBx, SPA_VIDEO_FORMAT_RGBA})
        spa_pod_builder_id(&b, format);
    spa_pod_builder_pop(&b, &choice);

    spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_size, 0);
    spa_pod_builder_push_choice(&b, &choice, SPA_CHOICE_Range, 0);
    spa_pod_builder_rectangle(&b, 1920, 1080);
    spa_pod_builder_rectangle(&b, 1, 1);
    spa_pod_builder_rectangle(&b, 16384, 16384);
    spa_pod_builder_pop(&b, &choice);

    // KWin announces 0/1 (variable, damage driven) and caps the rate with maxFramerate.
    const auto rate = static_cast<uint32_t>(fps);
    spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_framerate, 0);
    spa_pod_builder_push_choice(&b, &choice, SPA_CHOICE_Range, 0);
    spa_pod_builder_fraction(&b, rate, 1);
    spa_pod_builder_fraction(&b, 0, 1);
    spa_pod_builder_fraction(&b, 1000, 1);
    spa_pod_builder_pop(&b, &choice);

    spa_pod_builder_prop(&b, SPA_FORMAT_VIDEO_maxFramerate, 0);
    spa_pod_builder_push_choice(&b, &choice, SPA_CHOICE_Range, 0);
    spa_pod_builder_fraction(&b, rate, 1);
    spa_pod_builder_fraction(&b, 0, 1);
    spa_pod_builder_fraction(&b, rate, 1);
    spa_pod_builder_pop(&b, &choice);

    return static_cast<const spa_pod *>(spa_pod_builder_pop(&b, &object));
}

const spa_pod *headerMeta(spa_pod_builder &b)
{
    spa_pod_frame object{};
    spa_pod_builder_push_object(&b, &object, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta);
    spa_pod_builder_prop(&b, SPA_PARAM_META_type, 0);
    spa_pod_builder_id(&b, SPA_META_Header);
    spa_pod_builder_prop(&b, SPA_PARAM_META_size, 0);
    spa_pod_builder_int(&b, sizeof(spa_meta_header));
    return static_cast<const spa_pod *>(spa_pod_builder_pop(&b, &object));
}

const spa_pod *sharedMemoryBuffers(spa_pod_builder &b)
{
    spa_pod_frame object{};
    spa_pod_frame choice{};
    spa_pod_builder_push_object(&b, &object, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers);
    spa_pod_builder_prop(&b, SPA_PARAM_BUFFERS_dataType, 0);
    spa_pod_builder_push_choice(&b, &choice, SPA_CHOICE_Flags, 0);
    spa_pod_builder_int(&b, (1 << SPA_DATA_MemFd) | (1 << SPA_DATA_MemPtr));
    spa_pod_builder_pop(&b, &choice);
    return static_cast<const spa_pod *>(spa_pod_builder_pop(&b, &object));
}

// What the mixer takes; PipeWire converts whatever the source produces.
const spa_pod *audioFormat(spa_pod_builder &b)
{
    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.rate = Mixer::kRate;
    info.channels = Mixer::kChannels;
    info.position[0] = SPA_AUDIO_CHANNEL_FL;
    info.position[1] = SPA_AUDIO_CHANNEL_FR;
    return spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);
}

} // namespace

Capture::Capture(Events &events, FrameQueue &frames, Mixer &mixer) : events_(events), frames_(frames), mixer_(mixer)
{
    pw_init(nullptr, nullptr);
}

Capture::~Capture()
{
    if (loop_)
        pw_thread_loop_stop(loop_);
    for (AudioSource &source : audio_) {
        if (source.stream)
            pw_stream_destroy(source.stream);
    }
    if (video_)
        pw_stream_destroy(video_);
    if (registry_)
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry_));
    if (core_)
        pw_core_disconnect(core_);
    if (context_)
        pw_context_destroy(context_);
    if (loop_)
        pw_thread_loop_destroy(loop_);
    pw_deinit();
}

std::string Capture::start(const Options &options)
{
    options_ = options;
    loop_ = pw_thread_loop_new("blink-pipewire", nullptr);
    if (!loop_)
        return "cannot create the PipeWire thread";
    context_ = pw_context_new(pw_thread_loop_get_loop(loop_), pw_properties_new(PW_KEY_APP_NAME, "Blink", nullptr), 0);
    if (!context_)
        return "cannot create the PipeWire context";
    core_ = pw_context_connect(context_, nullptr, 0);
    if (!core_)
        return "cannot connect to PipeWire";

    static constexpr pw_core_events kCoreEvents = [] {
        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        events.error = &Capture::onCoreError;
        return events;
    }();
    pw_core_add_listener(core_, &coreListener_, &kCoreEvents, this);

    static constexpr pw_registry_events kRegistryEvents = [] {
        pw_registry_events events{};
        events.version = PW_VERSION_REGISTRY_EVENTS;
        events.global = &Capture::onGlobal;
        events.global_remove = &Capture::onGlobalRemove;
        return events;
    }();
    registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
    if (!registry_)
        return "cannot list PipeWire objects";
    pw_registry_add_listener(registry_, &registryListener_, &kRegistryEvents, this);

    if (std::string error = connectVideo(options); !error.empty())
        return error;
    if (options.systemAudio) {
        AudioSource &source = audio_[static_cast<size_t>(audioSources_)];
        source.name = "system audio";
        if (connectAudio(source, true))
            ++audioSources_;
    }
    if (options.microphone) {
        AudioSource &source = audio_[static_cast<size_t>(audioSources_)];
        source.name = "microphone";
        if (connectAudio(source, false))
            ++audioSources_;
    }
    if (pw_thread_loop_start(loop_) < 0)
        return "cannot start the PipeWire thread";
    return {};
}

std::string Capture::connectVideo(const Options &options)
{
    // WirePlumber would otherwise link a missing or vanished target to whatever other video source exists.
    pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Video", PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen",
                                             "node.dont-fallback", "true", nullptr);
    if (options.bySerial)
        pw_properties_setf(props, PW_KEY_TARGET_OBJECT, "%u", options.target);
    video_ = pw_stream_new(core_, "blink-recorder", props);
    if (!video_)
        return "cannot create the screen cast stream";

    static constexpr pw_stream_events kVideoEvents = [] {
        pw_stream_events events{};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.state_changed = &Capture::onVideoState;
        events.param_changed = &Capture::onVideoParam;
        events.process = &Capture::onVideoProcess;
        return events;
    }();
    pw_stream_add_listener(video_, &videoListener_, &kVideoEvents, this);

    std::array<uint8_t, 1024> buffer{};
    spa_pod_builder b{};
    spa_pod_builder_init(&b, buffer.data(), buffer.size());
    const spa_pod *params[] = {videoFormat(b, options.fps)};
    // Without RT_PROCESS the frames are copied on the PipeWire thread loop, not on the data thread that also runs
    // the audio streams.
    const int error = pw_stream_connect(
        video_, PW_DIRECTION_INPUT, options.bySerial ? PW_ID_ANY : options.target,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_DONT_RECONNECT), params, 1);
    if (error < 0)
        return std::string("cannot connect to the screen cast: ") + strerror(-error);
    return {};
}

bool Capture::connectAudio(AudioSource &source, bool sink)
{
    source.capture = this;
    source.index = static_cast<int>(&source - audio_.data());
    pw_properties *props = pw_properties_new(PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Capture", nullptr);
    if (sink)
        pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
    source.stream = pw_stream_new(core_, sink ? "blink-recorder system audio" : "blink-recorder microphone", props);
    if (!source.stream) {
        Events::emit(kEvWarning, std::string("cannot record the ") + source.name);
        return false;
    }

    static constexpr pw_stream_events kAudioEvents = [] {
        pw_stream_events events{};
        events.version = PW_VERSION_STREAM_EVENTS;
        events.state_changed = &Capture::onAudioState;
        events.param_changed = &Capture::onAudioParam;
        events.process = &Capture::onAudioProcess;
        return events;
    }();
    pw_stream_add_listener(source.stream, &source.listener, &kAudioEvents, &source);

    std::array<uint8_t, 1024> buffer{};
    spa_pod_builder b{};
    spa_pod_builder_init(&b, buffer.data(), buffer.size());
    const spa_pod *params[] = {audioFormat(b)};
    const int error = pw_stream_connect(
        source.stream, PW_DIRECTION_INPUT, PW_ID_ANY,
        static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS), params, 1);
    if (error < 0) {
        Events::emit(kEvWarning, std::string("cannot record the ") + source.name + ": " + strerror(-error));
        pw_stream_destroy(source.stream);
        source.stream = nullptr;
        return false;
    }
    return true;
}

void Capture::stopVideo()
{
    pw_thread_loop_lock(loop_);
    videoStopped_ = true;
    pw_stream_disconnect(video_);
    pw_thread_loop_unlock(loop_);
}

void Capture::onCoreError(void *data, uint32_t id, int /*seq*/, int res, const char *message)
{
    auto *self = static_cast<Capture *>(data);
    // Errors on other objects arrive as stream errors; this one means PipeWire itself went away.
    if (id == PW_ID_CORE && res == -EPIPE && !self->videoStopped_) {
        if (self->started_)
            self->events_.sourceEnded();
        else
            self->events_.fail(std::string("PipeWire: ") + message);
    }
}

void Capture::onGlobal(void *data, uint32_t id, uint32_t /*permissions*/, const char *type, uint32_t /*version*/, const spa_dict *props)
{
    auto *self = static_cast<Capture *>(data);
    if (std::strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
        return;
    if (!self->options_.bySerial) {
        if (id == self->options_.target)
            self->targetNode_ = id;
        return;
    }
    const char *serial = props ? spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL) : nullptr;
    if (!serial)
        return;
    uint64_t value = 0;
    const char *end = serial + std::strlen(serial);
    if (std::from_chars(serial, end, value).ptr == end && value == self->options_.target)
        self->targetNode_ = id;
}

void Capture::onGlobalRemove(void *data, uint32_t id)
{
    auto *self = static_cast<Capture *>(data);
    // KWin destroys the stream when the screen cast ends (the output went away, the window closed, ...).
    if (id == self->targetNode_ && !self->videoStopped_)
        self->events_.sourceEnded();
}

void Capture::onVideoState(void *data, pw_stream_state /*old*/, pw_stream_state state, const char *error)
{
    auto *self = static_cast<Capture *>(data);
    if (self->videoStopped_)
        return;
    if (state == PW_STREAM_STATE_STREAMING) {
        self->streaming_ = true;
    } else if (state == PW_STREAM_STATE_ERROR || (state == PW_STREAM_STATE_UNCONNECTED && self->streaming_)) {
        if (self->started_)
            self->events_.sourceEnded();
        else
            self->events_.fail(std::string("the screen cast failed: ") + (error ? error : "disconnected"));
    }
}

void Capture::onVideoParam(void *data, uint32_t id, const spa_pod *param)
{
    auto *self = static_cast<Capture *>(data);
    if (!param || id != SPA_PARAM_Format)
        return;
    uint32_t mediaType = 0;
    uint32_t mediaSubtype = 0;
    if (spa_format_parse(param, &mediaType, &mediaSubtype) < 0 || mediaType != SPA_MEDIA_TYPE_video
        || mediaSubtype != SPA_MEDIA_SUBTYPE_raw)
        return;
    spa_video_info_raw format{};
    if (spa_format_video_raw_parse(param, &format) < 0)
        return;
    const AVPixelFormat pixelFormat = recorder::pixelFormat(format.format);
    // H.264 4:2:0 needs even dimensions: odd ones lose their last column/row.
    const auto width = static_cast<int>(format.size.width & ~1U);
    const auto height = static_cast<int>(format.size.height & ~1U);
    if (pixelFormat == AV_PIX_FMT_NONE || width < 2 || height < 2) {
        self->events_.fail("the screen cast has an unsupported format");
        return;
    }
    self->format_ = format;
    self->pixelFormat_ = pixelFormat;
    if (!self->pool_.init(width, height, pixelFormat)) {
        self->events_.fail("out of memory");
        return;
    }

    std::array<uint8_t, 1024> buffer{};
    spa_pod_builder b{};
    spa_pod_builder_init(&b, buffer.data(), buffer.size());
    const spa_pod *params[] = {headerMeta(b), sharedMemoryBuffers(b)};
    pw_stream_update_params(self->video_, params, 2);
}

void Capture::onVideoProcess(void *data)
{
    auto *self = static_cast<Capture *>(data);
    // Only the newest frame with content matters; older ones and cursor-only updates (empty chunks) go straight back.
    pw_buffer *latest = nullptr;
    while (pw_buffer *buffer = pw_stream_dequeue_buffer(self->video_)) {
        const spa_buffer *b = buffer->buffer;
        const bool content = b->n_datas > 0 && b->datas[0].data && b->datas[0].chunk && b->datas[0].chunk->size > 0;
        if (!content) {
            pw_stream_queue_buffer(self->video_, buffer);
            continue;
        }
        if (latest)
            pw_stream_queue_buffer(self->video_, latest);
        latest = buffer;
    }
    if (!latest)
        return;
    if (!self->videoStopped_ && self->pixelFormat_ != AV_PIX_FMT_NONE)
        self->copyFrame(latest);
    pw_stream_queue_buffer(self->video_, latest);
}

void Capture::copyFrame(pw_buffer *buffer)
{
    const spa_buffer *b = buffer->buffer;
    const spa_data &d = b->datas[0];
    if (d.chunk->flags & SPA_CHUNK_FLAG_CORRUPTED)
        return;
    const auto *header = static_cast<const spa_meta_header *>(spa_buffer_find_meta_data(b, SPA_META_Header, sizeof(spa_meta_header)));
    if (header && (header->flags & SPA_META_HEADER_FLAG_CORRUPTED))
        return;
    const int64_t ts = timestamp(header);

    AVFrame *frame = frames_.slot();
    if (!frame)
        return;
    if (!pool_.get(frame))
        return;
    const auto rowBytes = static_cast<size_t>(frame->width) * kBytesPerPixel;
    const int32_t chunkStride = d.chunk->stride;
    const size_t stride = chunkStride > 0 ? static_cast<size_t>(chunkStride) : size_t{format_.size.width} * kBytesPerPixel;
    const size_t offset = d.chunk->offset;
    const auto rows = static_cast<size_t>(frame->height);
    if (stride < rowBytes || offset > d.maxsize || (stride * (rows - 1)) + rowBytes > d.maxsize - offset) {
        av_frame_unref(frame);
        return;
    }
    const auto *src = static_cast<const uint8_t *>(d.data) + offset;
    uint8_t *dst = frame->data[0];
    const auto linesize = static_cast<size_t>(frame->linesize[0]);
    if (linesize == stride) {
        std::memcpy(dst, src, (stride * (rows - 1)) + rowBytes);
    } else {
        for (size_t y = 0; y < rows; ++y)
            std::memcpy(dst + (y * linesize), src + (y * stride), rowBytes);
    }
    frame->pts = ts;
    if (!started_) {
        started_ = true;
        mixer_.start(ts);
        events_.frameArrived();
    }
    frames_.push();
}

void Capture::onAudioState(void *data, pw_stream_state /*old*/, pw_stream_state state, const char *error)
{
    const auto *source = static_cast<AudioSource *>(data);
    if (state == PW_STREAM_STATE_ERROR)
        Events::emit(kEvWarning, std::string("the ") + source->name + " stopped: " + (error ? error : "unknown error"));
}

void Capture::onAudioParam(void *data, uint32_t id, const spa_pod *param)
{
    auto *source = static_cast<AudioSource *>(data);
    if (!param || id != SPA_PARAM_Format)
        return;
    spa_audio_info_raw info{};
    const bool usable = spa_format_audio_raw_parse(param, &info) >= 0 && info.format == SPA_AUDIO_FORMAT_F32 && info.rate == Mixer::kRate
                     && info.channels == Mixer::kChannels;
    source->usable.store(usable, std::memory_order_release);
    if (!usable)
        Events::emit(kEvWarning, std::string("the ") + source->name + " has an unsupported format");
}

void Capture::onAudioProcess(void *data)
{
    auto *source = static_cast<AudioSource *>(data);
    pw_buffer *buffer = pw_stream_dequeue_buffer(source->stream);
    if (!buffer)
        return;
    const spa_data &d = buffer->buffer->datas[0];
    if (source->usable.load(std::memory_order_acquire) && d.data && d.chunk) {
        const uint32_t offset = std::min(d.chunk->offset, d.maxsize);
        const uint32_t size = std::min(d.chunk->size, d.maxsize - offset);
        const uint32_t frames = size / (sizeof(float) * Mixer::kChannels);
        if (frames > 0) {
            const auto *samples = reinterpret_cast<const float *>(static_cast<const uint8_t *>(d.data) + offset);
            source->capture->mixer_.write(source->index, samples, frames, monotonicNs());
        }
    }
    pw_stream_queue_buffer(source->stream, buffer);
}

} // namespace blink::recorder
