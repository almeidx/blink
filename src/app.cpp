#include "app.h"

#include "appname.h"
#include "clipboard.h"
#include "kwinscreenshot.h"
#include "notifier.h"
#include "pngencoder.h"
#include "recording.h"
#include "regionselector.h"
#include "settingsdialog.h"
#include "uploader.h"
#include "uploadpath.h"
#include "windowlist.h"

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
#include <QLoggingCategory>
#include <QMenu>
#include <QPromise>
#include <QSaveFile>
#include <QScreen>
#include <QSettings>
#include <QStandardPaths>
#include <QSystemTrayIcon>
#include <QThreadPool>

#include <algorithm>
#include <array>
#include <chrono>
#include <utility>

namespace blink {
namespace {

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

// Timings of the capture path: QT_LOGGING_RULES="blink.debug=true".
Q_LOGGING_CATEGORY(lcBlink, "blink", QtInfoMsg)

QIcon appIcon()
{
    return QIcon::fromTheme(u"blink"_s, QIcon(u":/icons/hicolor/scalable/apps/blink.svg"_s));
}

// <folder>/<name>.<extension>, numbered when that exists.
QString newFile(const QString &folder, const QString &name, const QString &extension)
{
    const QString stem = folder + u'/' + name;
    QString path = stem + u'.' + extension;
    for (int n = 2; QFileInfo::exists(path); ++n)
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

// An Exec value per the Desktop Entry spec: quoted when needed, then escaped again as a string value.
QString desktopExec(const QString &program)
{
    static const QString reserved = u" \t\n\"'\\><~|&;$*?#()`"_s;
    if (std::ranges::none_of(program, [](QChar c) { return reserved.contains(c); }))
        return program;
    QString quoted = u"\""_s;
    for (const QChar c : program) {
        if (c == u'\\')
            quoted += u"\\\\\\\\"_s;
        else if (c == u'"' || c == u'`' || c == u'$')
            quoted += u"\\\\"_s + c;
        else
            quoted += c;
    }
    return quoted + u'"';
}

} // namespace

App::App() : screenshot_(new KWinScreenshot(this)), windows_(new WindowList(this)), notifier_(new Notifier(this))
{
    const bool firstRun = !QFileInfo::exists(QSettings().fileName());
    settings_ = Settings::load();
    if (firstRun)
        settings_.save();
    applyAutostart();

    createActions();
    registerShortcuts();
    createTray();
    QDBusConnection::sessionBus().registerObject(u"/Blink"_s, this, QDBusConnection::ExportScriptableInvokables);

    recordingTicker_.setInterval(1s);
    connect(&recordingTicker_, &QTimer::timeout, this, &App::updateTray);

    if (firstRun) {
        QString keys;
        for (QAction *action : {regionAction_, fullscreenAction_, windowAction_, recordAction_}) {
            if (const QList<QKeySequence> shortcut = KGlobalAccel::self()->shortcut(action); !shortcut.isEmpty())
                keys += u'\n' + shortcut.first().toString(QKeySequence::NativeText) + u": "_s + action->text();
        }
        notifier_->notify(tr("Blink is in the system tray"), tr("Shortcuts can be changed in System Settings.") + keys);
    }
}

App::~App() = default;

void App::createActions()
{
    regionAction_ = new QAction(QIcon::fromTheme(u"select-rectangular"_s), tr("Capture Region"), this);
    regionAction_->setObjectName(u"capture-region"_s);
    connect(regionAction_, &QAction::triggered, this, &App::captureRegion);

    windowAction_ = new QAction(QIcon::fromTheme(u"window"_s), tr("Capture Active Window"), this);
    windowAction_->setObjectName(u"capture-window"_s);
    connect(windowAction_, &QAction::triggered, this, &App::captureWindow);

    fullscreenAction_ = new QAction(QIcon::fromTheme(u"view-fullscreen"_s), tr("Capture Full Screen"), this);
    fullscreenAction_->setObjectName(u"capture-fullscreen"_s);
    connect(fullscreenAction_, &QAction::triggered, this, &App::captureFullscreen);

    // The global shortcut's name stays fixed; the menu entry says what a click does now.
    recordAction_ = new QAction(QIcon::fromTheme(u"media-record"_s), tr("Start or Stop Recording"), this);
    recordAction_->setObjectName(u"toggle-recording"_s);
    connect(recordAction_, &QAction::triggered, this, &App::toggleRecording);
    recordMenuAction_ = new QAction(this);
    connect(recordMenuAction_, &QAction::triggered, this, &App::toggleRecording);

    abortAction_ = new QAction(QIcon::fromTheme(u"dialog-cancel"_s), tr("Discard Recording"), this);
    connect(abortAction_, &QAction::triggered, this, [this] {
        if (recording_)
            recording_->abort();
    });
}

void App::registerShortcuts()
{
    const std::array<std::pair<QAction *, QKeySequence>, 4> shortcuts{{
        {regionAction_, QKeySequence(Qt::Key_Print)},
        {fullscreenAction_, QKeySequence(Qt::SHIFT | Qt::Key_Print)},
        {windowAction_, QKeySequence(Qt::META | Qt::Key_Print)},
        {recordAction_, QKeySequence(Qt::META | Qt::SHIFT | Qt::Key_R)},
    }};

    // Spectacle holds these keys by default and KGlobalAccel gives a key to one owner only, so take them from
    // it, once: later changes in System Settings are the user's.
    QSettings state;
    if (!state.value(u"State/shortcutsClaimed"_s).toBool()) {
        for (const auto &[action, key] : shortcuts) {
            const QList<KGlobalShortcutInfo> owners = KGlobalAccel::globalShortcutsByKey(key);
            if (std::ranges::any_of(
                    owners, [](const KGlobalShortcutInfo &owner) { return owner.componentUniqueName() == "org.kde.spectacle.desktop"_L1; }))
                KGlobalAccel::stealShortcutSystemwide(key);
        }
        state.setValue(u"State/shortcutsClaimed"_s, true);
        state.sync();
        QFile::setPermissions(state.fileName(), QFile::ReadOwner | QFile::WriteOwner);
    }
    for (const auto &[action, key] : shortcuts) {
        KGlobalAccel::self()->setDefaultShortcut(action, {key});
        KGlobalAccel::self()->setShortcut(action, {key});
    }

    // Menu entries show the keys; QAction shortcuts are inert here, since the menu is exported to the panel.
    auto showKeys = [this](QAction *action, const QList<QKeySequence> &keys) {
        const QKeySequence key = keys.value(0);
        (action == recordAction_ ? recordMenuAction_ : action)->setShortcut(key);
    };
    for (const auto &[action, key] : shortcuts)
        showKeys(action, KGlobalAccel::self()->shortcut(action));
    connect(KGlobalAccel::self(), &KGlobalAccel::globalShortcutChanged, this,
            [showKeys](QAction *action, const QKeySequence &key) { showKeys(action, {key}); });
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
    menu_->addAction(QIcon::fromTheme(u"configure"_s), u"Settings…"_s, this, &App::showSettings);
    menu_->addAction(QIcon::fromTheme(u"application-exit"_s), u"Quit"_s, qApp, &QCoreApplication::quit);

    tray_ = new QSystemTrayIcon(appIcon(), this);
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
    tray_->setIcon(recording ? QIcon::fromTheme(u"media-record"_s, appIcon()) : appIcon());
    if (!recording)
        tray_->setToolTip(u"Blink"_s);
    else if (recordingTimer_.isValid())
        tray_->setToolTip(tr("Blink: recording %1. Click to stop.").arg(elapsedText(std::chrono::milliseconds(recordingTimer_.elapsed()))));
    else
        tray_->setToolTip(tr("Blink: starting to record"));
}

void App::captureRegion()
{
    selectRegion(Purpose::Screenshot);
}

void App::captureWindow()
{
    if (busy_)
        return;
    busy_ = true;
    captureTimer_.start();
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
    if (busy_)
        return;
    busy_ = true;
    captureTimer_.start();
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
        selectRegion(Purpose::Recording);
}

void App::selectRegion(Purpose purpose)
{
    if (busy_ || (purpose == Purpose::Recording && recording_))
        return;
    busy_ = true;
    captureTimer_.start();

    // Runs while the screens are captured; the selection takes the windows whenever they come.
    knownWindows_.clear();
    windowsKnown_ = false;
    windows_->query([this](const QList<WindowList::Window> &windows) {
        qCDebug(lcBlink) << windows.size() << "windows after" << captureTimer_.elapsed() << "ms";
        knownWindows_ = windows;
        windowsKnown_ = true;
        if (selector_)
            selector_->setWindows(windows);
    });

    struct Pending {
        std::vector<ScreenImage> shots;
        qsizetype remaining = 0;
        QString error;
    };
    const QList<QScreen *> screens = QGuiApplication::screens();
    auto pending = std::make_shared<Pending>();
    pending->shots.resize(screens.size());
    pending->remaining = screens.size();
    for (qsizetype i = 0; i < screens.size(); ++i) {
        pending->shots[i].screen = screens[i];
        screenshot_->captureScreen(screens[i]->name(), {},
                                   [this, pending, i, purpose](const QImage &image, const QString &, const QString &error) {
                                       if (error.isEmpty())
                                           pending->shots[i].image = image;
                                       else
                                           pending->error = error;
                                       if (--pending->remaining > 0)
                                           return;
                                       qCDebug(lcBlink) << "screens captured after" << captureTimer_.elapsed() << "ms";
                                       const QList<QScreen *> current = QGuiApplication::screens();
                                       const bool screensChanged = std::ranges::any_of(
                                           pending->shots, [&](const ScreenImage &shot) { return !current.contains(shot.screen); });
                                       if (!pending->error.isEmpty() || screensChanged) {
                                           busy_ = false;
                                           if (!pending->error.isEmpty())
                                               notifier_->error(tr("Could not capture the screen"), pending->error);
                                           return;
                                       }
                                       showSelector(std::move(pending->shots), purpose);
                                   });
    }
}

void App::showSelector(std::vector<ScreenImage> shots, Purpose purpose)
{
    auto *selector = new RegionSelector(
        std::move(shots), purpose == Purpose::Recording ? RegionSelector::Purpose::Recording : RegionSelector::Purpose::Screenshot,
        settings_.magnifier, this);
    selector_ = selector;
    if (windowsKnown_)
        selector->setWindows(knownWindows_);
    connect(selector, &RegionSelector::selected, this, [this, selector, purpose](const QRectF &region, const QString &app) {
        selector->deleteLater();
        busy_ = false;
        if (purpose == Purpose::Screenshot) {
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
    auto promise = std::make_shared<QPromise<QByteArray>>();
    promise->start();
    clipboard::setPng(promise->future());
    qCDebug(lcBlink) << "on the clipboard after" << captureTimer_.elapsed() << "ms";

    const QDateTime time = QDateTime::currentDateTime();
    const QString name = captureName(time, appName(app));
    const QString path = settings_.saveScreenshots ? newFile(settings_.screenshotFolder, name, u"png"_s) : QString();
    const UploadInfo info{time, path.isEmpty() ? name : QFileInfo(path).completeBaseName(), u"png"_s, image.size()};
    // A saved file is previewed by Plasma itself; uploads report their link instead.
    const bool thumbnail = settings_.notifications && path.isEmpty() && !settings_.uploadScreenshots;
    QThreadPool::globalInstance()->start([self = QPointer(this), image, promise, path, info, thumbnail] {
        QElapsedTimer timer;
        timer.start();
        const QByteArray png = encodePng(image);
        promise->addResult(png);
        promise->finish();
        qCDebug(lcBlink) << "PNG of" << image.size() << "took" << timer.elapsed() << "ms," << png.size() << "bytes";
        const QString saveError = path.isEmpty() ? QString() : writeFile(path, png);
        const QImage small = thumbnail ? image.scaled(QSize(256, 256), Qt::KeepAspectRatio, Qt::SmoothTransformation) : QImage();
        QMetaObject::invokeMethod(
            qApp,
            [self, png, info, path, saveError, small] {
                if (self)
                    self->screenshotDone(png, info, path, saveError, small);
            },
            Qt::QueuedConnection);
    });
}

void App::screenshotDone(const QByteArray &png, const UploadInfo &info, const QString &path, const QString &saveError,
                         const QImage &thumbnail)
{
    if (!saveError.isEmpty())
        notifier_->error(tr("Could not save the screenshot"), saveError);
    if (settings_.uploadScreenshots) {
        upload(png, {}, info, u"image/png"_s);
        return;
    }
    if (settings_.notifications)
        notifier_->notify(tr("Screenshot copied"), path, path.isEmpty() ? QUrl() : QUrl::fromLocalFile(path), thumbnail);
}

void App::startRecording(const QRect &region, qreal scale, const QString &app)
{
    const QDateTime time = QDateTime::currentDateTime();
    Recording::Options options;
    options.output = newFile(settings_.recordingFolder, captureName(time, appName(app)), u"mp4"_s);
    options.fps = settings_.fps;
    options.quality = settings_.quality;
    options.encoder = settings_.encoder;
    options.cursor = settings_.recordCursor;
    options.systemAudio = settings_.systemAudio;
    options.microphone = settings_.microphone;

    auto *recording = new Recording(region, scale, std::move(options), this);
    recording_ = recording;
    recordingTimer_.invalidate();
    connect(recording, &Recording::started, this, [this](const QString &encoder) {
        qCDebug(lcBlink) << "recording with" << encoder;
        recordingTimer_.start();
        recordingTicker_.start();
        updateTray();
    });
    connect(recording, &Recording::warning, this, [this](const QString &text) { notifier_->error(u"Recording"_s, text); });
    connect(recording, &Recording::finished, this, [this, recording, time](const QString &path) {
        const UploadInfo info{time, QFileInfo(path).completeBaseName(), u"mp4"_s, recording->videoSize()};
        recordingDone();
        deliverRecording(path, info);
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
}

void App::deliverRecording(const QString &path, const UploadInfo &info)
{
    clipboard::setFile(path);
    if (settings_.uploadRecordings) {
        upload({}, path, info, u"video/mp4"_s);
        return;
    }
    if (settings_.notifications)
        notifier_->notify(tr("Recording saved"), path, QUrl::fromLocalFile(path));
}

void App::upload(const QByteArray &data, const QString &path, const UploadInfo &info, const QString &contentType)
{
    if (!settings_.s3.isComplete()) {
        notifier_->error(tr("Not uploaded"), tr("Uploads are on, but the S3 storage settings are incomplete."));
        return;
    }
    QString error;
    const QString key = UploadPath::expand(settings_.s3.path, info, &error);
    if (key.isEmpty()) {
        notifier_->error(tr("Not uploaded"), tr("The upload path in the settings is invalid. %1").arg(error));
        return;
    }
    if (!uploader_)
        uploader_ = new Uploader(this);
    auto done = [this](const Uploader::Result &result) {
        if (!result.error.isEmpty()) {
            notifier_->error(tr("Upload failed"), result.error);
            return;
        }
        if (settings_.copyUrl)
            clipboard::setText(result.url);
        if (settings_.notifications)
            notifier_->notify(settings_.copyUrl ? tr("Link copied") : tr("Uploaded"), result.url, QUrl(result.url));
    };
    if (path.isEmpty())
        uploader_->upload(settings_.s3, data, key, contentType, std::move(done));
    else
        uploader_->uploadFile(settings_.s3, path, key, contentType, std::move(done));
}

void App::showSettings()
{
    if (dialog_) {
        dialog_->raise();
        dialog_->activateWindow();
        return;
    }
    auto *dialog = new SettingsDialog(settings_, {regionAction_, windowAction_, fullscreenAction_, recordAction_});
    dialog_ = dialog;
    connect(dialog, &SettingsDialog::applied, this, &App::applySettings);
    dialog->show();
}

void App::applySettings(const Settings &settings)
{
    settings_ = settings;
    settings_.save();
    applyAutostart();
}

void App::applyAutostart() const
{
    const QString path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + u"/autostart/blink.desktop"_s;
    if (!settings_.startAtLogin) {
        QFile::remove(path);
        return;
    }
    // An existing entry is left alone, so running a build tree once does not make it the one that starts at login.
    if (QFileInfo::exists(path))
        return;
    const QByteArray entry = "[Desktop Entry]\nType=Application\nName=Blink\nComment=Screenshots and screen recordings\nIcon=blink\nExec="
                           + desktopExec(QCoreApplication::applicationFilePath()).toUtf8() + '\n';
    if (const QString error = writeFile(path, entry); !error.isEmpty())
        notifier_->error(tr("Could not set Blink to start at login"), error);
}

} // namespace blink
