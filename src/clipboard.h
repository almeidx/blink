#pragma once

#include <QByteArray>
#include <QFuture>
#include <QString>

// Clipboard writes through KSystemClipboard, which uses Wayland's data control protocol: plain QClipboard is
// ignored by the compositor unless one of our windows has keyboard focus, and after a hotkey none has.
namespace blink::clipboard {

// Offers a PNG that may still be encoding; a paste that arrives first waits for it.
void setPng(const QFuture<QByteArray> &png);
// Offers a file the way file managers copy one, so it pastes as an attachment.
void setFile(const QString &path);
void setText(const QString &text);

} // namespace blink::clipboard
