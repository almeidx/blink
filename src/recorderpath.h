#pragma once

// Where blink-recorder is. Shared by recordings and the library's video thumbnails, so the thumbnailer does not
// pull LayerShellQt and the recording machinery in through recording.h.

#include <QString>

namespace blink {

// blink-recorder: the one built next to blink when it runs from a build tree, else the installed one.
QString recorderPath();

} // namespace blink
