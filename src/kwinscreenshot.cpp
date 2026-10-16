#include "kwinscreenshot.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDBusUnixFileDescriptor>
#include <QPointer>
#include <QThreadPool>
#include <QtNumeric>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <optional>

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace blink {
namespace {

using namespace Qt::StringLiterals;

constexpr int kReadTimeoutMs = 3000;
// KWin answers at once: it has just sent the screenshot.
constexpr int kWindowInfoTimeoutMs = 1000;
// The window types of KWin's Window::isSpecialWindow(): Desktop, Dock, Toolbar, Splash, Tooltip, Notification,
// OnScreenDisplay, CriticalNotification and AppletPopup. Such windows belong to Plasma or KWin, not an app.
constexpr std::array kSpecialWindowTypes{1, 2, 3, 9, 12, 13, 16, 17, 18};

// Reads exactly `size` bytes. Never waits for EOF: the D-Bus message keeps a duplicate of the write end open.
bool readFully(int fd, uchar *data, qsizetype size, QString *error)
{
    qsizetype done = 0;
    while (done < size) {
        pollfd pfd{fd, POLLIN, 0};
        const int ready = ::poll(&pfd, 1, kReadTimeoutMs);
        if (ready == 0) {
            *error = KWinScreenshot::tr("KWin stopped sending the image");
            return false;
        }
        if (ready < 0) {
            if (errno == EINTR)
                continue;
            *error = qt_error_string(errno);
            return false;
        }
        const ssize_t n = ::read(fd, data + done, static_cast<size_t>(size - done));
        if (n > 0) {
            done += n;
        } else if (n == 0) {
            *error = KWinScreenshot::tr("KWin sent a truncated image");
            return false;
        } else if (errno != EINTR && errno != EAGAIN) {
            *error = qt_error_string(errno);
            return false;
        }
    }
    return true;
}

QImage readImage(int fd, const QVariantMap &results, QString *error)
{
    if (results.value(u"type"_s).toString() != "raw"_L1) {
        *error = KWinScreenshot::tr("KWin sent an image in an unknown encoding");
        return {};
    }
    const int width = results.value(u"width"_s).toInt();
    const int height = results.value(u"height"_s).toInt();
    const qsizetype stride = results.value(u"stride"_s).toLongLong();
    const uint format = results.value(u"format"_s).toUInt();
    if (width <= 0 || height <= 0 || format <= QImage::Format_Invalid || format >= QImage::NImageFormats) {
        *error = KWinScreenshot::tr("KWin sent an invalid image");
        return {};
    }

    // Rows shorter than the pixels they hold would have the read below write past the buffer, and a stride * height
    // that overflows would allocate too little.
    const qsizetype minStride = (static_cast<qsizetype>(width) * QImage::toPixelFormat(static_cast<QImage::Format>(format)).bitsPerPixel()
                                 + 7)
                              / 8;
    qsizetype size = 0;
    if (stride < minStride || qMulOverflow(stride, static_cast<qsizetype>(height), &size)) {
        *error = KWinScreenshot::tr("KWin sent an invalid image");
        return {};
    }

    QImage image(width, height, static_cast<QImage::Format>(format));
    if (image.isNull()) {
        *error = KWinScreenshot::tr("Out of memory");
        return {};
    }
    if (image.bytesPerLine() != stride) {
        // KWin's stride is normally QImage's own; when it isn't, wrap its layout rather than copying row by row.
        auto *buffer = static_cast<uchar *>(std::malloc(static_cast<size_t>(size)));
        if (!buffer) {
            *error = KWinScreenshot::tr("Out of memory");
            return {};
        }
        image = QImage(buffer, width, height, stride, static_cast<QImage::Format>(format), std::free, buffer);
        if (image.isNull()) {
            // QImage takes ownership only of a buffer it accepts.
            std::free(buffer);
            *error = KWinScreenshot::tr("KWin sent an invalid image");
            return {};
        }
    }
    if (!readFully(fd, image.bits(), size, error))
        return {};
    image.setDevicePixelRatio(results.value(u"scale"_s, 1.0).toDouble());
    return image;
}

// Which app a captured window belongs to.
QDBusPendingReply<QVariantMap> windowInfo(const QString &id)
{
    QDBusMessage message = QDBusMessage::createMethodCall(u"org.kde.KWin"_s, u"/KWin"_s, u"org.kde.KWin"_s, u"getWindowInfo"_s);
    message << id;
    return QDBusConnection::sessionBus().asyncCall(message, kWindowInfoTimeoutMs);
}

// Waits for windowInfo()'s answer: off the UI thread only.
QString appOf(QDBusPendingReply<QVariantMap> info)
{
    info.waitForFinished();
    if (!info.isValid())
        return {};
    const QVariantMap window = info.value();
    if (std::ranges::contains(kSpecialWindowTypes, window.value(u"type"_s).toInt()))
        return {};
    const QString desktopFile = window.value(u"desktopFile"_s).toString();
    return desktopFile.isEmpty() ? window.value(u"resourceClass"_s).toString() : desktopFile;
}

} // namespace

KWinScreenshot::KWinScreenshot(QObject *parent) : QObject(parent) {}

void KWinScreenshot::captureScreen(const QString &name, const Options &options, Callback done)
{
    call(u"CaptureScreen"_s, {name}, options, std::move(done));
}

void KWinScreenshot::captureWorkspace(const Options &options, Callback done)
{
    call(u"CaptureWorkspace"_s, {}, options, std::move(done));
}

void KWinScreenshot::captureActiveWindow(const Options &options, Callback done)
{
    call(u"CaptureActiveWindow"_s, {}, options, std::move(done));
}

void KWinScreenshot::call(const QString &method, QVariantList arguments, const Options &options, Callback done)
{
    int fds[2];
    if (::pipe2(fds, O_CLOEXEC) != 0) {
        done({}, {}, qt_error_string(errno));
        return;
    }
    // Fewer wakeups for the 33 MB of a 4K screen; fails harmlessly above /proc/sys/fs/pipe-max-size.
    ::fcntl(fds[0], F_SETPIPE_SZ, 1u << 20); // NOLINT(bugprone-signed-bitwise)

    arguments.append(QVariantMap{
        {u"native-resolution"_s, true},
        {u"include-cursor"_s, options.cursor},
        {u"include-decoration"_s, options.decorations},
        {u"include-shadow"_s, options.shadow},
    });
    arguments.append(QVariant::fromValue(QDBusUnixFileDescriptor(fds[1])));
    ::close(fds[1]); // the message carries its own duplicate

    QDBusMessage message = QDBusMessage::createMethodCall(u"org.kde.KWin.ScreenShot2"_s, u"/org/kde/KWin/ScreenShot2"_s,
                                                          u"org.kde.KWin.ScreenShot2"_s, method);
    message.setArguments(arguments);
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [this, fd = fds[0], done = std::move(done)](QDBusPendingCallWatcher *watcher) {
                watcher->deleteLater();
                const QDBusPendingReply<QVariantMap> reply = *watcher;
                if (reply.isError()) {
                    ::close(fd);
                    if (reply.error().name() == "org.kde.KWin.ScreenShot2.Error.NoAuthorized"_L1)
                        done({}, {},
                             tr("KWin only lets apps take screenshots when a desktop file has Exec=%1.")
                                 .arg(QCoreApplication::applicationFilePath()));
                    else
                        done({}, {}, reply.error().message());
                    return;
                }
                // KWin writes the pixels from a thread after replying; read them off the UI thread. For a window, KWin
                // says which app it belongs to meanwhile.
                const QVariantMap results = reply.value();
                std::optional<QDBusPendingReply<QVariantMap>> info;
                if (const QString id = results.value(u"windowId"_s).toString(); !id.isEmpty())
                    info = windowInfo(id);
                QThreadPool::globalInstance()->start([self = QPointer(this), fd, results, info, done] {
                    QString error;
                    const QImage image = readImage(fd, results, &error);
                    ::close(fd);
                    const QString app = info ? appOf(*info) : QString();
                    QMetaObject::invokeMethod(
                        qApp,
                        [self, image, app, error, done] {
                            if (self)
                                done(image, app, error);
                        },
                        Qt::QueuedConnection);
                });
            });
}

} // namespace blink
