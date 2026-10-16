#include "videoencoder.h"

#include "events.h"
#include "framequeue.h"
#include "muxer.h"
#include "recorderprotocol.h"

extern "C" {
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavutil/hwcontext.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
}

#include <algorithm>
#include <array>
#include <cstdio>
#include <span>
#include <vector>

#include <dirent.h>

namespace blink::recorder {
namespace {

constexpr AVRational kTimeBase{1, 1'000'000};
constexpr int64_t kNsPerTick = 1'000'000'000 / kTimeBase.den;
// KWin only sends damaged frames, so a GOP counted in frames can last minutes on a still screen: seeking needs a
// keyframe at least this often in video time (2 s).
constexpr int64_t kKeyframeInterval = 2 * kTimeBase.den;

int pick(Quality quality, int low, int medium, int high)
{
    switch (quality) {
    case Quality::Low:
        return low;
    case Quality::Medium:
        return medium;
    case Quality::High:
        break;
    }
    return high;
}

bool isRgb(AVPixelFormat format)
{
    return (av_pix_fmt_desc_get(format)->flags & AV_PIX_FMT_FLAG_RGB) != 0;
}

// VA-API render nodes, in name order, for when the default device cannot encode (e.g. it is NVIDIA's).
std::vector<std::string> renderNodes()
{
    std::vector<std::string> nodes;
    DIR *dir = opendir("/dev/dri");
    if (!dir)
        return nodes;
    while (const dirent *entry = readdir(dir)) {
        if (std::string_view(entry->d_name).starts_with("renderD"))
            nodes.push_back(std::string("/dev/dri/") + entry->d_name);
    }
    closedir(dir);
    std::ranges::sort(nodes);
    return nodes;
}

} // namespace

VideoEncoder::VideoEncoder(const Options &options, FrameQueue &queue, Muxer &muxer, Events &events)
    : options_(options)
    , queue_(queue)
    , muxer_(muxer)
    , events_(events)
    , packet_(av_packet_alloc())
    , last_(av_frame_alloc())
    , converted_(av_frame_alloc())
    , adapted_(av_frame_alloc())
{
}

VideoEncoder::~VideoEncoder()
{
    join();
}

void VideoEncoder::start()
{
    thread_ = std::thread([this] { run(); });
}

void VideoEncoder::finish(int64_t stopNs)
{
    stopNs_ = stopNs; // published to the thread by the queue's mutex
    queue_.finish();
    join();
}

void VideoEncoder::join()
{
    if (thread_.joinable())
        thread_.join();
}

void VideoEncoder::run()
{
    const FramePtr frame(av_frame_alloc());
    if (!frame || !packet_ || !last_ || !converted_ || !adapted_) {
        events_.fail("out of memory");
        return;
    }
    // A CUDA context takes ~150 ms to create. Made while PipeWire negotiates the stream, it is not on the path of the
    // first frame, during which later frames are dropped. Fails quickly without NVIDIA's driver.
    if (options_.family == EncoderFamily::Auto || options_.family == EncoderFamily::Nvenc) {
        AVBufferRef *cuda = nullptr;
        if (av_hwdevice_ctx_create(&cuda, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0) >= 0)
            cuda_.reset(cuda);
    }
    for (;;) {
        const FrameQueue::Pop result = queue_.pop(frame.get());
        if (result == FrameQueue::Pop::Cancelled)
            return;
        if (result == FrameQueue::Pop::Finished)
            break;
        if (!codec_) {
            if (!open(frame.get()))
                return;
            t0_ = frame->pts;
            inputWidth_ = frame->width;
            inputHeight_ = frame->height;
            inputFormat_ = static_cast<AVPixelFormat>(frame->format);
        }
        if (!encode(frame.get()))
            return;
    }
    if (!codec_)
        return;
    // KWin only sends damaged frames: without this, a still screen at the end would be cut from the video.
    if ((stopNs_ - t0_) / kNsPerTick - lastPts_ >= kTimeBase.den / options_.fps && last_->buf[0]) {
        last_->pts = stopNs_;
        if (!encode(last_.get()))
            return;
    }
    if (graph_) {
        if (av_buffersrc_add_frame_flags(source_, nullptr, 0) < 0 || !send(nullptr))
            return;
    }
    const int error = avcodec_send_frame(codec_.get(), nullptr);
    if (error < 0) {
        events_.fail("cannot flush the video encoder: " + avError(error));
        return;
    }
    drain();
}

bool VideoEncoder::open(const AVFrame *frame)
{
    std::span<const Kind> kinds;
    static constexpr std::array kCandidates{Kind::Nvenc, Kind::Vaapi, Kind::X264, Kind::OpenH264};
    switch (options_.family) {
    case EncoderFamily::Auto:
        kinds = kCandidates;
        break;
    case EncoderFamily::Nvenc:
        kinds = std::span(kCandidates).subspan(0, 1);
        break;
    case EncoderFamily::Vaapi:
        kinds = std::span(kCandidates).subspan(1, 1);
        break;
    case EncoderFamily::Software:
        kinds = std::span(kCandidates).subspan(2);
        break;
    }
    std::string errors;
    for (const Kind kind : kinds) {
        const std::string error = tryOpen(kind, frame);
        if (error.empty()) {
            if (std::string message = muxer_.setParameters(muxer_.videoStream(), codec_.get()); !message.empty()) {
                events_.fail(message);
                return false;
            }
            if (std::string message = muxer_.start(); !message.empty()) {
                events_.fail(message);
                return false;
            }
            Events::emit(kEvEncoder,
                         std::string(codec_->codec->name) + ' ' + std::to_string(codec_->width) + 'x' + std::to_string(codec_->height));
            return true;
        }
        reset();
        errors += errors.empty() ? "" : "; ";
        errors += error;
    }
    events_.fail("no usable H.264 encoder (" + errors + ")");
    return false;
}

std::string VideoEncoder::tryOpen(Kind kind, const AVFrame *frame)
{
    const int fps = options_.fps;
    AVDictionary *options = nullptr;
    switch (kind) {
    case Kind::Nvenc:
        av_dict_set(&options, "preset", "p5", 0);
        av_dict_set(&options, "tune", "hq", 0);
        av_dict_set(&options, "rc", "vbr", 0);
        av_dict_set_int(&options, "cq", pick(options_.quality, 31, 26, 21), 0);
        av_dict_set(&options, "profile", "high", 0);
        // With B-frames NVENC allocates many more input surfaces: ~1 GB more memory at 4K, and more latency.
        av_dict_set_int(&options, "bf", 0, 0);
        // Without it the I-frames encode() asks for are intra but not IDR, which the MP4 muxer cannot seek to.
        av_dict_set_int(&options, "forced-idr", 1, 0);
        return openCodec("h264_nvenc", frame, static_cast<AVPixelFormat>(frame->format), &options, cuda_.get());
    case Kind::Vaapi:
        return openVaapi(frame);
    case Kind::X264:
        av_dict_set(&options, "preset", "veryfast", 0);
        av_dict_set_int(&options, "crf", pick(options_.quality, 28, 23, 18), 0);
        av_dict_set_int(&options, "forced-idr", 1, 0);
        if (std::string error = openCodec("libx264", frame, AV_PIX_FMT_YUV420P, &options); !error.empty())
            return error;
        return openSoftware("libx264", frame);
    case Kind::OpenH264: {
        const double bitsPerPixel = pick(options_.quality, 6, 10, 15) / 100.0;
        av_dict_set(&options, "rc_mode", "bitrate", 0);
        av_dict_set_int(&options, "b", static_cast<int64_t>(frame->width * frame->height * fps * bitsPerPixel), 0);
        if (std::string error = openCodec("libopenh264", frame, AV_PIX_FMT_YUV420P, &options); !error.empty())
            return error;
        return openSoftware("libopenh264", frame);
    }
    }
    return {};
}

std::string VideoEncoder::openCodec(const char *name, const AVFrame *frame, AVPixelFormat format, AVDictionary **options,
                                    AVBufferRef *device)
{
    const std::string prefix = std::string(name) + ": ";
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    if (!codec) {
        av_dict_free(options);
        return prefix + "not available";
    }
    codec_.reset(avcodec_alloc_context3(codec));
    if (!codec_) {
        av_dict_free(options);
        return prefix + "out of memory";
    }
    if (format != AV_PIX_FMT_VAAPI) {
        const void *configs = nullptr;
        int count = 0;
        avcodec_get_supported_config(codec_.get(), codec, AV_CODEC_CONFIG_PIX_FORMAT, 0, &configs, &count);
        const std::span formats(static_cast<const AVPixelFormat *>(configs), configs ? static_cast<size_t>(count) : 0);
        if (!formats.empty() && std::ranges::find(formats, format) == formats.end()) {
            av_dict_free(options);
            return prefix + "does not take " + av_get_pix_fmt_name(format);
        }
    }
    codec_->width = frame->width;
    codec_->height = frame->height;
    codec_->pix_fmt = format;
    codec_->time_base = kTimeBase;
    codec_->framerate = {options_.fps, 1};
    codec_->gop_size = 2 * options_.fps; // upper bound for a busy screen; encode() also forces kKeyframeInterval
    codec_->bit_rate = 0; // the rate control is quality based; libopenh264 gets "b" in the options
    codec_->color_primaries = AVCOL_PRI_BT709;
    codec_->color_trc = AVCOL_TRC_BT709;
    // NVENC converts RGB input with the BT.601 matrix (and says so in the bitstream); the container must agree.
    codec_->colorspace = isRgb(format) ? AVCOL_SPC_BT470BG : AVCOL_SPC_BT709;
    codec_->color_range = AVCOL_RANGE_MPEG;
    if (muxer_.globalHeader())
        codec_->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    if (sink_) {
        codec_->hw_frames_ctx = av_buffer_ref(av_buffersink_get_hw_frames_ctx(sink_));
        if (!codec_->hw_frames_ctx) {
            av_dict_free(options);
            return prefix + "no VA-API frames";
        }
    }
    if (device)
        codec_->hw_device_ctx = av_buffer_ref(device);
    const int error = avcodec_open2(codec_.get(), codec, options);
    av_dict_free(options);
    if (error < 0)
        return prefix + avError(error);
    return {};
}

std::string VideoEncoder::openVaapi(const AVFrame *frame)
{
    std::string errors = openVaapiDevice(nullptr, frame);
    if (errors.empty())
        return {};
    for (const std::string &node : renderNodes()) {
        reset();
        const std::string error = openVaapiDevice(node.c_str(), frame);
        if (error.empty())
            return {};
        errors += "; " + error;
    }
    return errors;
}

std::string VideoEncoder::openVaapiDevice(const char *device, const AVFrame *frame)
{
    const std::string prefix = std::string("h264_vaapi on ") + (device ? device : "the default device") + ": ";
    AVBufferRef *created = nullptr;
    int error = av_hwdevice_ctx_create(&created, AV_HWDEVICE_TYPE_VAAPI, device, nullptr, 0);
    if (error < 0)
        return prefix + avError(error);
    device_.reset(created);

    graph_.reset(avfilter_graph_alloc());
    if (!graph_)
        return prefix + "out of memory";
    std::array<char, 160> args{};
    std::snprintf(args.data(), args.size(), "video_size=%dx%d:pix_fmt=%d:time_base=%d/%d:pixel_aspect=1/1", frame->width, frame->height,
                  frame->format, kTimeBase.num, kTimeBase.den);
    error = avfilter_graph_create_filter(&source_, avfilter_get_by_name("buffer"), "in", args.data(), nullptr, graph_.get());
    AVFilterContext *upload = nullptr;
    if (error >= 0) {
        upload = avfilter_graph_alloc_filter(graph_.get(), avfilter_get_by_name("hwupload"), "upload");
        error = upload ? 0 : AVERROR(ENOMEM);
    }
    if (error >= 0) {
        upload->hw_device_ctx = av_buffer_ref(device_.get());
        error = avfilter_init_str(upload, nullptr);
    }
    AVFilterContext *scale = nullptr;
    if (error >= 0) {
        error = avfilter_graph_create_filter(
            &scale, avfilter_get_by_name("scale_vaapi"), "scale",
            "format=nv12:out_color_matrix=bt709:out_range=tv:out_color_primaries=bt709:out_color_transfer=bt709", nullptr, graph_.get());
    }
    AVFilterContext *sink = nullptr;
    if (error >= 0)
        error = avfilter_graph_create_filter(&sink, avfilter_get_by_name("buffersink"), "out", nullptr, nullptr, graph_.get());
    if (error >= 0)
        error = avfilter_link(source_, 0, upload, 0);
    if (error >= 0)
        error = avfilter_link(upload, 0, scale, 0);
    if (error >= 0)
        error = avfilter_link(scale, 0, sink, 0);
    if (error >= 0)
        error = avfilter_graph_config(graph_.get(), nullptr);
    if (error < 0)
        return prefix + "filter graph: " + avError(error);
    sink_ = sink;

    AVDictionary *options = nullptr;
    av_dict_set(&options, "rc_mode", "CQP", 0);
    av_dict_set_int(&options, "qp", pick(options_.quality, 30, 25, 20), 0);
    av_dict_set(&options, "profile", "high", 0);
    if (std::string message = openCodec("h264_vaapi", frame, AV_PIX_FMT_VAAPI, &options); !message.empty())
        return prefix + message;
    return {};
}

std::string VideoEncoder::openSoftware(const char *name, const AVFrame *frame)
{
    const std::string prefix = std::string(name) + ": ";
    sws_.reset(sws_alloc_context());
    if (!sws_)
        return prefix + "out of memory";
    av_opt_set_int(sws_.get(), "srcw", frame->width, 0);
    av_opt_set_int(sws_.get(), "srch", frame->height, 0);
    av_opt_set_int(sws_.get(), "src_format", frame->format, 0);
    av_opt_set_int(sws_.get(), "dstw", frame->width, 0);
    av_opt_set_int(sws_.get(), "dsth", frame->height, 0);
    av_opt_set_int(sws_.get(), "dst_format", AV_PIX_FMT_YUV420P, 0);
    av_opt_set_int(sws_.get(), "sws_flags", SWS_FAST_BILINEAR, 0);
    av_opt_set_int(sws_.get(), "threads", 0, 0); // automatic; sws_scale_frame() slices the conversion across them
    int error = sws_init_context(sws_.get(), nullptr, nullptr);
    if (error < 0)
        return prefix + "swscale: " + avError(error);
    const int *bt709 = sws_getCoefficients(SWS_CS_ITU709);
    // Full-range RGB in, limited-range BT.709 out. 1 << 16 is neutral brightness/contrast/saturation.
    error = sws_setColorspaceDetails(sws_.get(), bt709, 1, bt709, 0, 0, 1 << 16, 1 << 16);
    if (error < 0)
        return prefix + "swscale cannot convert to BT.709";
    if (!pool_.init(frame->width, frame->height, AV_PIX_FMT_YUV420P))
        return prefix + "out of memory";
    return {};
}

void VideoEncoder::reset()
{
    codec_.reset();
    sws_.reset();
    sink_ = nullptr;
    source_ = nullptr;
    graph_.reset();
    device_.reset();
}

bool VideoEncoder::encode(AVFrame *frame)
{
    const int64_t pts = (frame->pts - t0_) / kNsPerTick;
    if (pts <= lastPts_) {
        av_frame_unref(frame);
        return true;
    }
    lastPts_ = pts;
    frame->pts = pts;
    // Set on every frame, not only on keyframes: last_ is encoded again at the stop time and must not keep a stale type.
    if (lastKeyPts_ < 0 || pts - lastKeyPts_ >= kKeyframeInterval) {
        frame->pict_type = AV_PICTURE_TYPE_I;
        frame->flags |= AV_FRAME_FLAG_KEY;
        lastKeyPts_ = pts;
    } else {
        frame->pict_type = AV_PICTURE_TYPE_NONE;
        frame->flags &= ~AV_FRAME_FLAG_KEY;
    }
    frame->duration = kTimeBase.den / options_.fps;
    AVFrame *input = frame;
    if (frame->width != inputWidth_ || frame->height != inputHeight_ || frame->format != inputFormat_) {
        // The stream was renegotiated to another size or format; the encoder cannot follow, so the frame is
        // converted to what it was opened with.
        adapt_.reset(sws_getCachedContext(adapt_.release(), frame->width, frame->height, static_cast<AVPixelFormat>(frame->format),
                                          inputWidth_, inputHeight_, inputFormat_, SWS_BILINEAR, nullptr, nullptr, nullptr));
        if (!adapt_
            || (!adaptPool_.matches(inputWidth_, inputHeight_, inputFormat_) && !adaptPool_.init(inputWidth_, inputHeight_, inputFormat_))
            || !adapted_) {
            events_.fail("cannot convert the renegotiated screen cast");
            return false;
        }
        av_frame_unref(adapted_.get());
        int error = adaptPool_.get(adapted_.get()) ? 0 : AVERROR(ENOMEM);
        if (error >= 0)
            error = av_frame_copy_props(adapted_.get(), frame);
        if (error >= 0)
            error = sws_scale_frame(adapt_.get(), adapted_.get(), frame);
        if (error < 0) {
            events_.fail("cannot convert the renegotiated screen cast: " + avError(error));
            return false;
        }
        av_frame_unref(frame);
        input = adapted_.get();
    }

    bool sent = false;
    if (graph_) {
        const int error = av_buffersrc_add_frame_flags(source_, input, AV_BUFFERSRC_FLAG_KEEP_REF);
        if (error < 0) {
            events_.fail("cannot upload the frame: " + avError(error));
            return false;
        }
        sent = send(nullptr);
    } else if (sws_) {
        av_frame_unref(converted_.get());
        int error = pool_.get(converted_.get()) ? 0 : AVERROR(ENOMEM);
        if (error >= 0)
            error = av_frame_copy_props(converted_.get(), input);
        if (error >= 0) {
            converted_->colorspace = AVCOL_SPC_BT709;
            converted_->color_range = AVCOL_RANGE_MPEG;
            error = sws_scale_frame(sws_.get(), converted_.get(), input);
        }
        if (error < 0) {
            events_.fail("cannot convert the frame: " + avError(error));
            return false;
        }
        sent = send(converted_.get());
    } else {
        sent = send(input);
    }
    if (!sent)
        return false;
    if (input != last_.get()) {
        av_frame_unref(last_.get());
        av_frame_move_ref(last_.get(), input);
    }
    if (!recording_) {
        recording_ = true;
        Events::emit(kEvRecording);
    }
    return true;
}

// Sends `frame` to the encoder, or with nullptr everything the filter graph has ready.
bool VideoEncoder::send(AVFrame *frame)
{
    if (!frame) {
        for (;;) {
            int error = av_buffersink_get_frame(sink_, converted_.get());
            if (error == AVERROR(EAGAIN) || error == AVERROR_EOF)
                return true;
            if (error >= 0) {
                error = avcodec_send_frame(codec_.get(), converted_.get());
                av_frame_unref(converted_.get());
            }
            if (error < 0) {
                events_.fail("cannot encode video: " + avError(error));
                return false;
            }
            if (!drain())
                return false;
        }
    }
    const int error = avcodec_send_frame(codec_.get(), frame);
    if (error < 0) {
        events_.fail("cannot encode video: " + avError(error));
        return false;
    }
    return drain();
}

bool VideoEncoder::drain()
{
    for (;;) {
        const int error = avcodec_receive_packet(codec_.get(), packet_.get());
        if (error == AVERROR(EAGAIN) || error == AVERROR_EOF)
            return true;
        if (error < 0) {
            events_.fail("cannot encode video: " + avError(error));
            return false;
        }
        if (std::string message = muxer_.write(packet_.get(), muxer_.videoStream(), codec_->time_base); !message.empty()) {
            events_.fail(message);
            return false;
        }
    }
}

} // namespace blink::recorder
