#pragma once

// Encodes the captured frames to H.264 on its own thread. The encoder is picked when the first frame arrives (its
// size and pixel format are known then): NVENC takes the RGB frames as they are, VA-API gets them through a
// hwupload/scale_vaapi filter graph, the software encoders through swscale.

#include "ffmpeg.h"
#include "recorderprotocol.h"

#include <cstdint>
#include <string>
#include <thread>

namespace blink::recorder {

class Events;
class FrameQueue;
class Muxer;

class VideoEncoder {
public:
    struct Options {
        Encoder family = Encoder::Auto;
        Quality quality = Quality::High;
        int fps = kDefaultFps;
    };

    VideoEncoder(const Options &options, FrameQueue &queue, Muxer &muxer, Events &events);
    ~VideoEncoder();
    VideoEncoder(const VideoEncoder &) = delete;
    VideoEncoder &operator=(const VideoEncoder &) = delete;

    void start();
    // Drains the queue (finishing it), holds the last frame until `stopNs` (CLOCK_MONOTONIC) and flushes the
    // encoder. Returns once everything is written.
    void finish(int64_t stopNs);
    // Call after FrameQueue::cancel().
    void join();

private:
    enum class Kind { Nvenc, Vaapi, X264, OpenH264 };

    void run();
    bool open(const AVFrame *frame);
    std::string tryOpen(Kind kind, const AVFrame *frame);
    std::string openCodec(const char *name, const AVFrame *frame, AVPixelFormat format, AVDictionary **options,
                          AVBufferRef *device = nullptr);
    std::string openVaapi(const AVFrame *frame);
    std::string openVaapiDevice(const char *device, const AVFrame *frame);
    std::string openSoftware(const char *name, const AVFrame *frame);
    void reset();
    bool encode(AVFrame *frame);
    bool send(AVFrame *frame);
    bool drain();

    Options options_;
    FrameQueue &queue_;
    Muxer &muxer_;
    Events &events_;

    CodecContextPtr codec_;
    PacketPtr packet_;
    FramePtr last_;      // the most recent captured frame, repeated at the stop time when the screen went still
    FramePtr converted_; // swscale output or filter graph output
    // NVENC
    BufferPtr cuda_;
    // VA-API
    BufferPtr device_;
    FilterGraphPtr graph_;
    AVFilterContext *source_ = nullptr;
    AVFilterContext *sink_ = nullptr;
    // Software
    SwsPtr sws_;
    FramePool pool_;
    // Frames after a renegotiation, converted to the size and format the encoder was opened with
    SwsPtr adapt_;
    FramePool adaptPool_;
    FramePtr adapted_;
    int inputWidth_ = 0;
    int inputHeight_ = 0;
    AVPixelFormat inputFormat_ = AV_PIX_FMT_NONE;

    int64_t t0_ = 0;
    int64_t lastPts_ = -1;
    int64_t lastKeyPts_ = -1; // pts of the last frame encode() asked to be a keyframe
    int64_t stopNs_ = 0;
    bool recording_ = false;

    std::thread thread_;
};

} // namespace blink::recorder
