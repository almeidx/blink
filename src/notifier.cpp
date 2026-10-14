#include "notifier.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QDebug>
#include <QDesktopServices>

namespace blink {
namespace {

using namespace Qt::StringLiterals;

const auto kService = u"org.freedesktop.Notifications"_s;
const auto kPath = u"/org/freedesktop/Notifications"_s;

// The image-data hint, (iiibiiay): width, height, stride, alpha, bits per sample, channels, RGBA rows.
QVariant imageHint(const QImage &thumbnail)
{
    const QImage image = thumbnail.convertToFormat(QImage::Format_RGBA8888);
    QDBusArgument argument;
    argument.beginStructure();
    argument << image.width() << image.height() << static_cast<int>(image.bytesPerLine()) << true << 8 << 4
             << QByteArray(reinterpret_cast<const char *>(image.constBits()), image.sizeInBytes());
    argument.endStructure();
    return QVariant::fromValue(argument);
}

} // namespace

Notifier::Notifier(QObject *parent) : QObject(parent)
{
    QDBusConnection bus = QDBusConnection::sessionBus();
    bus.connect(kService, kPath, kService, u"ActionInvoked"_s, this, SLOT(onActionInvoked(uint, QString)));
    bus.connect(kService, kPath, kService, u"NotificationClosed"_s, this, SLOT(onClosed(uint, uint)));
}

void Notifier::notify(const QString &summary, const QString &body, const QUrl &target, const QImage &thumbnail)
{
    QVariantMap hints{{u"desktop-entry"_s, u"blink"_s}};
    if (target.isLocalFile())
        hints.insert(u"x-kde-urls"_s, QStringList{target.toString()});
    else if (!thumbnail.isNull())
        hints.insert(u"image-data"_s, imageHint(thumbnail));
    send(summary, body, hints, target);
}

void Notifier::error(const QString &summary, const QString &body)
{
    // Also in the journal: notifications may be off or gone.
    qWarning().noquote() << summary + u": "_s + body;
    send(summary, body, {{u"desktop-entry"_s, u"blink"_s}}, {});
}

void Notifier::send(const QString &summary, const QString &body, const QVariantMap &hints, const QUrl &target)
{
    QDBusMessage message = QDBusMessage::createMethodCall(kService, kPath, kService, u"Notify"_s);
    const QStringList actions = target.isValid() ? QStringList{u"default"_s, tr("Open")} : QStringList{};
    message << u"Blink"_s << 0u << u"blink"_s << summary << body << actions << hints << -1;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, target](QDBusPendingCallWatcher *watcher) {
        watcher->deleteLater();
        const QDBusPendingReply<uint> reply = *watcher;
        if (!reply.isError() && target.isValid())
            targets_.insert(reply.value(), target);
    });
}

void Notifier::onActionInvoked(uint id, const QString &action)
{
    if (action != "default"_L1)
        return;
    if (const QUrl target = targets_.value(id); target.isValid())
        QDesktopServices::openUrl(target);
}

void Notifier::onClosed(uint id, uint reason)
{
    Q_UNUSED(reason)
    targets_.remove(id);
}

} // namespace blink
