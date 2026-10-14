#pragma once

#include "settings.h"

#include <QObject>
#include <QProcess>
#include <QRect>

#include <memory>
#include <vector>

class QWindow;

namespace blink {

class Screencast;

// One screen recording: KWin streams the region over PipeWire and blink-recorder encodes it into a file. While
// it runs, a frame drawn just outside the region (so it is not recorded) shows what is being recorded.
class Recording final : public QObject {
    Q_OBJECT

public:
    struct Options {
        QString output; // .mp4
        int fps = 60;
        Quality quality = Quality::High;
        EncoderPreference encoder = EncoderPreference::Auto;
        bool cursor = true;
        bool systemAudio = true;
        bool microphone = false;
    };

    // `region` is in global logical coordinates.
    Recording(const QRect &region, qreal scale, Options options, QObject *parent = nullptr);
    ~Recording() override;

    void start();
    void stop();  // keeps the file
    void abort(); // deletes it

Q_SIGNALS:
    void started(const QString &encoder); // frames are going into the file
    void warning(const QString &text);    // e.g. the microphone could not be opened; recording goes on
    void finished(const QString &path);
    void aborted();
    void failed(const QString &error);

private:
    enum class State { Idle, Requesting, Starting, Recording, Stopping, Done };

    void launch(quint64 target, bool isSerial);
    void readEvents();
    void exited(int code, QProcess::ExitStatus status);
    void fail(const QString &error);
    void showFrame();
    void cleanUp();

    QRect region_;
    qreal scale_;
    Options options_;
    State state_ = State::Idle;
    bool aborting_ = false;
    QString encoder_;
    QString error_;
    std::unique_ptr<Screencast> screencast_;
    QProcess process_;
    std::vector<std::unique_ptr<QWindow>> frame_;
};

} // namespace blink
