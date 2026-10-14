#pragma once

#include <QImage>
#include <QObject>
#include <QVariantMap>

#include <functional>

namespace blink {

// Client of KWin's org.kde.KWin.ScreenShot2 D-Bus API. KWin renders the capture itself and writes raw pixels
// into a pipe, so a 4K screen is in memory ~15 ms after the call. KWin only answers executables that a desktop
// file lists with X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2.
class KWinScreenshot final : public QObject {
    Q_OBJECT

public:
    struct Options {
        bool cursor = false;
        bool decorations = true; // window captures only
        bool shadow = false;     // window captures only
    };
    // Exactly one of `image` and `error` is set. The image is at native resolution, with the scale as its device
    // pixel ratio.
    using Callback = std::function<void(const QImage &image, const QString &error)>;

    explicit KWinScreenshot(QObject *parent = nullptr);

    void captureScreen(const QString &name, const Options &options, Callback done);
    void captureWorkspace(const Options &options, Callback done); // all screens in one image
    void captureActiveWindow(const Options &options, Callback done);

private:
    void call(const QString &method, QVariantList arguments, const Options &options, Callback done);
};

} // namespace blink
