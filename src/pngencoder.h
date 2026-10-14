#pragma once

#include <QByteArray>

class QImage;

namespace blink {

// Encodes `image` as PNG. Rows are filtered and deflated in parallel strips that join into a single zlib
// stream, so a 4K screenshot takes milliseconds rather than the ~150 ms of QImageWriter. Images whose
// pixels are all opaque are written as RGB, others as RGBA. Blocks the calling thread; thread-safe.
QByteArray encodePng(const QImage &image);

} // namespace blink
