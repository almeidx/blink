#pragma once

// The contract between blink (the tray app) and blink-recorder, which the app starts for each recording.
// Plain C++ so the recorder needs no Qt.
//
// Command line:
//   blink-recorder (--serial <n> | --node <n>) --output <file.mp4> [--fps <n>] [--quality low|medium|high]
//                  [--encoder auto|nvenc|vaapi|software] [--system-audio] [--microphone]
// --serial is the PipeWire object serial of KWin's screencast stream (zkde_screencast v6+), --node its node id
// (older KWin). Defaults: --fps 60 --quality high --encoder auto, no audio.
//
// stdin, one command per line: "stop" finishes the file, "abort" deletes it. EOF, SIGINT and SIGTERM count as "stop".
// stdout, one event per line: "<event>" or "<event> <text>".

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

inline constexpr std::string_view kQualityLow = "low";
inline constexpr std::string_view kQualityMedium = "medium";
inline constexpr std::string_view kQualityHigh = "high";

// auto tries NVENC, then VA-API, then software (libx264, then libopenh264).
inline constexpr std::string_view kEncoderAuto = "auto";
inline constexpr std::string_view kEncoderNvenc = "nvenc";
inline constexpr std::string_view kEncoderVaapi = "vaapi";
inline constexpr std::string_view kEncoderSoftware = "software";

inline constexpr std::string_view kCmdStop = "stop";
inline constexpr std::string_view kCmdAbort = "abort";

inline constexpr std::string_view kEvEncoder = "encoder";     // <FFmpeg encoder name>, once the stream format is known
inline constexpr std::string_view kEvRecording = "recording"; // the first frame is in
inline constexpr std::string_view kEvWarning = "warning";     // <text>; recording goes on
inline constexpr std::string_view kEvError = "error";         // <text>; exits with status 1
inline constexpr std::string_view kEvFinished = "finished";   // the file is complete; exits with status 0
inline constexpr std::string_view kEvAborted = "aborted";     // the file was deleted; exits with status 0

} // namespace blink::recorder
