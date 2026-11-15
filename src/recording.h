#pragma once

#include "settings.h"

#include <QObject>
#include <QProcess>
#include <QRect>
#include <QSize>

#include <memory>
#include <optional>
#include <vector>

class QWindow;

namespace blink {

class Screencast;

// One screen recording: KWin streams the region over PipeWire and blink-recorder encodes it into a file. While
// it runs, a frame drawn just outside the region (so it is not recorded) shows what is being recorded.
class Recording final : public QObject {
    Q_OBJECT

public:
    // `region` is in global logical coordinates; the video goes to `output`, an .mp4 file.
    Recording(const QRect &region, qreal scale, QString output, const RecordingOptions &options, QObject *parent = nullptr);
    ~Recording() override;

    void start();
    void stop();  // keeps the file
    void abort(); // deletes it

Q_SIGNALS:
    void started(const QString &encoder);           // frames are going into the file
    void warning(const QString &text);              // e.g. the microphone could not be opened; recording goes on
    void finished(const QString &path, QSize size); // `size` of the video, in pixels
    void aborted();
    void failed(const QString &error);

private:
    enum class State { Idle, Requesting, Starting, Recording, Stopping, Done };
    // How the recorder said the file ended, before it exited.
    enum class Reported { Nothing, Finished, Aborted };

    void launch(quint64 target, bool isSerial);
    void readEvents();
    void exited(int code, QProcess::ExitStatus status);
    void fail(const QString &error);
    void showFrame();
    void cleanUp();

    QRect region_;
    qreal scale_;
    QString output_;
    RecordingOptions options_;
    State state_ = State::Idle;
    bool aborting_ = false; // abort() or fail(): the recorder is told to delete the file
    Reported reported_ = Reported::Nothing;
    QString encoder_;
    QSize videoSize_;
    QString error_;
    // Set by fail() while the recorder still runs; exited() reports it. Optional because KWin's text may be empty.
    std::optional<QString> failure_;
    std::unique_ptr<Screencast> screencast_;
    QProcess process_;
    std::vector<std::unique_ptr<QWindow>> frame_;
};

} // namespace blink
