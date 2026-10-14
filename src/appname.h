#pragma once

#include <QString>

namespace blink {

// What the app KWin knows as `app` (its desktop file name, else its window class) calls itself: the untranslated
// Name in its desktop file, e.g. "Konsole" for "org.kde.konsole". `app` itself when there is no desktop file.
// Reads the file: a few stat calls and a small read.
QString appName(const QString &app);

} // namespace blink
