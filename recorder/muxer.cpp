#include "muxer.h"

#include "ffmpeg.h"

#include <unistd.h>

namespace blink::recorder {
namespace {

std::string setParameters(AVStream *stream, const AVCodecContext *codec)
{
    const int error = avcodec_parameters_from_context(stream->codecpar, codec);
    if (error < 0)
        return "cannot set the stream parameters: " + avError(error);
    stream->time_base = codec->time_base;
    return {};
}

} // namespace

Muxer::~Muxer()
{
    close();
}

std::string Muxer::open(const std::string &path)
{
    path_ = path;
    int error = avformat_alloc_output_context2(&context_, nullptr, "mp4", path.c_str());
    if (error < 0)
        return "cannot create the MP4 muxer: " + avError(error);
    video_ = avformat_new_stream(context_, nullptr);
    if (!video_)
        return "cannot create the video stream";
    error = avio_open(&context_->pb, path.c_str(), AVIO_FLAG_WRITE);
    if (error < 0)
        return "cannot create " + path + ": " + avError(error);
    return {};
}

bool Muxer::globalHeader() const
{
    return (context_->oformat->flags & AVFMT_GLOBALHEADER) != 0;
}

std::string Muxer::setVideoParameters(const AVCodecContext *codec)
{
    return setParameters(video_, codec);
}

std::string Muxer::addAudioStream(const AVCodecContext *codec)
{
    if (codec->codec_id == AV_CODEC_ID_OPUS)
        context_->strict_std_compliance = FF_COMPLIANCE_EXPERIMENTAL;
    audio_ = avformat_new_stream(context_, nullptr);
    if (!audio_)
        return "cannot create the audio stream";
    return setParameters(audio_, codec);
}

std::string Muxer::start()
{
    std::lock_guard lock(mutex_);
    AVDictionary *options = nullptr;
    av_dict_set(&options, "movflags", "+faststart", 0);
    const int error = avformat_write_header(context_, &options);
    av_dict_free(&options);
    if (error < 0)
        return "cannot write the MP4 header: " + avError(error);
    started_.store(true, std::memory_order_release);
    return {};
}

std::string Muxer::write(AVPacket *packet, Stream stream, AVRational timeBase)
{
    std::lock_guard lock(mutex_);
    const AVStream *target = stream == Stream::Video ? video_ : audio_;
    packet->stream_index = target->index;
    av_packet_rescale_ts(packet, timeBase, target->time_base);
    const int error = av_interleaved_write_frame(context_, packet);
    if (error < 0)
        return "cannot write to the file: " + avError(error);
    return {};
}

std::string Muxer::finish()
{
    std::lock_guard lock(mutex_);
    const int error = av_write_trailer(context_);
    close();
    if (error < 0) {
        // Without the trailer (the moov box) the file cannot be played, and discard() cannot delete it any more
        // once the context is gone: it is removed here so the library never lists it.
        unlink(path_.c_str());
        return "cannot finish the file: " + avError(error);
    }
    return {};
}

void Muxer::discard()
{
    std::lock_guard lock(mutex_);
    const bool created = context_ && context_->pb;
    close();
    if (created)
        unlink(path_.c_str());
}

void Muxer::close()
{
    if (!context_)
        return;
    avio_closep(&context_->pb);
    avformat_free_context(context_);
    context_ = nullptr;
    video_ = nullptr;
    audio_ = nullptr;
}

} // namespace blink::recorder
