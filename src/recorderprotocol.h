#pragma once

// The contract between blink (the tray app) and blink-recorder, which the app starts for each recording.
// Plain C++ so the recorder needs no Qt.
//
// Command line:
//   blink-recorder (--serial <n> | --node <n>) --output <file.mp4> [--fps <n>] [--quality low|medium|high]
//                  [--encoder auto|nvenc|vaapi|software] [--system-audio] [--microphone]
// --serial is the 64-bit PipeWire object serial of KWin's screencast stream (zkde_screencast v6+), --node its 32-bit
// node id (older KWin). Defaults: --fps 60 --quality high --encoder auto, no audio; --fps accepts 1 to 240.
//
// stdin, one command per line: "stop" finishes the file, "abort" deletes it. EOF, SIGINT and SIGTERM count as "stop".
// stdout, one event per line: "<event>" or "<event> <text>".
//
// blink-recorder --thumbnail <size> <video> instead writes the video's first frame to stdout as a binary PPM image
// that fits in size×size pixels, for the library; see recorder/thumbnail.h.

#include <optional>
#include <string_view>

namespace blink::recorder {

inline constexpr std::string_view kOptSerial = "--serial";
inline constexpr std::string_view kOptNode = "--node";
inline constexpr std::string_view kOptOutput = "--output";
inline constexpr std::string_view kOptFps = "--fps";
inline constexpr std::string_view kOptQuality = "--quality";
inline constexpr std::string_view kOptEncoder = "--encoder";
inline constexpr std::string_view kOptSystemAudio = "--system-audio";
inline constexpr std::string_view kOptMicrophone = "--microphone";
inline constexpr std::string_view kOptThumbnail = "--thumbnail";

inline constexpr std::string_view kQualityLow = "low";
inline constexpr std::string_view kQualityMedium = "medium";
inline constexpr std::string_view kQualityHigh = "high";

// auto tries NVENC, then VA-API, then software (libx264, then libopenh264).
inline constexpr std::string_view kEncoderAuto = "auto";
inline constexpr std::string_view kEncoderNvenc = "nvenc";
inline constexpr std::string_view kEncoderVaapi = "vaapi";
inline constexpr std::string_view kEncoderSoftware = "software";

enum class Quality { Low, Medium, High };
enum class Encoder { Auto, Nvenc, Vaapi, Software };

constexpr std::string_view name(Quality quality)
{
    switch (quality) {
    case Quality::Low:
        return kQualityLow;
    case Quality::Medium:
        return kQualityMedium;
    case Quality::High:
        break;
    }
    return kQualityHigh;
}

constexpr std::string_view name(Encoder encoder)
{
    switch (encoder) {
    case Encoder::Nvenc:
        return kEncoderNvenc;
    case Encoder::Vaapi:
        return kEncoderVaapi;
    case Encoder::Software:
        return kEncoderSoftware;
    case Encoder::Auto:
        break;
    }
    return kEncoderAuto;
}

constexpr std::optional<Quality> qualityFromName(std::string_view text)
{
    for (const Quality quality : {Quality::Low, Quality::Medium, Quality::High})
        if (text == name(quality))
            return quality;
    return std::nullopt;
}

constexpr std::optional<Encoder> encoderFromName(std::string_view text)
{
    for (const Encoder encoder : {Encoder::Auto, Encoder::Nvenc, Encoder::Vaapi, Encoder::Software})
        if (text == name(encoder))
            return encoder;
    return std::nullopt;
}

// --fps bounds and default. 240 covers every current display; higher values only cost dropped frames.
inline constexpr int kMinFps = 1;
inline constexpr int kMaxFps = 240;
inline constexpr int kDefaultFps = 60;
// --thumbnail size limit: the largest tier of the freedesktop.org thumbnail cache (its xx-large directory).
inline constexpr int kThumbnailMaxSize = 1024;

inline constexpr std::string_view kCmdStop = "stop";
inline constexpr std::string_view kCmdAbort = "abort";

inline constexpr std::string_view kEvEncoder = "encoder";     // <FFmpeg encoder name> <width>x<height>, once the stream format is known
inline constexpr std::string_view kEvRecording = "recording"; // the first frame is in
inline constexpr std::string_view kEvWarning = "warning";     // <text>; recording goes on
inline constexpr std::string_view kEvError = "error";         // <text>; exits with status 1
inline constexpr std::string_view kEvFinished = "finished";   // the file is complete; exits with status 0
inline constexpr std::string_view kEvAborted = "aborted";     // file deleted (abort, or stop before the first frame); exits with status 0

} // namespace blink::recorder
