#include "screencast.h"

#include "qwayland-zkde-screencast-unstable-v1.h"

#include <QCoreApplication>
#include <QtWaylandClient/QWaylandClientExtensionTemplate>

namespace blink {

using namespace Qt::StringLiterals;

namespace {

// stream_region; version 6 adds the PipeWire object serial.
constexpr int kMinimumVersion = 3;
constexpr int kMaximumVersion = 6;

} // namespace

class ScreencastGlobal final : public QWaylandClientExtensionTemplate<ScreencastGlobal, &QtWayland::zkde_screencast_unstable_v1::destroy>,
                               public QtWayland::zkde_screencast_unstable_v1 {
public:
    ScreencastGlobal() : QWaylandClientExtensionTemplate(kMaximumVersion) { initialize(); }
};

class ScreencastStream final : public QtWayland::zkde_screencast_stream_unstable_v1 {
public:
    ScreencastStream(::zkde_screencast_stream_unstable_v1 *object, Screencast *owner)
        : zkde_screencast_stream_unstable_v1(object), owner_(owner)
    {
    }
    ~ScreencastStream() override { close(); }
    ScreencastStream(const ScreencastStream &) = delete;
    ScreencastStream &operator=(const ScreencastStream &) = delete;

protected:
    void zkde_screencast_stream_unstable_v1_serial(uint32_t high, uint32_t low) override
    {
        serial_ = (static_cast<quint64>(high) << 32) | low;
    }

    // Comes after serial when there is one.
    void zkde_screencast_stream_unstable_v1_created(uint32_t node) override
    {
        if (serial_)
            Q_EMIT owner_->ready(serial_, true);
        else
            Q_EMIT owner_->ready(node, false);
    }

    void zkde_screencast_stream_unstable_v1_failed(const QString &error) override { Q_EMIT owner_->failed(error); }
    void zkde_screencast_stream_unstable_v1_closed() override { Q_EMIT owner_->closed(); }

private:
    Screencast *owner_;
    quint64 serial_ = 0;
};

Screencast::Screencast(QObject *parent) : QObject(parent), global_(std::make_unique<ScreencastGlobal>()) {}

Screencast::~Screencast() = default;

void Screencast::start(const QRect &region, qreal scale, bool cursor)
{
    if (!global_->isActive()) {
        Q_EMIT failed(
            tr("KWin only lets apps record the screen when a desktop file has Exec=%1.").arg(QCoreApplication::applicationFilePath()));
        return;
    }
    if (global_->QtWayland::zkde_screencast_unstable_v1::version() < kMinimumVersion) {
        Q_EMIT failed(tr("This KWin is too old to record part of the screen."));
        return;
    }
    const uint32_t pointer = cursor ? QtWayland::zkde_screencast_unstable_v1::pointer_embedded
                                    : QtWayland::zkde_screencast_unstable_v1::pointer_hidden;
    stream_ = std::make_unique<ScreencastStream>(global_->stream_region(region.x(), region.y(), static_cast<uint32_t>(region.width()),
                                                                        static_cast<uint32_t>(region.height()), wl_fixed_from_double(scale),
                                                                        pointer),
                                                 this);
}

} // namespace blink
