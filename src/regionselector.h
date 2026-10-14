#pragma once

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

// A screen frozen by KWin: its image at native resolution (the device pixel ratio is the screen's scale).
struct ScreenImage {
    QScreen *screen = nullptr;
    QImage image;
};

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
    void setWindows(QList<QRectF> windows);

    // The pixels of `region` (global logical coordinates), composed at the highest scale when it spans screens.
    QImage grab(const QRectF &region) const;
    // The highest scale among the screens `region` covers.
    qreal scale(const QRectF &region) const;

Q_SIGNALS:
    // In global logical coordinates, on device pixel boundaries.
    void selected(const QRectF &region);
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
    QSize selectionSize(const QRectF &region) const;

    QPointF pointerMoved(int screen, const QPointF &local); // returns the global position
    void pressed(int screen, const QPointF &local, Qt::MouseButton button);
    void released(int screen, const QPointF &local, Qt::MouseButton button);
    void keyPressed(int key);
    void pointerLeft(int screen);
    bool close(); // hides the overlay; false when it already was
    void cancel();
    void finish(const QRectF &region);
    void refresh();

    std::vector<Screen> screens_;
    QList<QRectF> windows_;
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
