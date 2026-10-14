#include "recording.h"

#include "recorderprotocol.h"
#include "screencast.h"

#include <LayerShellQt/Window>

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QGuiApplication>
#include <QPainter>
#include <QRasterWindow>
#include <QScreen>

#include <string_view>

namespace blink {
namespace {

using namespace Qt::StringLiterals;

constexpr int kFrameWidth = 2; // logical
const QColor kFrameColor(0xda, 0x44, 0x53);

QLatin1StringView latin1(std::string_view text)
{
    return {text.data(), static_cast<qsizetype>(text.size())};
}

// A build tree runs the recorder built next to it.
QString recorderPath()
{
    if (QString local = QCoreApplication::applicationDirPath() + u"/blink-recorder"_s; QFileInfo(local).isExecutable())
        return local;
    return QStringLiteral(BLINK_RECORDER_PATH);
}

std::string_view qualityArgument(Quality quality)
{
    switch (quality) {
    case Quality::Low:
        return recorder::kQualityLow;
    case Quality::Medium:
        return recorder::kQualityMedium;
    case Quality::High:
        break;
    }
    return recorder::kQualityHigh;
}

std::string_view encoderArgument(EncoderPreference encoder)
{
    switch (encoder) {
    case EncoderPreference::Nvenc:
        return recorder::kEncoderNvenc;
    case EncoderPreference::Vaapi:
        return recorder::kEncoderVaapi;
    case EncoderPreference::Software:
        return recorder::kEncoderSoftware;
    case EncoderPreference::Auto:
        break;
    }
    return recorder::kEncoderAuto;
}

// One side of the frame: an input-transparent layer-shell strip, placed by its margins on its screen.
class FrameStrip final : public QRasterWindow {
public:
    FrameStrip(QScreen *screen, const QRect &rect)
    {
        setScreen(screen);
        setFlag(Qt::WindowTransparentForInput);
        resize(rect.size());
        auto *layer = LayerShellQt::Window::get(this);
        layer->setScope(u"blink-recording-frame"_s);
        layer->setLayer(LayerShellQt::Window::LayerOverlay);
        layer->setAnchors({LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorLeft});
        layer->setMargins(QMargins(rect.x() - screen->geometry().x(), rect.y() - screen->geometry().y(), 0, 0));
        layer->setDesiredSize(rect.size());
        layer->setExclusiveZone(-1);
        layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityNone);
        layer->setScreen(screen);
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        Q_UNUSED(event)
        QPainter(this).fillRect(QRect(QPoint(), size()), kFrameColor);
    }
};

} // namespace

Recording::Recording(const QRect &region, qreal scale, Options options, QObject *parent)
    : QObject(parent), region_(region), scale_(scale), options_(std::move(options))
{
    process_.setProcessChannelMode(QProcess::ForwardedErrorChannel);
    connect(&process_, &QProcess::readyReadStandardOutput, this, &Recording::readEvents);
    connect(&process_, &QProcess::finished, this, &Recording::exited);
    connect(&process_, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
        if (error == QProcess::FailedToStart)
            fail(tr("Could not start blink-recorder: %1").arg(process_.errorString()));
    });
}

Recording::~Recording()
{
    // QProcess would SIGKILL the recorder and leave a broken file; let it finish instead.
    if (process_.state() != QProcess::NotRunning) {
        process_.write(aborting_ ? "abort\n" : "stop\n");
        process_.closeWriteChannel();
        process_.waitForFinished(10'000);
    }
}

void Recording::start()
{
    QDir().mkpath(QFileInfo(options_.output).absolutePath());
    screencast_ = std::make_unique<Screencast>();
    connect(screencast_.get(), &Screencast::ready, this, &Recording::launch);
    connect(screencast_.get(), &Screencast::failed, this, &Recording::fail);
    // KWin ended the stream (its screen went away, for one): keep what was recorded.
    connect(screencast_.get(), &Screencast::closed, this, &Recording::stop);
    state_ = State::Requesting;
    screencast_->start(region_, scale_, options_.cursor);
}

void Recording::launch(quint64 target, bool isSerial)
{
    if (state_ != State::Requesting)
        return;
    state_ = State::Starting;
    QStringList arguments{latin1(isSerial ? recorder::kOptSerial : recorder::kOptNode),
                          QString::number(target),
                          latin1(recorder::kOptOutput),
                          options_.output,
                          latin1(recorder::kOptFps),
                          QString::number(options_.fps),
                          latin1(recorder::kOptQuality),
                          latin1(qualityArgument(options_.quality)),
                          latin1(recorder::kOptEncoder),
                          latin1(encoderArgument(options_.encoder))};
    if (options_.systemAudio)
        arguments.append(latin1(recorder::kOptSystemAudio));
    if (options_.microphone)
        arguments.append(latin1(recorder::kOptMicrophone));
    process_.start(recorderPath(), arguments);
}

void Recording::readEvents()
{
    while (process_.canReadLine()) {
        const QString line = QString::fromUtf8(process_.readLine()).trimmed();
        const qsizetype space = line.indexOf(u' ');
        const QStringView event = space < 0 ? QStringView(line) : QStringView(line).left(space);
        const QString text = space < 0 ? QString() : line.mid(space + 1);
        if (event == latin1(recorder::kEvEncoder)) {
            // "<name> <width>x<height>"
            const QStringList parts = text.split(u' ');
            encoder_ = parts.value(0);
            const QStringList size = parts.value(1).split(u'x');
            videoSize_ = QSize(size.value(0).toInt(), size.value(1).toInt());
        } else if (event == latin1(recorder::kEvRecording)) {
            if (state_ == State::Starting) {
                state_ = State::Recording;
                showFrame();
                Q_EMIT started(encoder_);
            }
        } else if (event == latin1(recorder::kEvWarning)) {
            Q_EMIT warning(text);
        } else if (event == latin1(recorder::kEvError)) {
            error_ = text;
        }
    }
}

void Recording::stop()
{
    switch (state_) {
    case State::Requesting:
        // Nothing recorded yet.
        cleanUp();
        state_ = State::Done;
        Q_EMIT aborted();
        break;
    case State::Starting:
    case State::Recording:
        state_ = State::Stopping;
        process_.write(aborting_ ? "abort\n" : "stop\n");
        break;
    case State::Idle:
    case State::Stopping:
    case State::Done:
        break;
    }
}

void Recording::abort()
{
    aborting_ = true;
    stop();
}

void Recording::exited(int code, QProcess::ExitStatus status)
{
    readEvents();
    cleanUp();
    if (state_ == State::Done)
        return;
    state_ = State::Done;
    if (status == QProcess::NormalExit && code == 0) {
        if (aborting_)
            Q_EMIT aborted();
        else
            Q_EMIT finished(options_.output);
        return;
    }
    Q_EMIT failed(error_.isEmpty() ? u"blink-recorder stopped unexpectedly."_s : error_);
}

void Recording::fail(const QString &error)
{
    if (state_ == State::Done)
        return;
    state_ = State::Done;
    if (process_.state() != QProcess::NotRunning)
        process_.write("abort\n");
    cleanUp();
    Q_EMIT failed(error);
}

void Recording::showFrame()
{
    const QRect outer = region_.adjusted(-kFrameWidth, -kFrameWidth, kFrameWidth, kFrameWidth);
    const QRect strips[] = {
        QRect(outer.left(), outer.top(), outer.width(), kFrameWidth),
        QRect(outer.left(), region_.bottom() + 1, outer.width(), kFrameWidth),
        QRect(outer.left(), region_.top(), kFrameWidth, region_.height()),
        QRect(region_.right() + 1, region_.top(), kFrameWidth, region_.height()),
    };
    const auto screens = QGuiApplication::screens();
    for (const QRect &strip : strips) {
        for (QScreen *screen : screens) {
            if (const QRect part = strip & screen->geometry(); !part.isEmpty()) {
                frame_.push_back(std::make_unique<FrameStrip>(screen, part));
                frame_.back()->show();
            }
        }
    }
}

void Recording::cleanUp()
{
    frame_.clear();
    // Ends the stream. Deferred: this can run from inside one of the stream's own Wayland event handlers.
    if (screencast_)
        screencast_.release()->deleteLater();
}

} // namespace blink
