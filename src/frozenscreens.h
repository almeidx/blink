#pragma once

#include <QImage>
#include <QString>

#include <functional>
#include <vector>

class QObject;
class QScreen;

namespace blink {

class KWinScreenshot;

// A screen frozen by KWin, ready for the region selection: `image` is 32-bit (RGB32, ARGB32 or ARGB32 premultiplied) at
// native resolution with the screen's scale as its device pixel ratio; `dimmed` is its backdrop.
struct ScreenImage {
    QScreen *screen = nullptr;
    QImage image;
    QImage dimmed;
};

// All screens in one list, or an empty list: then `error` says why, or is empty when the screens changed meanwhile.
using FrozenScreens = std::function<void(std::vector<ScreenImage> screens, const QString &error)>;

// Captures every screen. Each one's pixel work (KWin sends RGBX8888, which the overlay cannot blit 1:1, and the
// backdrop is a full pass over the pixels) runs on the thread pool while the others are still arriving. `done` runs
// on `context`'s thread, unless `context` is gone by then.
void freezeScreens(KWinScreenshot &screenshot, QObject *context, FrozenScreens done);

} // namespace blink
