#include "ffmpeg.h"

extern "C" {
#include <libavutil/error.h>
#include <libavutil/imgutils.h>
#include <libavutil/macros.h>
#include <libavutil/pixdesc.h>
}

#include <array>

namespace blink::recorder {

std::string avError(int error)
{
    std::array<char, AV_ERROR_MAX_STRING_SIZE> text{};
    av_strerror(error, text.data(), text.size());
    return text.data();
}

bool FramePool::init(int width, int height, AVPixelFormat format)
{
    pool_.reset();
    // Rows padded to 64 pixels keep every plane SIMD-aligned for swscale and the encoders.
    if (av_image_fill_linesizes(linesize_, format, FFALIGN(width, 64)) < 0)
        return false;
    ptrdiff_t linesizes[4] = {linesize_[0], linesize_[1], linesize_[2], linesize_[3]};
    size_t sizes[4] = {};
    if (av_image_fill_plane_sizes(sizes, format, height, linesizes) < 0)
        return false;
    planes_ = av_pix_fmt_count_planes(format);
    size_t total = 0;
    for (int i = 0; i < planes_; ++i) {
        offset_[i] = static_cast<ptrdiff_t>(total);
        total += sizes[i];
    }
    pool_.reset(av_buffer_pool_init(total + AV_INPUT_BUFFER_PADDING_SIZE, nullptr));
    if (!pool_)
        return false;
    width_ = width;
    height_ = height;
    format_ = format;
    return true;
}

bool FramePool::matches(int width, int height, AVPixelFormat format) const
{
    return pool_ && width == width_ && height == height_ && format == format_;
}

bool FramePool::get(AVFrame *frame) const
{
    AVBufferRef *buffer = av_buffer_pool_get(pool_.get());
    if (!buffer)
        return false;
    frame->buf[0] = buffer;
    for (int i = 0; i < planes_; ++i) {
        frame->data[i] = buffer->data + offset_[i];
        frame->linesize[i] = linesize_[i];
    }
    frame->width = width_;
    frame->height = height_;
    frame->format = format_;
    return true;
}

} // namespace blink::recorder
