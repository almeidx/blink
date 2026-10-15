#include "thumbnailer.h"

#include "recording.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QImageReader>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>
#include <QThread>
#include <QUrl>

#include <algorithm>
#include <array>

namespace blink {
namespace {

using namespace Qt::StringLiterals;

// Memory for thumbnails already made; those dropped come back from the disk cache.
constexpr qsizetype kMemoryBytes = 64 << 20;
constexpr int kVideoTimeoutMs = 10'000;
// The value of the standard's Software key. A thumbnail Blink made is as large as its directory allows.
constexpr QLatin1StringView kSoftware = "Blink"_L1;

struct Tier {
    QLatin1StringView directory;
    int size; // the largest thumbnail it holds, in pixels
};
constexpr std::array kTiers{Tier{"normal"_L1, 128}, Tier{"large"_L1, 256}, Tier{"x-large"_L1, 512}, Tier{"xx-large"_L1, 1024}};

// The directory KIO picks for thumbnails of `box` at `ratio` (KIO::ThumbnailCache::tierDir), so both read and write
// the same files: the box's longer side rounded up to a tier, times the ratio, rounded up to a tier again.
Tier tierFor(QSize box, qreal ratio)
{
    const int longer = std::max(box.width(), box.height());
    const auto rounded = std::ranges::find_if(kTiers, [&](const Tier &tier) { return tier.size >= longer; });
    const int wanted = static_cast<int>(ratio * (rounded == kTiers.end() ? kTiers.back().size : rounded->size));
    const auto tier = std::ranges::find_if(kTiers, [&](const Tier &tier) { return tier.size >= wanted; });
    return tier == kTiers.end() ? kTiers.back() : *tier;
}

QString cacheRoot()
{
    return QDir::cleanPath(QStandardPaths::writableLocation(QStandardPaths::GenericCacheLocation) + u"/thumbnails"_s);
}

// The cached thumbnail at `path` when it is of this version of the file. Thumbnails made for a smaller view than
// this one, as Dolphin makes them at a low zoom, are made again; Blink's own are as large as they get.
QImage readCached(const QString &path, const QString &uri, qint64 modified, qint64 size, int needed)
{
    // QImage::text() rather than QImageReader::text(), which splits each entry at its first colon: these keys have two.
    QImage cached;
    if (!cached.load(path, "png"))
        return {};
    bool known = false;
    if (cached.text(u"Thumb::URI"_s) != uri || cached.text(u"Thumb::MTime"_s).toLongLong(&known) != modified || !known)
        return {};
    if (const QString cachedSize = cached.text(u"Thumb::Size"_s); !cachedSize.isEmpty() && cachedSize.toLongLong() != size)
        return {};
    if (std::max(cached.width(), cached.height()) < needed && cached.text(u"Software"_s) != kSoftware)
        return {};
    return cached;
}

// An image scaled to fit in size×size pixels, never enlarged. `original` gets the image's own size.
QImage imageThumbnail(const QString &path, int size, QSize *original)
{
    QImageReader reader(path);
    reader.setAutoTransform(true);
    QSize full = reader.size();
    // JPEG decodes straight at the smaller size; other formats are decoded whole and then scaled.
    if (full.isValid() && (full.width() > size || full.height() > size))
        reader.setScaledSize(full.scaled(size, size, Qt::KeepAspectRatio));
    QImage image = reader.read();
    if (image.isNull())
        return {};
    if (full.isValid() && reader.transformation().testFlag(QImageIOHandler::TransformationRotate90))
        full.transpose();
    *original = full.isValid() ? full : image.size();
    if (image.width() > size || image.height() > size)
        image = image.scaled(size, size, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    return image;
}

// The first frame of a video, from blink-recorder --thumbnail.
QImage videoThumbnail(const QString &path, int size, const std::atomic_bool &stopping)
{
    QProcess process;
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(recorderPath(), {u"--thumbnail"_s, QString::number(size), path}, QIODevice::ReadOnly);
    QElapsedTimer timer;
    timer.start();
    while (!process.waitForFinished(100)) {
        if (process.state() == QProcess::NotRunning) // it did not start
            return {};
        if (stopping || timer.hasExpired(kVideoTimeoutMs)) {
            process.kill();
            process.waitForFinished();
            return {};
        }
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    return QImage::fromData(process.readAllStandardOutput(), "PPM");
}

// As the standard has it: directories 0700, files 0600, written under another name and renamed into place.
void save(const QString &directory, const QString &path, QImage thumbnail, const QString &uri, qint64 modified, qint64 size, QSize original)
{
    constexpr QFile::Permissions kOwnerOnly = QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner;
    if (const QString root = cacheRoot(); !QFileInfo::exists(root)) {
        QDir().mkpath(QFileInfo(root).path());
        QDir().mkdir(root, kOwnerOnly);
    }
    if (!QFileInfo::exists(directory))
        QDir().mkdir(directory, kOwnerOnly);

    thumbnail = thumbnail.convertToFormat(QImage::Format_ARGB32); // 8-bit RGBA, which the standard asks for
    thumbnail.setText(u"Thumb::URI"_s, uri);
    thumbnail.setText(u"Thumb::MTime"_s, QString::number(modified));
    thumbnail.setText(u"Thumb::Size"_s, QString::number(size));
    if (original.isValid()) {
        thumbnail.setText(u"Thumb::Image::Width"_s, QString::number(original.width()));
        thumbnail.setText(u"Thumb::Image::Height"_s, QString::number(original.height()));
    }
    thumbnail.setText(u"Software"_s, kSoftware);
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return;
    file.setPermissions(QFile::ReadOwner | QFile::WriteOwner);
    if (thumbnail.save(&file, "png"))
        file.commit();
    else
        file.cancelWriting();
}

// The thumbnail of `file` from the disk cache, or made and stored there; scaled to fit `display` device pixels.
QImage make(const Thumbnailer::File &file, Tier tier, QSize display, const std::atomic_bool &stopping)
{
    // The standard: no thumbnail, cached or new, for a file the user cannot read.
    const QFileInfo info(file.path);
    if (stopping || !info.isReadable())
        return {};
    // KIO's name for it, so the thumbnails are shared: the MD5 of the fully encoded file URL.
    const QByteArray url = QUrl::fromLocalFile(file.path).toEncoded(QUrl::FullyEncoded);
    const QString uri = QString::fromLatin1(url);
    const QString directory = cacheRoot() + u'/' + tier.directory;
    const QString path = directory + u'/' + QString::fromLatin1(QCryptographicHash::hash(url, QCryptographicHash::Md5).toHex()) + ".png"_L1;
    const qint64 modified = info.lastModified().toSecsSinceEpoch();

    QImage thumbnail = readCached(path, uri, modified, info.size(), std::max(display.width(), display.height()));
    if (thumbnail.isNull()) {
        QSize original;
        thumbnail = file.video ? videoThumbnail(file.path, tier.size, stopping) : imageThumbnail(file.path, tier.size, &original);
        if (thumbnail.isNull())
            return {};
        save(directory, path, thumbnail, uri, modified, info.size(), original);
    }
    if (thumbnail.width() > display.width() || thumbnail.height() > display.height())
        thumbnail = thumbnail.scaled(display, Qt::KeepAspectRatio, Qt::SmoothTransformation);
    // The formats QPainter draws fastest.
    return thumbnail.convertToFormat(thumbnail.hasAlphaChannel() ? QImage::Format_ARGB32_Premultiplied : QImage::Format_RGB32);
}

} // namespace

Thumbnailer::Thumbnailer(QSize box, QObject *parent) : QObject(parent), box_(box)
{
    made_.setMaxCost(kMemoryBytes);
    // Half the cores: thumbnails fill in quickly and leave room for everything else.
    pool_.setMaxThreadCount(std::max(1, QThread::idealThreadCount() / 2));
}

Thumbnailer::~Thumbnailer()
{
    *stopping_ = true;
    pool_.clear();
    pool_.waitForDone();
}

void Thumbnailer::setDevicePixelRatio(qreal ratio)
{
    if (qFuzzyCompare(ratio, ratio_))
        return;
    ratio_ = ratio;
    ++generation_;
    pool_.clear();
    made_.clear();
    failed_.clear();
    pending_.clear();
}

QImage Thumbnailer::thumbnail(const File &file)
{
    if (const Made *made = made_.object(file.path); made && made->modified == file.modified && made->size == file.size)
        return made->image;
    if (const auto failed = failed_.constFind(file.path);
        failed != failed_.cend() && failed->modified == file.modified && failed->size == file.size)
        return {};
    if (pending_.contains(file.path))
        return {};
    pending_.insert(file.path);
    const QSize display = (QSizeF(box_) * ratio_).toSize();
    pool_.start(
        [this, generation = generation_, file, tier = tierFor(box_, ratio_), display, ratio = ratio_, stopping = stopping_] {
            QImage image = make(file, tier, display, *stopping);
            image.setDevicePixelRatio(ratio);
            // Delivered only while this exists: the destructor waits for these tasks, and posted calls die with it.
            QMetaObject::invokeMethod(this, [this, generation, file, image] { finish(generation, file, image); }, Qt::QueuedConnection);
        },
        ++sequence_);
    return {};
}

void Thumbnailer::finish(int generation, const File &file, const QImage &image)
{
    if (generation != generation_)
        return;
    pending_.remove(file.path);
    if (image.isNull())
        failed_.insert(file.path, Made{{}, file.modified, file.size});
    else
        made_.insert(file.path, new Made{image, file.modified, file.size}, image.sizeInBytes());
    Q_EMIT ready(file.path);
}

} // namespace blink
