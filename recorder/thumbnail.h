#pragma once

namespace blink::recorder {

// blink-recorder --thumbnail <size> <video>: writes the first frame of the video to stdout as a binary PPM image,
// scaled to fit in size×size pixels and never enlarged. The tray app's library makes video thumbnails this way, since
// the recorder is what has FFmpeg. Returns the exit status; errors go to stderr.
int thumbnail(int argc, char **argv);

} // namespace blink::recorder
