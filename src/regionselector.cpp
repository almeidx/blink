#include "regionselector.h"

#include <LayerShellQt/Window>

#include <QFontInfo>
#include <QFontMetrics>
#include <QGuiApplication>
#include <QKeyEvent>
#include <QPainter>
#include <QPalette>
#include <QRasterWindow>
#include <QScreen>

#include <algorithm>
#include <cmath>
#include <limits>
#include <thread>

namespace blink {

using namespace Qt::StringLiterals;

namespace {

constexpr qreal kDragThreshold = 3;   // logical pixels the pointer moves before a press becomes a drag
constexpr int kMagnifierCells = 15;   // odd, so the middle cell is the pixel under the pointer
constexpr qreal kMagnifierCell = 8;   // logical size of one magnified pixel
constexpr qreal kMagnifierOffset = 24; // logical distance between the pointer and the magnifier
constexpr qreal kLabelPadding = 4;    // logical
constexpr qreal kHintMargin = 24;     // logical

QRect alignedDevice(const QRectF &device)
{
    const QPoint topLeft(static_cast<int>(std::lround(device.left())), static_cast<int>(std::lround(device.top())));
    const QPoint bottomRight(static_cast<int>(std::lround(device.right())), static_cast<int>(std::lround(device.bottom())));
    return QRect(topLeft, bottomRight - QPoint(1, 1));
}

} // namespace

// The backdrop: the frozen screen at 62.5% brightness, two channels per multiply. A 4K screen is 33 MB and the
// overlay waits for this, so rows are split across threads.
static QImage dim(const QImage &image)
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

void prepareScreenImage(QImage &image, QImage &dimmed)
{
    if (image.format() != QImage::Format_ARGB32_Premultiplied && image.format() != QImage::Format_RGB32
        && image.format() != QImage::Format_ARGB32)
        image.convertTo(QImage::Format_ARGB32_Premultiplied);
    dimmed = dim(image);
}

// One overlay per screen: a layer-shell surface above everything, including panels. Paints in the frozen
// image's own pixels, so the image is copied 1:1 into the backing store and outlines are exact.
class SelectionWindow final : public QRasterWindow {
public:
    SelectionWindow(RegionSelector *selector, int index, QScreen *screen);

    void refresh();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void keyPressEvent(QKeyEvent *event) override;
    bool event(QEvent *event) override;

private:
    struct Decorations {
        QRect highlight; // bright, in the image's pixels
        QRect frame;     // highlight plus its border
        QRect label;
        QString labelText;
        QRect magnifier;     // the zoomed pixels and the caption under them
        QRect magnifierBox;  // the zoomed pixels
        QPoint magnified;    // the pixel under the pointer
        QString caption;     // its position and colour
        QRect hint;

        bool operator==(const Decorations &) const = default;
    };

    const RegionSelector::Screen &screen() const { return selector_->screens_[index_]; }
    Decorations compute() const;
    QRect labelRect(const QString &text, QPoint anchor, bool above) const;
    void paintArea(QPainter &painter, const QRect &area) const;
    void paintLabel(QPainter &painter, const QRect &rect, const QString &text) const;
    void paintMagnifier(QPainter &painter) const;
    QRect toDevice(const QRect &logical) const;
    QRect toLogical(const QRect &device) const;

    RegionSelector *selector_;
    int index_;
    qreal scale_;
    int border_;
    QFont font_;
    QColor accent_;
    QString hintText_;
    Decorations current_;
};

SelectionWindow::SelectionWindow(RegionSelector *selector, int index, QScreen *screen)
    : selector_(selector)
    , index_(index)
    , scale_(selector->screens_[index].scale)
    , border_(std::max(1, static_cast<int>(std::lround(1.5 * scale_))))
    , font_(QGuiApplication::font())
{
    font_.setPixelSize(static_cast<int>(std::lround(QFontInfo(font_).pixelSize() * scale_)));
    const bool recording = selector->purpose_ == RegionSelector::Purpose::Recording;
    accent_ = recording ? QColor(0xda, 0x44, 0x53) : QGuiApplication::palette().color(QPalette::Highlight);
    hintText_ = recording ? RegionSelector::tr("Drag to select the area to record  ·  Click to record a window  ·  Esc to cancel")
                          : RegionSelector::tr("Drag to select an area  ·  Click to capture a window  ·  Esc to cancel");

    setScreen(screen);
    setGeometry(screen->geometry());
    setCursor(Qt::CrossCursor);
    auto *layer = LayerShellQt::Window::get(this);
    layer->setScope(u"blink-region"_s);
    layer->setLayer(LayerShellQt::Window::LayerOverlay);
    // NOLINTBEGIN(bugprone-signed-bitwise): the anchors are an unscoped enum, ORed as intended.
    layer->setAnchors({LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom | LayerShellQt::Window::AnchorLeft
                       | LayerShellQt::Window::AnchorRight});
    // NOLINTEND(bugprone-signed-bitwise)
    layer->setExclusiveZone(-1);
    layer->setKeyboardInteractivity(LayerShellQt::Window::KeyboardInteractivityExclusive);
    layer->setScreen(screen);
    current_ = compute();
}

QRect SelectionWindow::toDevice(const QRect &logical) const
{
    const int left = static_cast<int>(std::floor(logical.left() * scale_));
    const int top = static_cast<int>(std::floor(logical.top() * scale_));
    const int right = static_cast<int>(std::ceil((logical.right() + 1) * scale_));
    const int bottom = static_cast<int>(std::ceil((logical.bottom() + 1) * scale_));
    return QRect(left, top, right - left, bottom - top);
}

QRect SelectionWindow::toLogical(const QRect &device) const
{
    const int left = static_cast<int>(std::floor(device.left() / scale_));
    const int top = static_cast<int>(std::floor(device.top() / scale_));
    const int right = static_cast<int>(std::ceil((device.right() + 1) / scale_));
    const int bottom = static_cast<int>(std::ceil((device.bottom() + 1) / scale_));
    return QRect(left, top, right - left, bottom - top);
}

QRect SelectionWindow::labelRect(const QString &text, QPoint anchor, bool above) const
{
    const int padding = static_cast<int>(std::lround(kLabelPadding * scale_));
    const QSize size = QFontMetrics(font_).size(Qt::TextSingleLine, text) + QSize(2 * padding, padding);
    const QRect bounds = screen().image.rect();
    QRect rect(anchor, size);
    if (above)
        rect.moveBottom(anchor.y() - 1);
    if (rect.top() < bounds.top())
        rect.moveTop(anchor.y() + padding);
    rect.moveLeft(std::clamp(rect.left(), bounds.left(), std::max(bounds.left(), bounds.right() - size.width())));
    rect.moveTop(std::clamp(rect.top(), bounds.top(), std::max(bounds.top(), bounds.bottom() - size.height())));
    return rect;
}

SelectionWindow::Decorations SelectionWindow::compute() const
{
    Decorations d;
    const RegionSelector &s = *selector_;
    const QRect bounds = screen().image.rect();

    if (const QRectF region = s.selection(); !region.isEmpty()) {
        d.highlight = s.toDevice(index_, region) & bounds;
        if (!d.highlight.isEmpty())
            d.frame = d.highlight.adjusted(-border_, -border_, border_, border_);
        // The size goes on the screen that has the selection's top left corner.
        if (s.screenAt(region.topLeft() + QPointF(0.01, 0.01)) == index_) {
            const QSize size = s.selectionSize(region);
            d.labelText = QString::number(size.width()) + u" × "_s + QString::number(size.height());
            const QRect corner = s.toDevice(index_, region);
            d.label = labelRect(d.labelText, QPoint(corner.left(), corner.top() - border_), true);
        }
    }

    if (s.magnifier_ && s.pointer_ && s.pointerScreen_ == index_) {
        const QPointF local = (*s.pointer_ - QPointF(screen().geometry.topLeft())) * scale_;
        d.magnified = QPoint(std::clamp(static_cast<int>(std::floor(local.x())), 0, bounds.width() - 1),
                             std::clamp(static_cast<int>(std::floor(local.y())), 0, bounds.height() - 1));
        d.caption = QString::number(d.magnified.x()) + u", "_s + QString::number(d.magnified.y()) + u"   "_s
                  + screen().image.pixelColor(d.magnified).name(QColor::HexRgb).toUpper();

        const int side = kMagnifierCells * static_cast<int>(std::lround(kMagnifierCell * scale_));
        const int padding = static_cast<int>(std::lround(kLabelPadding * scale_));
        const QFontMetrics metrics(font_);
        const QSize caption(std::max(side, metrics.horizontalAdvance(d.caption) + 2 * padding), metrics.height() + padding);
        const QSize size(caption.width(), side + caption.height());
        const int offset = static_cast<int>(std::lround(kMagnifierOffset * scale_));
        QPoint topLeft = d.magnified + QPoint(offset, offset);
        if (topLeft.x() + size.width() > bounds.width())
            topLeft.setX(d.magnified.x() - offset - size.width());
        if (topLeft.y() + size.height() > bounds.height())
            topLeft.setY(d.magnified.y() - offset - size.height());
        d.magnifier = QRect(topLeft, size);
        d.magnifierBox = QRect(topLeft + QPoint((size.width() - side) / 2, 0), QSize(side, side));
    }

    if (!s.pressedOnce_) {
        d.hint = labelRect(hintText_, QPoint(0, static_cast<int>(std::lround(kHintMargin * scale_))), false);
        d.hint.moveLeft((bounds.width() - d.hint.width()) / 2);
    }
    return d;
}

void SelectionWindow::refresh()
{
    const Decorations next = compute();
    if (next == current_)
        return;
    QRegion damage = QRegion(current_.highlight).xored(QRegion(next.highlight));
    if (current_.frame != next.frame)
        damage += QRegion(current_.frame).subtracted(QRegion(current_.highlight)) + QRegion(next.frame).subtracted(QRegion(next.highlight));
    if (current_.label != next.label || current_.labelText != next.labelText)
        damage += QRegion(current_.label) + QRegion(next.label);
    if (current_.magnifier != next.magnifier || current_.magnified != next.magnified || current_.caption != next.caption)
        damage += QRegion(current_.magnifier) + QRegion(next.magnifier);
    if (current_.hint != next.hint)
        damage += QRegion(current_.hint) + QRegion(next.hint);
    current_ = next;

    QRegion logical;
    for (const QRect &rect : damage)
        logical += toLogical(rect);
    update(logical);
}

void SelectionWindow::paintEvent(QPaintEvent *event)
{
    QPainter painter(this);
    // Work in the frozen image's pixels: with the device pixel ratio cancelled out, blits are plain copies.
    painter.setWorldTransform(QTransform::fromScale(1 / scale_, 1 / scale_));
    const QRect bounds = screen().image.rect();
    for (const QRect &rect : event->region()) {
        if (const QRect area = toDevice(rect) & bounds; !area.isEmpty())
            paintArea(painter, area);
    }
}

void SelectionWindow::paintArea(QPainter &painter, const QRect &area) const
{
    const RegionSelector::Screen &s = screen();
    painter.setClipRect(area);
    painter.drawImage(area.topLeft(), s.dimmed, area);
    if (const QRect bright = area & current_.highlight; !bright.isEmpty())
        painter.drawImage(bright.topLeft(), s.image, bright);

    if (!current_.frame.isEmpty() && area.intersects(current_.frame)) {
        const QRect &f = current_.frame;
        const QRect &h = current_.highlight;
        painter.fillRect(QRect(f.left(), f.top(), f.width(), h.top() - f.top()), accent_);
        painter.fillRect(QRect(f.left(), h.bottom() + 1, f.width(), f.bottom() - h.bottom()), accent_);
        painter.fillRect(QRect(f.left(), h.top(), h.left() - f.left(), h.height()), accent_);
        painter.fillRect(QRect(h.right() + 1, h.top(), f.right() - h.right(), h.height()), accent_);
    }
    if (area.intersects(current_.label))
        paintLabel(painter, current_.label, current_.labelText);
    if (area.intersects(current_.hint))
        paintLabel(painter, current_.hint, hintText_);
    if (area.intersects(current_.magnifier))
        paintMagnifier(painter);
}

void SelectionWindow::paintLabel(QPainter &painter, const QRect &rect, const QString &text) const
{
    painter.fillRect(rect, QColor(0, 0, 0, 190));
    painter.setPen(Qt::white);
    painter.setFont(font_);
    painter.drawText(rect, Qt::AlignCenter, text);
}

void SelectionWindow::paintMagnifier(QPainter &painter) const
{
    const QImage &image = screen().image;
    const int cell = static_cast<int>(std::lround(kMagnifierCell * scale_));
    const QRect &box = current_.magnifierBox;
    const int half = kMagnifierCells / 2;

    painter.fillRect(box, Qt::black);
    const QRect source(current_.magnified - QPoint(half, half), QSize(kMagnifierCells, kMagnifierCells));
    if (const QRect visible = source & image.rect(); !visible.isEmpty()) {
        const QRect target(box.topLeft() + (visible.topLeft() - source.topLeft()) * cell, visible.size() * cell);
        painter.drawImage(target, image, visible); // nearest neighbour: no SmoothPixmapTransform
    }

    painter.setPen(QColor(0, 0, 0, 50));
    for (int i = 1; i < kMagnifierCells; ++i) {
        painter.drawLine(box.left() + i * cell, box.top(), box.left() + i * cell, box.bottom());
        painter.drawLine(box.left(), box.top() + i * cell, box.right(), box.top() + i * cell);
    }
    const QRect middle(box.topLeft() + QPoint(half * cell, half * cell), QSize(cell, cell));
    painter.setPen(Qt::black);
    painter.drawRect(middle.adjusted(-1, -1, 0, 0));
    painter.setPen(Qt::white);
    painter.drawRect(middle.adjusted(0, 0, -1, -1));
    painter.drawRect(box.adjusted(0, 0, -1, -1));

    const QRect caption(current_.magnifier.left(), box.bottom() + 1, current_.magnifier.width(),
                        current_.magnifier.bottom() - box.bottom());
    paintLabel(painter, caption, current_.caption);
}

void SelectionWindow::mouseMoveEvent(QMouseEvent *event)
{
    selector_->pointerMoved(index_, event->position());
}

void SelectionWindow::mousePressEvent(QMouseEvent *event)
{
    selector_->pressed(index_, event->position(), event->button());
}

void SelectionWindow::mouseReleaseEvent(QMouseEvent *event)
{
    selector_->released(index_, event->position(), event->button());
}

void SelectionWindow::keyPressEvent(QKeyEvent *event)
{
    selector_->keyPressed(event->key());
}

bool SelectionWindow::event(QEvent *event)
{
    if (event->type() == QEvent::Enter)
        selector_->pointerMoved(index_, static_cast<QEnterEvent *>(event)->position());
    else if (event->type() == QEvent::Leave)
        selector_->pointerLeft(index_);
    return QRasterWindow::event(event);
}

RegionSelector::RegionSelector(std::vector<ScreenImage> screens, Purpose purpose, bool magnifier, QObject *parent)
    : QObject(parent), purpose_(purpose), magnifier_(magnifier)
{
    screens_.reserve(screens.size());
    for (ScreenImage &shot : screens) {
        Screen screen;
        screen.geometry = shot.screen->geometry();
        screen.scale = shot.image.devicePixelRatio();
        screen.image = std::move(shot.image);
        screen.image.setDevicePixelRatio(1);
        screen.dimmed = std::move(shot.dimmed);
        screens_.push_back(std::move(screen));
    }
    for (size_t i = 0; i < screens_.size(); ++i)
        screens_[i].window = std::make_unique<SelectionWindow>(this, static_cast<int>(i), screens[i].screen);

    // A frozen screen that went away cannot be selected from any more.
    connect(qGuiApp, &QGuiApplication::screenRemoved, this, &RegionSelector::cancel);
}

RegionSelector::~RegionSelector() = default;

void RegionSelector::show()
{
    for (Screen &screen : screens_)
        screen.window->show();
}

void RegionSelector::setWindows(QList<WindowList::Window> windows)
{
    windows_ = std::move(windows);
    refresh();
}

int RegionSelector::screenAt(const QPointF &global) const
{
    int nearest = 0;
    qreal nearestDistance = std::numeric_limits<qreal>::max();
    for (size_t i = 0; i < screens_.size(); ++i) {
        const QRectF g = screens_[i].geometry;
        if (g.contains(global))
            return static_cast<int>(i);
        const qreal dx = std::max({g.left() - global.x(), 0.0, global.x() - g.right()});
        const qreal dy = std::max({g.top() - global.y(), 0.0, global.y() - g.bottom()});
        if (const qreal distance = dx * dx + dy * dy; distance < nearestDistance) {
            nearestDistance = distance;
            nearest = static_cast<int>(i);
        }
    }
    return nearest;
}

RegionSelector::Pixel RegionSelector::pixelAt(const QPointF &global) const
{
    const int index = screenAt(global);
    const Screen &screen = screens_[index];
    const QPointF local = (global - QPointF(screen.geometry.topLeft())) * screen.scale;
    return {index, QPoint(std::clamp(static_cast<int>(std::floor(local.x())), 0, screen.image.width() - 1),
                          std::clamp(static_cast<int>(std::floor(local.y())), 0, screen.image.height() - 1))};
}

QRectF RegionSelector::pixelRect(const Pixel &pixel) const
{
    const Screen &screen = screens_[pixel.screen];
    return QRectF(QPointF(screen.geometry.topLeft()) + QPointF(pixel.position) / screen.scale, QSizeF(1, 1) / screen.scale);
}

QRectF RegionSelector::clipToScreens(const QRectF &region) const
{
    QRectF all;
    for (const Screen &screen : screens_)
        all |= QRectF(screen.geometry);
    return region & all;
}

QRect RegionSelector::toDevice(int index, const QRectF &region) const
{
    const Screen &screen = screens_[index];
    return alignedDevice(QRectF((region.topLeft() - QPointF(screen.geometry.topLeft())) * screen.scale, region.size() * screen.scale));
}

QRectF RegionSelector::selection() const
{
    if (!pointer_)
        return {};
    if (anchor_ && dragging_)
        return pixelRect(*anchor_) | pixelRect(pixelAt(*pointer_));
    if (const WindowList::Window *window = selectedWindow())
        return clipToScreens(window->frame);
    return screens_[screenAt(*pointer_)].geometry;
}

const WindowList::Window *RegionSelector::selectedWindow() const
{
    if (!pointer_ || (anchor_ && dragging_))
        return nullptr;
    for (const WindowList::Window &window : windows_) {
        if (window.frame.contains(*pointer_))
            return &window;
    }
    return nullptr;
}

RegionSelector::Coverage RegionSelector::coverage(const QRectF &region) const
{
    Coverage coverage;
    for (size_t i = 0; i < screens_.size(); ++i) {
        if (!(region & QRectF(screens_[i].geometry)).isEmpty()) {
            coverage.screens.push_back(static_cast<int>(i));
            coverage.scale = std::max(coverage.scale, screens_[i].scale);
        }
    }
    return coverage;
}

qreal RegionSelector::scale(const QRectF &region) const
{
    return coverage(region).scale;
}

QSize RegionSelector::selectionSize(const QRectF &region) const
{
    const Coverage covered = coverage(region);
    if (covered.screens.size() == 1)
        return toDevice(covered.screens.front(), region).size();
    return alignedDevice(QRectF(QPointF(), region.size() * covered.scale)).size();
}

QImage RegionSelector::grab(const QRectF &region) const
{
    const auto [covered, scale] = coverage(region);
    if (covered.empty())
        return {};
    if (covered.size() == 1) {
        const Screen &screen = screens_[covered.front()];
        const QRect rect = toDevice(covered.front(), region) & screen.image.rect();
        // A view of the frozen pixels rather than a copy: a desktop click grabs the whole 33 MB of a 4K screen, and
        // the PNG encoder reads it on the pool after this selector is deleted, so a handle to the frozen image keeps
        // the pixels alive as long as the view. Rows start at x * 4 because prepareScreenImage() made the image
        // 32-bit. The non-const constructor: setDevicePixelRatio() detaches, and Qt copies read-only data on detach.
        // Nothing writes through the result (encodePng and scaled() take it const).
        auto *holder = new QImage(screen.image);
        auto *bits = const_cast<uchar *>(screen.image.constScanLine(rect.y()) + static_cast<qsizetype>(rect.x()) * 4);
        QImage view(
            bits, rect.width(), rect.height(), screen.image.bytesPerLine(), screen.image.format(),
            [](void *info) { delete static_cast<QImage *>(info); }, holder);
        view.setDevicePixelRatio(screen.scale);
        return view;
    }

    // Spanning screens: compose at the highest scale; areas no screen covers stay transparent.
    QImage image(alignedDevice(QRectF(QPointF(), region.size() * scale)).size(), QImage::Format_ARGB32_Premultiplied);
    image.fill(Qt::transparent);
    QPainter painter(&image);
    painter.setRenderHint(QPainter::SmoothPixmapTransform);
    for (const int index : covered) {
        const Screen &screen = screens_[index];
        const QRectF part = region & QRectF(screen.geometry);
        const QRect source = toDevice(index, part) & screen.image.rect();
        const QRect target = alignedDevice(QRectF((part.topLeft() - region.topLeft()) * scale, part.size() * scale));
        painter.drawImage(target, screen.image, source);
    }
    painter.end();
    image.setDevicePixelRatio(scale);
    return image;
}

void RegionSelector::refresh()
{
    for (Screen &screen : screens_)
        screen.window->refresh();
}

QPointF RegionSelector::pointerMoved(int screen, const QPointF &local)
{
    const QPointF global = QPointF(screens_[screen].geometry.topLeft()) + local;
    pointer_ = global;
    // While a button is down, Wayland keeps sending motion to the window it went down on, in its coordinates.
    pointerScreen_ = screenAt(global);
    if (anchor_ && !dragging_) {
        const QPointF moved = global - pressPosition_;
        dragging_ = std::abs(moved.x()) > kDragThreshold || std::abs(moved.y()) > kDragThreshold;
    }
    refresh();
    return global;
}

void RegionSelector::pressed(int screen, const QPointF &local, Qt::MouseButton button)
{
    const QPointF global = pointerMoved(screen, local);
    if (button == Qt::RightButton) {
        if (!anchor_) {
            cancel();
            return;
        }
        anchor_.reset();
        dragging_ = false;
        refresh();
        return;
    }
    if (button != Qt::LeftButton)
        return;
    anchor_ = pixelAt(global);
    pressPosition_ = global;
    dragging_ = false;
    pressedOnce_ = true;
    refresh();
}

void RegionSelector::released(int screen, const QPointF &local, Qt::MouseButton button)
{
    if (button != Qt::LeftButton || !anchor_)
        return;
    pointerMoved(screen, local);
    const QRectF region = selection();
    const WindowList::Window *window = selectedWindow();
    anchor_.reset();
    dragging_ = false;
    if (!region.isEmpty())
        finish(region, window);
}

void RegionSelector::keyPressed(int key)
{
    if (key == Qt::Key_Escape) {
        cancel();
    } else if ((key == Qt::Key_Return || key == Qt::Key_Enter) && !anchor_) {
        if (const QRectF region = selection(); !region.isEmpty())
            finish(region, selectedWindow());
    }
}

void RegionSelector::pointerLeft(int screen)
{
    // Moving onto another screen's overlay: its enter event takes over the pointer.
    if (pointerScreen_ == screen && !anchor_) {
        pointerScreen_ = -1;
        refresh();
    }
}

bool RegionSelector::close()
{
    if (done_)
        return false;
    done_ = true;
    for (Screen &screen : screens_)
        screen.window->hide();
    return true;
}

void RegionSelector::cancel()
{
    if (close())
        Q_EMIT cancelled();
}

void RegionSelector::finish(const QRectF &region, const WindowList::Window *window)
{
    const QString app = window ? window->app : QString();
    if (close())
        Q_EMIT selected(region, app);
}

} // namespace blink
