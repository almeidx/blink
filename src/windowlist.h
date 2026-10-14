#pragma once

#include <QList>
#include <QObject>
#include <QRectF>
#include <QTimer>

#include <functional>

namespace blink {

// Asks KWin for the windows on screen so region selection can snap to them. No Wayland protocol lists other
// clients' windows, so a short-lived KWin script reports them over D-Bus to the report method of this object,
// which it exports at /WindowList. Takes ~10 ms.
class WindowList final : public QObject {
    Q_OBJECT
    Q_CLASSINFO("D-Bus Interface", "io.github.almeidx.Blink.WindowList")

public:
    struct Window {
        QRectF frame; // global logical coordinates
        QString app;  // as in KWinScreenshot::Callback
    };
    // Topmost first. Empty when KWin does not answer in time.
    using Callback = std::function<void(const QList<Window> &windows)>;

    explicit WindowList(QObject *parent = nullptr);
    ~WindowList() override;

    void query(Callback done); // replaces a query still in flight

    // Called by the script.
    Q_SCRIPTABLE Q_INVOKABLE void report(int request, const QString &windows);

private:
    void finish(const QList<Window> &windows);

    QByteArray script_; // windowlist.js, prefixed per query with the request number and our bus name
    QString scriptPath_;
    int request_ = 0;
    QString plugin_; // KWin plugin name of the script in flight; empty when idle
    Callback done_;
    QTimer timeout_;
};

} // namespace blink
