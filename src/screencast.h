#pragma once

#include <QObject>
#include <QRect>

#include <memory>

namespace blink {

class ScreencastGlobal;
class ScreencastStream;

// A PipeWire stream of part of the screen, rendered by KWin (zkde_screencast_unstable_v1). KWin only offers the
// protocol to executables that a desktop file lists with X-KDE-Wayland-Interfaces=zkde_screencast_unstable_v1.
class Screencast final : public QObject {
    Q_OBJECT

public:
    explicit Screencast(QObject *parent = nullptr);
    ~Screencast() override; // ends the stream

    // `region` is in global logical coordinates; the stream has its size times `scale`. Answers with ready() or
    // failed().
    void start(const QRect &region, qreal scale, bool cursor);

Q_SIGNALS:
    // The PipeWire object serial of the stream, or its node id with older KWin.
    void ready(quint64 target, bool isSerial);
    void failed(const QString &error);
    void closed(); // KWin ended the stream

private:
    friend class ScreencastStream;

    std::unique_ptr<ScreencastGlobal> global_;
    std::unique_ptr<ScreencastStream> stream_;
};

} // namespace blink
