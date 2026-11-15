#include "frozenscreens.h"

#include "kwinscreenshot.h"
#include "logging.h"
#include "threadpool.h"

#include <QElapsedTimer>
#include <QGuiApplication>
#include <QPointer>
#include <QScreen>

#include <algorithm>
#include <memory>
#include <thread>
#include <utility>

namespace blink {
namespace {

// The backdrop: the frozen screen at 62.5% brightness, two channels per multiply. A 4K screen is 33 MB and the
// overlay waits for this, so rows are split across threads.
QImage dim(const QImage &image)
{
    QImage dimmed(image.size(), QImage::Format_RGB32);
    const uchar *src = image.constBits();
    uchar *dst = dimmed.bits();
    const qsizetype srcStride = image.bytesPerLine();
    const qsizetype dstStride = dimmed.bytesPerLine();
    const int width = image.width();
    auto rows = [=](int first, int last) {
        for (int y = first; y < last; ++y) {
            const auto *in = reinterpret_cast<const uint32_t *>(src + y * srcStride);
            auto *out = reinterpret_cast<uint32_t *>(dst + y * dstStride);
            for (int x = 0; x < width; ++x) {
                const uint32_t p = in[x];
                const uint32_t rb = ((p & 0x00ff00ffu) * 5u >> 3u) & 0x00ff00ffu;
                const uint32_t g = ((p >> 8u) & 0xffu) * 5u >> 3u;
                out[x] = 0xff000000u | rb | (g << 8u);
            }
        }
    };

    const int height = image.height();
    const int chunks = std::clamp(height / 64, 1, static_cast<int>(std::max(1u, std::thread::hardware_concurrency())));
    std::vector<std::jthread> workers;
    workers.reserve(chunks - 1);
    for (int i = 1; i < chunks; ++i)
        workers.emplace_back(rows, height * i / chunks, height * (i + 1) / chunks);
    rows(0, height / chunks);
    return dimmed;
}

// Keeps the device pixel ratio of `image`.
std::pair<QImage, QImage> prepare(QImage image)
{
    if (image.format() != QImage::Format_ARGB32_Premultiplied && image.format() != QImage::Format_RGB32
        && image.format() != QImage::Format_ARGB32)
        image.convertTo(QImage::Format_ARGB32_Premultiplied);
    QImage dimmed = dim(image);
    return {std::move(image), std::move(dimmed)};
}

struct Pending {
    std::vector<ScreenImage> screens;
    qsizetype captured = 0;
    qsizetype remaining = 0;
    QString error;
    FrozenScreens done;
    QElapsedTimer timer;
};

void finish(Pending &pending)
{
    qCDebug(lcBlink) << "screens prepared after" << pending.timer.elapsed() << "ms";
    if (!pending.error.isEmpty()) {
        pending.done({}, pending.error);
        return;
    }
    const QList<QScreen *> current = QGuiApplication::screens();
    if (std::ranges::any_of(pending.screens, [&current](const ScreenImage &shot) { return !current.contains(shot.screen); })) {
        pending.done({}, {});
        return;
    }
    pending.done(std::move(pending.screens), {});
}

} // namespace

void freezeScreens(KWinScreenshot &screenshot, QObject *context, FrozenScreens done)
{
    const QList<QScreen *> screens = QGuiApplication::screens();
    auto pending = std::make_shared<Pending>();
    pending->screens.resize(screens.size());
    pending->remaining = screens.size();
    pending->done = std::move(done);
    pending->timer.start();
    for (qsizetype i = 0; i < screens.size(); ++i) {
        pending->screens[i].screen = screens[i];
        screenshot.captureScreen(screens[i]->name(), {},
                                 [guard = QPointer(context), pending, i](QImage image, const QString &, const QString &error) {
                                     if (++pending->captured == std::ssize(pending->screens))
                                         qCDebug(lcBlink) << "screens captured after" << pending->timer.elapsed() << "ms";
                                     if (!guard)
                                         return;
                                     if (!error.isEmpty()) {
                                         pending->error = error;
                                         if (--pending->remaining == 0)
                                             finish(*pending);
                                         return;
                                     }
                                     // The only reference: the conversion from KWin's RGBX8888 runs in place instead of on a
                                     // new 33 MB buffer.
                                     runInPool([image = std::move(image)]() mutable {
                                         return prepare(std::move(image));
                                     }).then(guard.data(), [pending, i](std::pair<QImage, QImage> prepared) {
                                         pending->screens[i].image = std::move(prepared.first);
                                         pending->screens[i].dimmed = std::move(prepared.second);
                                         if (--pending->remaining == 0)
                                             finish(*pending);
                                     });
                                 });
    }
}

} // namespace blink
