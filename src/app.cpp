#include "app.h"

#include "appname.h"
#include "autostart.h"
#include "clipboard.h"
#include "kwinscreenshot.h"
#include "librarywindow.h"
#include "logging.h"
#include "notifier.h"
#include "pathpattern.h"
#include "pngencoder.h"
#include "recording.h"
#include "settingsdialog.h"
#include "threadpool.h"

#include <KGlobalAccel>
#include <KGlobalShortcutInfo>

#include <QAction>
#include <QCoreApplication>
#include <QDBusConnection>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QIcon>
#include <QImageReader>
#include <QMenu>
#include <QMetaClassInfo>
#include <QMimeDatabase>
#include <QSaveFile>
#include <QSettings>
#include <QSystemTrayIcon>

#include <algorithm>
#include <chrono>
#include <functional>
#include <utility>

namespace blink {
namespace {

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

// A capture taken now of `app` (KWin's name for it; empty when it isn't of one window).
CaptureInfo newCapture(const QString &app, const QString &extension, QSize size = {})
{
    const QDateTime time = QDateTime::currentDateTime();
    return {time, captureName(time, appName(app)), extension, size};
}

// <folder>/<name>.<extension>, numbered when that exists on disk or is in `pending` (chosen, not written yet).
QString newFile(const QString &folder, const QString &name, const QString &extension, const QSet<QString> &pending)
{
    const QString stem = folder + u'/' + name;
    QString path = stem + u'.' + extension;
    for (int n = 2; pending.contains(path) || QFileInfo::exists(path); ++n)
        path = stem + u'_' + QString::number(n) + u'.' + extension;
    return path;
}

QString writeFile(const QString &path, const QByteArray &data)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(data) != data.size() || !file.commit())
        return file.errorString();
    return {};
}

QString elapsedText(std::chrono::milliseconds elapsed)
{
    const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
    const QString minutesAndSeconds = QString::number(seconds / 60 % 60).rightJustified(2, u'0') + u':'
                                    + QString::number(seconds % 60).rightJustified(2, u'0');
    return seconds >= 3600 ? QString::number(seconds / 3600) + u':' + minutesAndSeconds : minutesAndSeconds;
}

} // namespace

App::App() : screenshot_(new KWinScreenshot(this)), windows_(new WindowList(this)), notifier_(new Notifier(this))
{
    const bool firstRun = !QFileInfo::exists(QSettings().fileName());
    settings_ = Settings::load();
    if (firstRun)
        settings_.save();
    updateAutostart();

    createActions();
    registerShortcuts();
    createTray();
    QDBusConnection::sessionBus().registerObject(kDBusPath, this, QDBusConnection::ExportScriptableInvokables);

    recordingTicker_.setInterval(1s);
    connect(&recordingTicker_, &QTimer::timeout, this, &App::updateTray);

    if (firstRun) {
        QString keys;
        for (const Shortcut &shortcut : shortcuts_) {
            if (const QList<QKeySequence> current = KGlobalAccel::self()->shortcut(shortcut.action); !current.isEmpty())
                keys += u'\n' + current.first().toString(QKeySequence::NativeText) + u": "_s + shortcut.action->text();
        }
        notifier_->notify(tr("Blink is in the system tray"), tr("Shortcuts can be changed in System Settings.") + keys);
    }
}

QString App::dbusInterface()
{
    return QLatin1StringView(staticMetaObject.classInfo(staticMetaObject.indexOfClassInfo("D-Bus Interface")).value());
}

App::~App() = default;

void App::createActions()
{
    const auto action = [this](const QString &icon, const QString &text, const QString &name, void (App::*slot)()) {
        auto *created = new QAction(QIcon::fromTheme(icon), text, this);
        created->setObjectName(name);
        connect(created, &QAction::triggered, this, slot);
        return created;
    };
    regionAction_ = action(u"select-rectangular"_s, tr("Capture Region"), u"capture-region"_s, &App::captureRegion);
    windowAction_ = action(u"window"_s, tr("Capture Active Window"), u"capture-window"_s, &App::captureWindow);
    fullscreenAction_ = action(u"view-fullscreen"_s, tr("Capture Full Screen"), u"capture-fullscreen"_s, &App::captureFullscreen);
    recordAction_ = action(u"media-record"_s, tr("Start or Stop Recording"), u"toggle-recording"_s, &App::toggleRecording);
    recordMenuAction_ = new QAction(this);
    connect(recordMenuAction_, &QAction::triggered, this, &App::toggleRecording);

    shortcuts_ = {{
        {regionAction_, QKeySequence(Qt::Key_Print), regionAction_},
        {windowAction_, QKeySequence(Qt::META | Qt::Key_Print), windowAction_},
        {fullscreenAction_, QKeySequence(Qt::SHIFT | Qt::Key_Print), fullscreenAction_},
        {recordAction_, QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_R), recordMenuAction_},
    }};

    abortAction_ = new QAction(QIcon::fromTheme(u"dialog-cancel"_s), tr("Discard Recording"), this);
    connect(abortAction_, &QAction::triggered, this, [this] {
        if (recording_)
            recording_->abort();
    });
}

void App::registerShortcuts()
{
    // Spectacle holds these keys by default and KGlobalAccel gives a key to one owner only, so take them from
    // it, once: later changes in System Settings are the user's.
    QSettings state;
    if (!state.value(u"State/shortcutsClaimed"_s).toBool()) {
        for (const Shortcut &shortcut : shortcuts_) {
            const QList<KGlobalShortcutInfo> owners = KGlobalAccel::globalShortcutsByKey(shortcut.key);
            if (std::ranges::any_of(
                    owners, [](const KGlobalShortcutInfo &owner) { return owner.componentUniqueName() == "org.kde.spectacle.desktop"_L1; }))
                KGlobalAccel::stealShortcutSystemwide(shortcut.key);
        }
        state.setValue(u"State/shortcutsClaimed"_s, true);
        state.sync();
        QFile::setPermissions(state.fileName(), QFile::ReadOwner | QFile::WriteOwner);
    }
    for (const Shortcut &shortcut : shortcuts_) {
        KGlobalAccel::self()->setDefaultShortcut(shortcut.action, {shortcut.key});
        KGlobalAccel::self()->setShortcut(shortcut.action, {shortcut.key});
        // Shown only: QAction shortcuts are inert here, since the menu is exported to the panel.
        shortcut.menuEntry->setShortcut(KGlobalAccel::self()->shortcut(shortcut.action).value(0));
    }
    connect(KGlobalAccel::self(), &KGlobalAccel::globalShortcutChanged, this, [this](QAction *action, const QKeySequence &key) {
        for (const Shortcut &shortcut : shortcuts_) {
            if (shortcut.action == action)
                shortcut.menuEntry->setShortcut(key);
        }
    });
}

void App::createTray()
{
    menu_ = std::make_unique<QMenu>();
    menu_->addAction(regionAction_);
    menu_->addAction(windowAction_);
    menu_->addAction(fullscreenAction_);
    menu_->addSeparator();
    menu_->addAction(recordMenuAction_);
    menu_->addAction(abortAction_);
    menu_->addSeparator();
    menu_->addAction(QIcon::fromTheme(u"folder-pictures"_s), tr("Library"), this, &App::showLibrary);
    menu_->addAction(QIcon::fromTheme(u"configure"_s), u"Settings…"_s, this, &App::showSettings);
    menu_->addAction(QIcon::fromTheme(u"application-exit"_s), u"Quit"_s, this, &App::quit);

    tray_ = new QSystemTrayIcon(QGuiApplication::windowIcon(), this);
    tray_->setContextMenu(menu_.get());
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason reason) {
        if (reason != QSystemTrayIcon::Trigger)
            return;
        if (recording_)
            recording_->stop();
        else
            captureRegion();
    });
    updateTray();
    tray_->show();
}

void App::updateTray()
{
    const bool recording = recording_;
    recordMenuAction_->setText(recording ? tr("Stop Recording") : tr("Start Recording"));
    recordMenuAction_->setIcon(QIcon::fromTheme(recording ? u"media-playback-stop"_s : u"media-record"_s));
    abortAction_->setVisible(recording);
    tray_->setIcon(recording ? QIcon::fromTheme(u"media-record"_s, QGuiApplication::windowIcon()) : QGuiApplication::windowIcon());
    if (!recording)
        tray_->setToolTip(QGuiApplication::applicationDisplayName());
    else if (quitting_)
        tray_->setToolTip(tr("Blink: finishing the recording…"));
    else if (recordingTimer_.isValid())
        tray_->setToolTip(tr("Blink: recording %1. Click to stop.").arg(elapsedText(std::chrono::milliseconds(recordingTimer_.elapsed()))));
    else
        tray_->setToolTip(tr("Blink: starting to record"));
}

bool App::beginCapture()
{
    if (busy_)
        return false;
    busy_ = true;
    captureTimer_.start();
    return true;
}

void App::captureRegion()
{
    selectRegion(RegionSelector::Purpose::Screenshot);
}

void App::captureWindow()
{
    if (!beginCapture())
        return;
    const KWinScreenshot::Options options{settings_.captureCursor, settings_.windowDecorations, settings_.windowShadow};
    screenshot_->captureActiveWindow(options, [this](const QImage &image, const QString &app, const QString &error) {
        busy_ = false;
        if (!error.isEmpty())
            notifier_->error(tr("Could not capture the window"), error);
        else
            deliverScreenshot(image, app);
    });
}

void App::captureFullscreen()
{
    if (!beginCapture())
        return;
    screenshot_->captureWorkspace({.cursor = settings_.captureCursor}, [this](const QImage &image, const QString &, const QString &error) {
        busy_ = false;
        if (!error.isEmpty())
            notifier_->error(tr("Could not capture the screen"), error);
        else
            deliverScreenshot(image, {});
    });
}

void App::toggleRecording()
{
    if (recording_)
        recording_->stop();
    else
        selectRegion(RegionSelector::Purpose::Recording);
}

void App::quit()
{
    if (!recording_) {
        QCoreApplication::quit();
        return;
    }
    // Ending the event loop now would destroy the Recording while blink-recorder writes the end of the file (with
    // +faststart that rewrites all of it). recordingDone() quits once the recorder has reported its outcome.
    quitting_ = true;
    recording_->stop();
    updateTray();
}

void App::selectRegion(RegionSelector::Purpose purpose)
{
    if ((purpose == RegionSelector::Purpose::Recording && recording_) || !beginCapture())
        return;
    // Runs while the screens are captured; the selection takes the windows whenever they come.
    const QFuture<QList<WindowList::Window>> windows = windows_->query().then(this, [this](const QList<WindowList::Window> &found) {
        qCDebug(lcBlink) << found.size() << "windows after" << captureTimer_.elapsed() << "ms";
        return found;
    });
    freezeScreens(*screenshot_, this, [this, windows, purpose](std::vector<ScreenImage> screens, const QString &error) {
        if (screens.empty()) {
            busy_ = false;
            if (!error.isEmpty())
                notifier_->error(tr("Could not capture the screen"), error);
            return;
        }
        showSelector(std::move(screens), windows, purpose);
    });
}

void App::showSelector(std::vector<ScreenImage> screens, const QFuture<QList<WindowList::Window>> &windows, RegionSelector::Purpose purpose)
{
    auto *selector = new RegionSelector(std::move(screens), windows, purpose, settings_.magnifier, this);
    connect(selector, &RegionSelector::selected, this, [this, selector, purpose](const QRectF &region, const QString &app) {
        selector->deleteLater();
        busy_ = false;
        if (purpose == RegionSelector::Purpose::Screenshot) {
            captureTimer_.start();
            deliverScreenshot(selector->grab(region), app);
        } else {
            startRecording(region.toAlignedRect(), selector->scale(region), app);
        }
    });
    connect(selector, &RegionSelector::cancelled, this, [this, selector] {
        selector->deleteLater();
        busy_ = false;
    });
    selector->show();
    qCDebug(lcBlink) << "selection shown after" << captureTimer_.elapsed() << "ms";
}

void App::deliverScreenshot(const QImage &image, const QString &app)
{
    if (image.isNull())
        return;
    // On the clipboard before the PNG exists: pasting waits for the encoder rather than the encoder delaying the copy.
    QFuture<QByteArray> png = runInPool([image] {
        QElapsedTimer timer;
        timer.start();
        QByteArray encoded = encodePng(image);
        qCDebug(lcBlink) << "PNG of" << image.size() << "took" << timer.elapsed() << "ms," << encoded.size() << "bytes";
        return encoded;
    });
    clipboard::setPng(png);
    qCDebug(lcBlink) << "on the clipboard after" << captureTimer_.elapsed() << "ms";

    CaptureInfo info = newCapture(app, u"png"_s, image.size());
    QString path;
    if (settings_.saveScreenshots) {
        QString error;
        const QString folder = captureFolder(settings_.screenshotFolder, settings_.screenshotSubfolder, info, &error);
        if (error.isEmpty()) {
            path = newFile(folder, info.fileName, info.extension, pendingFiles_);
            pendingFiles_.insert(path);
            info.fileName = QFileInfo(path).completeBaseName(); // numbered when the name was taken
        } else {
            notifier_->error(tr("Could not save the screenshot"), tr("The subfolder in the settings is invalid. %1").arg(error));
        }
    }
    // A saved file is previewed by Plasma itself; uploads report their link instead.
    const bool thumbnail = settings_.notifications && path.isEmpty() && !settings_.uploadScreenshots;
    struct Saved {
        QByteArray png;
        QString error;
        QImage thumbnail;
    };
    png.then(QtFuture::Launch::Async,
             [image, path, thumbnail](const QByteArray &png) {
                 return Saved{png, path.isEmpty() ? QString() : writeFile(path, png),
                              thumbnail ? image.scaled(QSize(256, 256), Qt::KeepAspectRatio, Qt::SmoothTransformation) : QImage()};
             })
        .then(this, [this, info, path](const Saved &saved) { screenshotDone(saved.png, info, path, saved.error, saved.thumbnail); });
}

void App::screenshotDone(const QByteArray &png, const CaptureInfo &info, const QString &path, const QString &saveError,
                         const QImage &thumbnail)
{
    pendingFiles_.remove(path); // written now, or failed: the disk is the truth again
    if (!saveError.isEmpty()) {
        // One notification, not an error followed by "copied" with an "Open" action on a file that does not exist.
        notifier_->error(tr("Could not save the screenshot"), tr("%1 The screenshot is still on the clipboard.").arg(saveError));
    } else if (library_ && !path.isEmpty()) {
        library_->rescan();
    }
    if (settings_.uploadScreenshots) {
        if (const std::optional<QString> key = uploadKey(info))
            uploader()->upload(settings_.s3, png, *key, u"image/png"_s, std::bind_front(&App::uploaded, this));
        return;
    }
    if (saveError.isEmpty() && settings_.notifications)
        notifier_->notify(tr("Screenshot copied"), path, path.isEmpty() ? QUrl() : QUrl::fromLocalFile(path), thumbnail);
}

void App::startRecording(const QRect &region, qreal scale, const QString &app)
{
    const CaptureInfo info = newCapture(app, u"mp4"_s);
    QString error;
    const QString folder = captureFolder(settings_.recordingFolder, settings_.recordingSubfolder, info, &error);
    if (!error.isEmpty()) {
        notifier_->error(tr("Recording failed"), tr("The subfolder in the settings is invalid. %1").arg(error));
        return;
    }
    auto *recording = new Recording(region, scale, newFile(folder, info.fileName, info.extension, pendingFiles_), settings_.recording,
                                    this);
    recording_ = recording;
    recordingTimer_.invalidate();
    connect(recording, &Recording::started, this, [this](const QString &encoder) {
        qCDebug(lcBlink) << "recording with" << encoder;
        recordingTimer_.start();
        recordingTicker_.start();
        updateTray();
    });
    connect(recording, &Recording::warning, this, [this](const QString &text) { notifier_->error(u"Recording"_s, text); });
    connect(recording, &Recording::finished, this, [this, info](const QString &path, QSize size) {
        recordingDone();
        deliverRecording(path, {info.time, QFileInfo(path).completeBaseName(), info.extension, size});
    });
    connect(recording, &Recording::aborted, this, &App::recordingDone);
    connect(recording, &Recording::failed, this, [this](const QString &error) {
        recordingDone();
        notifier_->error(tr("Recording failed"), error);
    });
    recording->start();
    updateTray();
}

void App::recordingDone()
{
    if (recording_)
        recording_->deleteLater();
    recording_ = nullptr;
    recordingTicker_.stop();
    updateTray();
    // The file is complete now, or gone.
    if (library_)
        library_->rescan();
    // Queued: the finished and failed handlers deliver the file or report the error right after this returns.
    if (quitting_)
        QMetaObject::invokeMethod(this, [] { QCoreApplication::quit(); }, Qt::QueuedConnection);
}

void App::deliverRecording(const QString &path, const CaptureInfo &info)
{
    clipboard::setFiles({path});
    if (settings_.uploadRecordings) {
        if (const std::optional<QString> key = uploadKey(info))
            uploader()->uploadFile(settings_.s3, path, *key, u"video/mp4"_s, std::bind_front(&App::uploaded, this));
        return;
    }
    if (settings_.notifications)
        notifier_->notify(tr("Recording saved"), path, QUrl::fromLocalFile(path));
}

void App::uploadFile(const QString &path)
{
    const QFileInfo file(path);
    const QString type = QMimeDatabase().mimeTypeForFile(file).name();
    CaptureInfo info;
    info.time = file.birthTime().isValid() ? file.birthTime() : file.lastModified();
    info.fileName = file.completeBaseName();
    info.extension = file.suffix();
    if (type.startsWith("image/"_L1))
        info.size = QImageReader(path).size(); // from the header; videos leave {width} and {height} empty
    if (const std::optional<QString> key = uploadKey(info))
        uploader()->uploadFile(settings_.s3, path, *key, type, std::bind_front(&App::uploaded, this));
}

std::optional<QString> App::uploadKey(const CaptureInfo &info)
{
    if (!settings_.s3.isComplete()) {
        notifier_->error(tr("Not uploaded"), tr("Uploads are on, but the S3 storage settings are incomplete."));
        return std::nullopt;
    }
    QString error;
    QString key = PathPattern::expand(settings_.s3.path, PathPattern::Target::ObjectKey, info, &error);
    if (!error.isEmpty()) {
        notifier_->error(tr("Not uploaded"), tr("The upload path in the settings is invalid. %1").arg(error));
        return std::nullopt;
    }
    return key;
}

Uploader *App::uploader()
{
    if (!uploader_)
        uploader_ = new Uploader(this);
    return uploader_;
}

void App::uploaded(const Uploader::Result &result)
{
    if (!result.error.isEmpty()) {
        notifier_->error(tr("Upload failed"), result.error);
        return;
    }
    if (settings_.copyUrl)
        clipboard::setText(result.url);
    if (settings_.notifications)
        notifier_->notify(settings_.copyUrl ? tr("Link copied") : tr("Uploaded"), result.url, QUrl(result.url));
}

void App::showSettings()
{
    if (dialog_) {
        dialog_->raise();
        dialog_->activateWindow();
        return;
    }
    QList<QAction *> actions;
    for (const Shortcut &shortcut : shortcuts_)
        actions.append(shortcut.action);
    auto *dialog = new SettingsDialog(settings_, actions);
    dialog_ = dialog;
    connect(dialog, &SettingsDialog::applied, this, &App::applySettings);
    dialog->show();
}

void App::showLibrary()
{
    if (library_) {
        library_->raise();
        library_->activateWindow();
        return;
    }
    auto *library = new LibraryWindow(settings_);
    library_ = library;
    connect(library, &LibraryWindow::uploadRequested, this, &App::uploadFile);
    library->show();
}

void App::applySettings(const Settings &settings)
{
    settings_ = settings;
    settings_.save();
    updateAutostart();
    if (library_)
        library_->setSettings(settings_);
}

void App::updateAutostart()
{
    if (const QString error = applyAutostart(settings_.startAtLogin); !error.isEmpty())
        notifier_->error(tr("Could not set Blink to start at login"), error);
}

} // namespace blink
