#pragma once

#include <QHash>
#include <QImage>
#include <QObject>
#include <QUrl>

namespace blink {

// Desktop notifications over org.freedesktop.Notifications.
class Notifier final : public QObject {
    Q_OBJECT

public:
    explicit Notifier(QObject *parent = nullptr);

    // Clicking the notification opens `target`. A local file target is previewed by Plasma itself (x-kde-urls);
    // otherwise `thumbnail`, if any, is shown.
    void notify(const QString &summary, const QString &body, const QUrl &target = {}, const QImage &thumbnail = {});
    void error(const QString &summary, const QString &body);

private:
    Q_INVOKABLE void onActionInvoked(uint id, const QString &action);
    Q_INVOKABLE void onClosed(uint id, uint reason);
    void send(const QString &summary, const QString &body, const QVariantMap &hints, const QUrl &target);

    QHash<uint, QUrl> targets_;
};

} // namespace blink
