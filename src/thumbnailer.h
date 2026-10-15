#pragma once

#include <QCache>
#include <QDateTime>
#include <QHash>
#include <QImage>
#include <QObject>
#include <QSet>
#include <QSize>
#include <QString>
#include <QThreadPool>

#include <atomic>
#include <memory>

namespace blink {

// Thumbnails for the library, kept in the cache of the freedesktop.org Thumbnail Managing Standard
// (~/.cache/thumbnails): thumbnails Dolphin made show up here, and Blink's in Dolphin. Images are decoded in threads
// of its own, so captures never wait behind it; video frames come from blink-recorder, which has FFmpeg.
class Thumbnailer final : public QObject {
    Q_OBJECT

public:
    struct File {
        QString path;
        QDateTime modified;
        qint64 size = 0;
        bool video = false;
    };

    // Thumbnails fit in `box`, in logical pixels.
    explicit Thumbnailer(QSize box, QObject *parent = nullptr);
    ~Thumbnailer() override; // stops the work in progress, then waits for it

    // The scale of the screen the thumbnails are shown on. Changing it drops the thumbnails made so far.
    void setDevicePixelRatio(qreal ratio);
    // The thumbnail of `file`, at the device pixel ratio. Null while it is being made, and ready() follows; also
    // null when it can't be made, e.g. for a broken file.
    QImage thumbnail(const File &file);

Q_SIGNALS:
    void ready(const QString &path);

private:
    struct Made {
        QImage image;
        QDateTime modified;
        qint64 size = 0;
    };

    void finish(int generation, const File &file, const QImage &image);

    QSize box_;
    qreal ratio_ = 1;
    int generation_ = 0; // bumped when the ratio changes; what older requests make is dropped
    int sequence_ = 0;   // later requests run first: they are for what is on screen now
    QCache<QString, Made> made_;
    QHash<QString, Made> failed_; // the files' versions that could not be made, with a null image
    QSet<QString> pending_;
    QString recorder_;
    std::shared_ptr<std::atomic_bool> stopping_ = std::make_shared<std::atomic_bool>(false);
    QThreadPool pool_;
};

} // namespace blink
