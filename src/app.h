#pragma once

#include "regionselector.h"
#include "settings.h"
#include "uploader.h"
#include "windowlist.h"

#include <QElapsedTimer>
#include <QFuture>
#include <QImage>
#include <QKeySequence>
#include <QList>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QTimer>

#include <array>
#include <memory>
#include <optional>
#include <vector>

class QAction;
class QMenu;
class QSystemTrayIcon;

namespace blink {

class KWinScreenshot;
class Notifier;
class LibraryWindow;
class Recording;
class SettingsDialog;
struct CaptureInfo;

// The tray app. Global shortcuts and the tray menu start captures; every capture goes to the clipboard, and
// optionally into a folder and an S3 bucket. `blink --region` and friends reach the running instance over D-Bus.
class App final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.almeidx.Blink")

public:
    // Where the running Blink is on the session bus; its interface is the class info above.
    static constexpr QLatin1StringView kDBusService{"io.github.almeidx.Blink"};
    static constexpr QLatin1StringView kDBusPath{"/Blink"};

    App();
    ~App() override;

    Q_SCRIPTABLE Q_INVOKABLE void captureRegion();
    Q_SCRIPTABLE Q_INVOKABLE void captureWindow();
    Q_SCRIPTABLE Q_INVOKABLE void captureFullscreen();
    Q_SCRIPTABLE Q_INVOKABLE void toggleRecording();
    Q_SCRIPTABLE Q_INVOKABLE void showSettings();
    Q_SCRIPTABLE Q_INVOKABLE void showLibrary();

private:
    struct Shortcut {
        QAction *action;    // registered with KGlobalAccel under its text
        QKeySequence key;   // the default
        QAction *menuEntry; // the tray menu entry that shows the key
    };

    void createActions();
    void registerShortcuts();
    void createTray();
    void updateTray();
    void applySettings(const Settings &settings);
    void updateAutostart();

    bool beginCapture(); // false while another capture or selection is in progress
    void selectRegion(RegionSelector::Purpose purpose);
    void showSelector(std::vector<ScreenImage> screens, const QFuture<QList<WindowList::Window>> &windows, RegionSelector::Purpose purpose);
    void deliverScreenshot(const QImage &image, const QString &app);
    void screenshotDone(const QByteArray &png, const CaptureInfo &info, const QString &path, const QString &saveError,
                        const QImage &thumbnail);
    void startRecording(const QRect &region, qreal scale, const QString &app);
    void recordingDone();
    void quit(); // the tray's Quit: a running recording is finished first
    void deliverRecording(const QString &path, const CaptureInfo &info);
    void uploadFile(const QString &path); // a file in the library
    // Where `info` goes in the bucket; nothing, and the user is told why, when it can't be uploaded.
    std::optional<QString> uploadKey(const CaptureInfo &info);
    Uploader *uploader();
    void uploaded(const Uploader::Result &result);

    Settings settings_;
    KWinScreenshot *screenshot_;
    WindowList *windows_;
    Notifier *notifier_;
    Uploader *uploader_ = nullptr; // created on the first upload

    QAction *regionAction_ = nullptr;
    QAction *windowAction_ = nullptr;
    QAction *fullscreenAction_ = nullptr;
    QAction *recordAction_ = nullptr;
    QAction *recordMenuAction_ = nullptr; // says what a click does now; recordAction_'s text stays fixed
    QAction *abortAction_ = nullptr;
    std::array<Shortcut, 4> shortcuts_{};
    std::unique_ptr<QMenu> menu_;
    QSystemTrayIcon *tray_ = nullptr;

    bool busy_ = false; // capturing or selecting: further capture requests are ignored
    // Paths handed to the encoder but not written yet: a second capture in the same second must not reuse them.
    QSet<QString> pendingFiles_;
    QElapsedTimer captureTimer_;
    QPointer<Recording> recording_;
    QElapsedTimer recordingTimer_;
    QTimer recordingTicker_; // keeps the tray tooltip's elapsed time current
    bool quitting_ = false;  // Quit was chosen during a recording: recordingDone() ends the app
    QPointer<SettingsDialog> dialog_;
    QPointer<LibraryWindow> library_;
};

} // namespace blink
