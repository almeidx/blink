#include "librarymodel.h"

#include <QDirIterator>
#include <QFileInfo>
#include <QLocale>
#include <QMimeData>
#include <QSet>
#include <QUrl>

#include <algorithm>
#include <array>
#include <chrono>

namespace blink {
namespace {

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

// What Blink saves, and the common types beside it.
constexpr std::array kImageSuffixes{"png"_L1, "jpg"_L1, "jpeg"_L1, "webp"_L1, "gif"_L1, "bmp"_L1, "avif"_L1, "jxl"_L1};
// The containers blink-recorder --thumbnail opens.
constexpr std::array kVideoSuffixes{"mp4"_L1, "mkv"_L1, "webm"_L1, "mov"_L1};
// More changes than this at once reset the model: cheaper than as many row insertions and removals.
constexpr qsizetype kMaxRowChanges = 64;

struct Found {
    QList<Thumbnailer::File> files; // newest first
    QStringList directories;        // to watch
};

Found scanFolders(const QStringList &folders, const std::atomic_bool &stopping)
{
    Found found;
    QSet<QString> seen; // a file in folders inside each other is listed once
    for (const QString &folder : folders) {
        if (!QFileInfo(folder).isDir())
            continue;
        found.directories.append(folder);
        QDirIterator it(folder, QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
        while (it.hasNext() && !stopping) {
            const QFileInfo info = it.nextFileInfo();
            if (info.isDir()) {
                found.directories.append(info.filePath());
                continue;
            }
            const QString suffix = info.suffix().toLower();
            const bool video = std::ranges::contains(kVideoSuffixes, suffix);
            if ((!video && !std::ranges::contains(kImageSuffixes, suffix)) || seen.contains(info.filePath()))
                continue;
            seen.insert(info.filePath());
            found.files.append({info.filePath(), info.lastModified(), info.size(), video});
        }
    }
    std::ranges::sort(found.files, [](const Thumbnailer::File &a, const Thumbnailer::File &b) {
        return a.modified != b.modified ? a.modified > b.modified : a.path < b.path;
    });
    found.directories.removeDuplicates();
    return found;
}

QString fileName(const QString &path)
{
    return path.sliced(path.lastIndexOf(u'/') + 1);
}

} // namespace

LibraryModel::LibraryModel(Thumbnailer *thumbnailer, QObject *parent) : QAbstractListModel(parent), thumbnailer_(thumbnailer)
{
    scanner_.setMaxThreadCount(1);
    rescan_.setSingleShot(true);
    rescan_.setInterval(200ms);
    connect(&rescan_, &QTimer::timeout, this, &LibraryModel::scan);
    connect(&watcher_, &QFileSystemWatcher::directoryChanged, this, &LibraryModel::rescan);
    connect(thumbnailer_, &Thumbnailer::ready, this, [this](const QString &path) {
        if (const int found = row(path); found >= 0)
            Q_EMIT dataChanged(index(found), index(found), {ThumbnailRole});
    });
}

LibraryModel::~LibraryModel()
{
    stopping_ = true;
    scanner_.clear();
    scanner_.waitForDone();
}

void LibraryModel::setFolders(const QStringList &folders)
{
    if (folders == folders_)
        return;
    folders_ = folders;
    scan();
}

void LibraryModel::rescan()
{
    rescan_.start();
}

void LibraryModel::remove(const QString &path)
{
    if (const int found = row(path); found >= 0) {
        beginRemoveRows({}, found, found);
        entries_.removeAt(found);
        endRemoveRows();
    }
}

int LibraryModel::rowCount(const QModelIndex &parent) const
{
    return parent.isValid() ? 0 : static_cast<int>(entries_.size());
}

QVariant LibraryModel::data(const QModelIndex &index, int role) const
{
    if (!checkIndex(index, CheckIndexOption::IndexIsValid | CheckIndexOption::ParentIsInvalid))
        return {};
    const Entry &entry = entries_.at(index.row());
    switch (role) {
    case Qt::DisplayRole:
        return fileName(entry.path);
    case Qt::ToolTipRole: {
        const QLocale locale;
        return QString(fileName(entry.path) + u'\n' + locale.toString(entry.modified, QLocale::ShortFormat) + u'\n'
                       + locale.formattedDataSize(entry.size));
    }
    case PathRole:
        return entry.path;
    case VideoRole:
        return entry.video;
    case KindRole:
        return entry.video ? u"recording"_s : u"screenshot"_s;
    case ThumbnailRole:
        return thumbnailer_->thumbnail(entry);
    case ModifiedRole:
        return entry.modified;
    default:
        return {};
    }
}

Qt::ItemFlags LibraryModel::flags(const QModelIndex &index) const
{
    if (!index.isValid())
        return Qt::NoItemFlags;
    return Qt::ItemIsEnabled | Qt::ItemIsSelectable | Qt::ItemIsDragEnabled | Qt::ItemNeverHasChildren;
}

QStringList LibraryModel::mimeTypes() const
{
    return {u"text/uri-list"_s};
}

QMimeData *LibraryModel::mimeData(const QModelIndexList &indexes) const
{
    QList<QUrl> urls;
    urls.reserve(indexes.size());
    for (const QModelIndex &index : indexes) {
        if (index.isValid())
            urls.append(QUrl::fromLocalFile(entries_.at(index.row()).path));
    }
    auto *mime = new QMimeData;
    mime->setUrls(urls);
    return mime;
}

void LibraryModel::scan()
{
    rescan_.stop();
    scanner_.start([this, scan = ++scan_, folders = folders_] {
        Found found = scanFolders(folders, stopping_);
        // Delivered only while this exists: the destructor waits for the scan, and posted calls die with it.
        QMetaObject::invokeMethod(
            this,
            [this, scan, found = std::move(found)]() mutable {
                if (scan != scan_)
                    return;
                watch(found.directories);
                apply(std::move(found.files));
                if (!loaded_) {
                    loaded_ = true;
                    Q_EMIT loaded();
                }
            },
            Qt::QueuedConnection);
    });
}

void LibraryModel::watch(const QStringList &directories)
{
    const QStringList watched = watcher_.directories();
    const QSet<QString> now(watched.cbegin(), watched.cend());
    const QSet<QString> wanted(directories.cbegin(), directories.cend());
    if (const QSet<QString> gone = now - wanted; !gone.isEmpty())
        watcher_.removePaths(gone.values());
    if (const QSet<QString> added = wanted - now; !added.isEmpty())
        watcher_.addPaths(added.values());
}

void LibraryModel::apply(QList<Entry> entries)
{
    // Files gone, and files changed, which may have moved in the order: removed first.
    QHash<QString, const Entry *> next;
    next.reserve(entries.size());
    for (const Entry &entry : entries)
        next.insert(entry.path, &entry);
    auto kept = [&](const Entry &entry) {
        const Entry *found = next.value(entry.path);
        return found && found->modified == entry.modified && found->size == entry.size;
    };
    const qsizetype keeping = std::ranges::count_if(entries_, kept);
    if ((entries_.size() - keeping) + (entries.size() - keeping) > kMaxRowChanges) {
        beginResetModel();
        entries_ = std::move(entries);
        endResetModel();
        return;
    }
    for (qsizetype row = entries_.size() - 1; row >= 0; --row) {
        if (!kept(entries_.at(row))) {
            beginRemoveRows({}, static_cast<int>(row), static_cast<int>(row));
            entries_.removeAt(row);
            endRemoveRows();
        }
    }
    // The rows left are in the new order already, so the new files go in between them.
    qsizetype row = 0;
    for (const Entry &entry : entries) {
        if (row < entries_.size() && entries_.at(row).path == entry.path) {
            ++row;
            continue;
        }
        beginInsertRows({}, static_cast<int>(row), static_cast<int>(row));
        entries_.insert(row, entry);
        endInsertRows();
        ++row;
    }
}

int LibraryModel::row(const QString &path) const
{
    const auto found = std::ranges::find(entries_, path, &Entry::path);
    return found == entries_.cend() ? -1 : static_cast<int>(found - entries_.cbegin());
}

} // namespace blink
