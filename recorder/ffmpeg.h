#pragma once

// Ownership wrappers and small helpers for the FFmpeg C API.

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavutil/buffer.h>
#include <libavutil/frame.h>
#include <libswscale/swscale.h>
}

#include <memory>
#include <string>

namespace blink::recorder {

struct FrameDeleter {
    void operator()(AVFrame *frame) const { av_frame_free(&frame); }
};
struct PacketDeleter {
    void operator()(AVPacket *packet) const { av_packet_free(&packet); }
};
struct CodecContextDeleter {
    void operator()(AVCodecContext *context) const { avcodec_free_context(&context); }
};
struct BufferDeleter {
    void operator()(AVBufferRef *buffer) const { av_buffer_unref(&buffer); }
};
struct BufferPoolDeleter {
    void operator()(AVBufferPool *pool) const { av_buffer_pool_uninit(&pool); }
};
struct FilterGraphDeleter {
    void operator()(AVFilterGraph *graph) const { avfilter_graph_free(&graph); }
};
struct SwsDeleter {
    void operator()(SwsContext *context) const { sws_freeContext(context); }
};

using FramePtr = std::unique_ptr<AVFrame, FrameDeleter>;
using PacketPtr = std::unique_ptr<AVPacket, PacketDeleter>;
using CodecContextPtr = std::unique_ptr<AVCodecContext, CodecContextDeleter>;
using BufferPtr = std::unique_ptr<AVBufferRef, BufferDeleter>;
using BufferPoolPtr = std::unique_ptr<AVBufferPool, BufferPoolDeleter>;
using FilterGraphPtr = std::unique_ptr<AVFilterGraph, FilterGraphDeleter>;
using SwsPtr = std::unique_ptr<SwsContext, SwsDeleter>;

std::string avError(int error);

// Recycles the pixel buffers of same-sized frames, so steady-state capture and conversion do not allocate.
// Buffers stay valid while anything (an encoder, a filter) holds a reference; they return to the pool afterwards.
class FramePool {
public:
    bool init(int width, int height, AVPixelFormat format);
    bool matches(int width, int height, AVPixelFormat format) const;
    // Points `frame` (which must hold no buffers) at a pooled buffer.
    bool get(AVFrame *frame) const;

private:
    BufferPoolPtr pool_;
    int width_ = 0;
    int height_ = 0;
    AVPixelFormat format_ = AV_PIX_FMT_NONE;
    int linesize_[4] = {};
    ptrdiff_t offset_[4] = {};
    int planes_ = 0;
};

} // namespace blink::recorder
