#include "windowlist.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDBusPendingReply>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

#include <chrono>

namespace blink {
namespace {

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

// Normally answered in ~10 ms; the region selection is already on screen while this runs.
constexpr auto kReplyTimeout = 500ms;

QDBusMessage scriptingCall(const QString &path, const QString &interface, const QString &method)
{
    return QDBusMessage::createMethodCall(u"org.kde.KWin"_s, path, interface, method);
}

void unloadScript(const QString &plugin)
{
    QDBusMessage message = scriptingCall(u"/Scripting"_s, u"org.kde.kwin.Scripting"_s, u"unloadScript"_s);
    message << plugin;
    QDBusConnection::sessionBus().call(message, QDBus::NoBlock);
}

} // namespace

WindowList::WindowList(QObject *parent)
    : QObject(parent), scriptPath_(QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation) + u"/blink-windowlist.js"_s)
{
    QFile script(u":/windowlist.js"_s);
    if (script.open(QIODevice::ReadOnly))
        script_ = script.readAll();
    QDBusConnection::sessionBus().registerObject(u"/WindowList"_s, this, QDBusConnection::ExportScriptableInvokables);
    timeout_.setSingleShot(true);
    timeout_.setInterval(kReplyTimeout);
    connect(&timeout_, &QTimer::timeout, this, [this] { finish({}); });
}

WindowList::~WindowList()
{
    if (!plugin_.isEmpty())
        unloadScript(plugin_);
    QFile::remove(scriptPath_);
}

void WindowList::query(Callback done)
{
    if (!plugin_.isEmpty())
        unloadScript(plugin_);
    done_ = std::move(done);
    ++request_;
    // KWin refuses to load a second script under a name that is still loaded.
    plugin_ = u"blink-windowlist-"_s + QString::number(request_);
    timeout_.start();

    QFile file(scriptPath_);
    const QByteArray prefix = "const blinkService = \"" + QDBusConnection::sessionBus().baseService().toLatin1()
                            + "\";\nconst blinkRequest = " + QByteArray::number(request_) + ";\n";
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate) || file.write(prefix + script_) < 0) {
        finish({});
        return;
    }
    file.close();

    QDBusMessage load = scriptingCall(u"/Scripting"_s, u"org.kde.kwin.Scripting"_s, u"loadScript"_s);
    load << scriptPath_ << plugin_;
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(load), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this, [this, request = request_](QDBusPendingCallWatcher *watcher) {
        watcher->deleteLater();
        const QDBusPendingReply<int> reply = *watcher;
        if (request != request_)
            return;
        if (reply.isError() || reply.value() < 0) {
            finish({});
            return;
        }
        const QDBusMessage run = scriptingCall(u"/Scripting/Script"_s + QString::number(reply.value()), u"org.kde.kwin.Script"_s, u"run"_s);
        QDBusConnection::sessionBus().call(run, QDBus::NoBlock);
    });
}

void WindowList::report(int request, const QString &windows)
{
    if (request != request_ || plugin_.isEmpty())
        return;
    const QJsonArray array = QJsonDocument::fromJson(windows.toUtf8()).array();
    QList<Window> found;
    found.reserve(array.size());
    for (const auto &value : array) {
        const QJsonArray w = value.toArray();
        if (w.size() == 5)
            found.append({QRectF(w[0].toDouble(), w[1].toDouble(), w[2].toDouble(), w[3].toDouble()), w[4].toString()});
    }
    finish(found);
}

void WindowList::finish(const QList<Window> &windows)
{
    timeout_.stop();
    if (!plugin_.isEmpty()) {
        unloadScript(plugin_);
        plugin_.clear();
    }
    if (Callback done = std::exchange(done_, {}))
        done(windows);
}

} // namespace blink
