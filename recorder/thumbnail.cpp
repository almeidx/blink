#include "thumbnail.h"

#include "ffmpeg.h"
#include "recorderprotocol.h"

extern "C" {
#include <libavformat/avformat.h>
#include <libavutil/dict.h>
#include <libavutil/opt.h>
}

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <memory>
#include <string>
#include <string_view>

namespace blink::recorder {
namespace {

struct FormatCloser {
    void operator()(AVFormatContext *context) const { avformat_close_input(&context); }
};
using FormatPtr = std::unique_ptr<AVFormatContext, FormatCloser>;

int fail(const std::string &message)
{
    std::fputs(("blink-recorder: " + message + "\n").c_str(), stderr);
    return 1;
}

// Feeds packets of `stream` to `decoder` until it gives a frame.
FramePtr firstFrame(AVFormatContext *format, int stream, AVCodecContext *decoder, std::string &error)
{
    PacketPtr packet(av_packet_alloc());
    FramePtr frame(av_frame_alloc());
    if (!packet || !frame) {
        error = "out of memory";
        return {};
    }
    bool draining = false;
    for (;;) {
        if (!draining) {
            const int read = av_read_frame(format, packet.get());
            if (read == AVERROR_EOF) {
                draining = true;
                avcodec_send_packet(decoder, nullptr);
            } else if (read < 0) {
                error = "cannot read the video: " + avError(read);
                return {};
            } else {
                const bool ours = packet->stream_index == stream;
                // EAGAIN means a frame is waiting, which is all this needs.
                const int sent = ours ? avcodec_send_packet(decoder, packet.get()) : 0;
                av_packet_unref(packet.get());
                if (!ours)
                    continue;
                if (sent < 0 && sent != AVERROR(EAGAIN)) {
                    error = "cannot decode the video: " + avError(sent);
                    return {};
                }
            }
        }
        const int received = avcodec_receive_frame(decoder, frame.get());
        if (received == 0)
            return frame;
        if (received == AVERROR_EOF || (draining && received == AVERROR(EAGAIN))) {
            error = "the video has no frames";
            return {};
        }
        if (received != AVERROR(EAGAIN)) {
            error = "cannot decode the video: " + avError(received);
            return {};
        }
    }
}

} // namespace

int thumbnail(int argc, char **argv)
{
    if (argc != 4)
        return fail("usage: blink-recorder --thumbnail <size> <video>");
    const std::string_view sizeText = argv[2];
    int size = 0;
    if (const auto [end, error] = std::from_chars(sizeText.data(), sizeText.data() + sizeText.size(), size);
        error != std::errc() || end != sizeText.data() + sizeText.size() || size < 1 || size > kThumbnailMaxSize)
        return fail("the size must be from 1 to " + std::to_string(kThumbnailMaxSize));

    // Local files in the containers the library lists only: a crafted file must not have FFmpeg open other files or
    // URLs. The "file:" prefix keeps a colon in the path from reading as a protocol.
    AVDictionary *options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "format_whitelist", "mov,mp4,matroska,webm", 0);
    AVFormatContext *opened = nullptr;
    int error = avformat_open_input(&opened, ("file:" + std::string(argv[3])).c_str(), nullptr, &options);
    av_dict_free(&options);
    if (error < 0)
        return fail("cannot open " + std::string(argv[3]) + ": " + avError(error));
    const FormatPtr format(opened);
    if (error = avformat_find_stream_info(format.get(), nullptr); error < 0)
        return fail("cannot read the streams: " + avError(error));

    const AVCodec *codec = nullptr;
    const int stream = av_find_best_stream(format.get(), AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
    if (stream < 0)
        return fail("no video stream that FFmpeg can decode: " + avError(stream));
    const CodecContextPtr decoder(avcodec_alloc_context3(codec));
    if (!decoder)
        return fail("out of memory");
    if (error = avcodec_parameters_to_context(decoder.get(), format->streams[stream]->codecpar); error < 0)
        return fail("cannot set up the decoder: " + avError(error));
    if (error = avcodec_open2(decoder.get(), codec, nullptr); error < 0)
        return fail(std::string("cannot open the ") + codec->name + " decoder: " + avError(error));

    std::string message;
    const FramePtr frame = firstFrame(format.get(), stream, decoder.get(), message);
    if (!frame)
        return fail(message);
    if (frame->width <= 0 || frame->height <= 0)
        return fail("the frame has no size");

    const double scale = std::min({1.0, static_cast<double>(size) / frame->width, static_cast<double>(size) / frame->height});
    const int width = std::max(1, static_cast<int>(std::lround(frame->width * scale)));
    const int height = std::max(1, static_cast<int>(std::lround(frame->height * scale)));
    const FramePtr rgb(av_frame_alloc());
    const SwsPtr sws(sws_alloc_context());
    if (!rgb || !sws)
        return fail("out of memory");
    rgb->width = width;
    rgb->height = height;
    rgb->format = AV_PIX_FMT_RGB24;
    if (error = av_frame_get_buffer(rgb.get(), 0); error < 0)
        return fail("out of memory");
    av_opt_set_int(sws.get(), "srcw", frame->width, 0);
    av_opt_set_int(sws.get(), "srch", frame->height, 0);
    av_opt_set_int(sws.get(), "src_format", frame->format, 0);
    av_opt_set_int(sws.get(), "dstw", width, 0);
    av_opt_set_int(sws.get(), "dsth", height, 0);
    av_opt_set_int(sws.get(), "dst_format", AV_PIX_FMT_RGB24, 0);
    av_opt_set_int(sws.get(), "sws_flags", SWS_AREA, 0); // averages, as downscaling needs
    if (error = sws_init_context(sws.get(), nullptr, nullptr); error < 0)
        return fail("swscale: " + avError(error));
    // The video's own matrix and range (Blink's recordings: limited-range BT.709, or BT.601 from NVENC with RGB input)
    // to full-range RGB. Not applicable, and harmlessly refused, when the video is RGB already.
    const int *matrix = sws_getCoefficients(frame->colorspace);
    sws_setColorspaceDetails(sws.get(), matrix, frame->color_range == AVCOL_RANGE_JPEG ? 1 : 0, matrix, 1, 0, 1 << 16, 1 << 16);
    if (error = sws_scale_frame(sws.get(), rgb.get(), frame.get()); error < 0)
        return fail("cannot convert the frame: " + avError(error));

    std::fputs(("P6\n" + std::to_string(width) + ' ' + std::to_string(height) + "\n255\n").c_str(), stdout);
    const auto rowBytes = static_cast<size_t>(width) * 3;
    for (int y = 0; y < height; ++y) {
        if (std::fwrite(rgb->data[0] + (static_cast<ptrdiff_t>(y) * rgb->linesize[0]), 1, rowBytes, stdout) != rowBytes)
            return fail("cannot write the image");
    }
    return std::fflush(stdout) == 0 ? 0 : fail("cannot write the image");
}

} // namespace blink::recorder
