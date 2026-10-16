#pragma once

#include "windowlist.h"

#include <QImage>
#include <QList>
#include <QObject>
#include <QPointF>
#include <QRectF>

#include <memory>
#include <optional>
#include <vector>

class QScreen;

namespace blink {

class SelectionWindow;

// A screen frozen by KWin, ready for the overlay: `image` is 32-bit (RGB32, ARGB32 or ARGB32 premultiplied) at native
// resolution with the screen's scale as its device pixel ratio; `dimmed` is its backdrop. Both come from
// prepareScreenImage().
struct ScreenImage {
    QScreen *screen = nullptr;
    QImage image;
    QImage dimmed;
};

// The pixel work a frozen screen needs before the overlay can show it: KWin sends screens as RGBX8888, which the
// overlay cannot blit 1:1, and the backdrop is a full pass over the pixels. Pure and thread-safe, so App runs it on
// the thread pool while the other screens are still arriving. Keeps the device pixel ratio of `image`.
void prepareScreenImage(QImage &image, QImage &dimmed);

// ShareX-style region picker. Covers every screen with its frozen image, dimmed: drag to select an area, click
// to take the window (or desktop) under the pointer, Esc or right click to cancel. A magnifier shows the pixels
// around the pointer. Selections are exact to the device pixel.
class RegionSelector final : public QObject {
    Q_OBJECT

public:
    enum class Purpose { Screenshot, Recording };

    RegionSelector(std::vector<ScreenImage> screens, Purpose purpose, bool magnifier, QObject *parent = nullptr);
    ~RegionSelector() override;

    void show();
    // Windows to snap to, topmost first, in global logical coordinates. May arrive after show().
    void setWindows(QList<WindowList::Window> windows);

    // The pixels of `region` (global logical coordinates): a read-only view into the frozen screen when one screen
    // covers it, composed at the highest scale when it spans screens.
    QImage grab(const QRectF &region) const;
    // The highest scale among the screens `region` covers.
    qreal scale(const QRectF &region) const;

Q_SIGNALS:
    // In global logical coordinates, on device pixel boundaries. `app` is the app of a window taken whole by a click
    // (or Enter); empty for dragged areas and the desktop.
    void selected(const QRectF &region, const QString &app);
    void cancelled();

private:
    friend class SelectionWindow;

    struct Screen {
        QRect geometry; // logical
        qreal scale = 1;
        QImage image;   // device pixels, device pixel ratio 1
        QImage dimmed;  // the backdrop
        std::unique_ptr<SelectionWindow> window;
    };
    struct Pixel {
        int screen = -1;
        QPoint position; // device pixels within the screen
    };
    struct Coverage {
        std::vector<int> screens; // that a region overlaps
        qreal scale = 0;          // the highest of theirs
    };

    int screenAt(const QPointF &global) const; // nearest screen when between or outside them
    Pixel pixelAt(const QPointF &global) const;
    QRectF pixelRect(const Pixel &pixel) const; // logical
    QRectF clipToScreens(const QRectF &region) const;
    Coverage coverage(const QRectF &region) const;
    QRect toDevice(int screen, const QRectF &region) const;
    QRectF selection() const; // the area that release (or click) would pick
    const WindowList::Window *selectedWindow() const; // the window selection() takes whole, if any
    QSize selectionSize(const QRectF &region) const;

    QPointF pointerMoved(int screen, const QPointF &local); // returns the global position
    void pressed(int screen, const QPointF &local, Qt::MouseButton button);
    void released(int screen, const QPointF &local, Qt::MouseButton button);
    void keyPressed(int key);
    void pointerLeft(int screen);
    bool close(); // hides the overlay; false when it already was
    void cancel();
    void finish(const QRectF &region, const WindowList::Window *window);
    void refresh();

    std::vector<Screen> screens_;
    QList<WindowList::Window> windows_;
    Purpose purpose_;
    bool magnifier_;
    std::optional<QPointF> pointer_; // global logical, while over one of our windows
    int pointerScreen_ = -1;
    std::optional<Pixel> anchor_;    // where the button went down
    QPointF pressPosition_;
    bool dragging_ = false;
    bool pressedOnce_ = false;       // hides the hint
    bool done_ = false;
};

} // namespace blink
