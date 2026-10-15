#pragma once

#include "settings.h"
#include "windowlist.h"

#include <QElapsedTimer>
#include <QImage>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QRectF>
#include <QTimer>

#include <memory>
#include <vector>

class QAction;
class QMenu;
class QSystemTrayIcon;

namespace blink {

class KWinScreenshot;
class Notifier;
class LibraryWindow;
class Recording;
class RegionSelector;
class SettingsDialog;
class Uploader;
struct ScreenImage;
struct CaptureInfo;

// The tray app. Global shortcuts and the tray menu start captures; every capture goes to the clipboard, and
// optionally into a folder and an S3 bucket. `blink --region` and friends reach the running instance over D-Bus
// (io.github.almeidx.Blink, object /Blink).
class App final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.almeidx.Blink")

public:
    App();
    ~App() override;

    Q_SCRIPTABLE Q_INVOKABLE void captureRegion();
    Q_SCRIPTABLE Q_INVOKABLE void captureWindow();
    Q_SCRIPTABLE Q_INVOKABLE void captureFullscreen();
    Q_SCRIPTABLE Q_INVOKABLE void toggleRecording();
    Q_SCRIPTABLE Q_INVOKABLE void showSettings();
    Q_SCRIPTABLE Q_INVOKABLE void showLibrary();

private:
    enum class Purpose { Screenshot, Recording };

    void createActions();
    void registerShortcuts();
    void createTray();
    void updateTray();
    void applySettings(const Settings &settings);
    void applyAutostart() const;

    void selectRegion(Purpose purpose);
    void showSelector(std::vector<ScreenImage> shots, Purpose purpose);
    void deliverScreenshot(const QImage &image, const QString &app);
    void screenshotDone(const QByteArray &png, const CaptureInfo &info, const QString &path, const QString &saveError,
                        const QImage &thumbnail);
    void startRecording(const QRect &region, qreal scale, const QString &app);
    void recordingDone();
    void deliverRecording(const QString &path, const CaptureInfo &info);
    // From memory when `path` is empty, else from the file.
    void upload(const QByteArray &data, const QString &path, const CaptureInfo &info, const QString &contentType);
    void uploadFile(const QString &path); // a file in the library

    Settings settings_;
    KWinScreenshot *screenshot_;
    WindowList *windows_;
    Notifier *notifier_;
    Uploader *uploader_ = nullptr; // created on the first upload

    // Global shortcuts; the first three are also tray menu entries.
    QAction *regionAction_ = nullptr;
    QAction *windowAction_ = nullptr;
    QAction *fullscreenAction_ = nullptr;
    QAction *recordAction_ = nullptr;
    // Tray menu only.
    QAction *recordMenuAction_ = nullptr;
    QAction *abortAction_ = nullptr;
    std::unique_ptr<QMenu> menu_;
    QSystemTrayIcon *tray_ = nullptr;

    bool busy_ = false; // capturing or selecting: further capture requests are ignored
    QElapsedTimer captureTimer_;
    QPointer<RegionSelector> selector_;
    QList<WindowList::Window> knownWindows_;
    bool windowsKnown_ = false;
    QPointer<Recording> recording_;
    QElapsedTimer recordingTimer_;
    QTimer recordingTicker_; // keeps the tray tooltip's elapsed time current
    QPointer<SettingsDialog> dialog_;
    QPointer<LibraryWindow> library_;
};

} // namespace blink
