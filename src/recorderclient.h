#pragma once

// The tray app's side of blink-recorder (recorderprotocol.h): finding it, its names as Qt strings, and its
// --thumbnail mode. Recording drives the recorder's main mode.

#include "recorderprotocol.h"

#include <QImage>
#include <QString>

#include <atomic>
#include <string_view>

namespace blink {

// blink-recorder: the one built next to blink when it runs from a build tree, else the installed one.
QString recorderPath();

inline QLatin1StringView latin1(std::string_view text)
{
    return {text.data(), static_cast<qsizetype>(text.size())};
}

// The first frame of the video at `path`, scaled to fit in size×size pixels. Null when it can't be made, or when
// `stopping` is set while the recorder works. Blocks: off the UI thread only.
QImage videoThumbnail(const QString &path, int size, const std::atomic_bool &stopping);

} // namespace blink
