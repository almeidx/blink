#pragma once

#include "thumbnailer.h"

#include <QAbstractListModel>
#include <QFileSystemWatcher>
#include <QList>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <atomic>

namespace blink {

// The images and videos in the save folders and their subfolders, newest first. Rescans by itself when the folders
// change on disk; rows are inserted and removed rather than reset, so the selection and scroll position stay.
class LibraryModel final : public QAbstractListModel {
    Q_OBJECT

public:
    enum Role {
        PathRole = Qt::UserRole,
        VideoRole,
        KindRole,      // "screenshot" or "recording", to filter on
        ThumbnailRole, // a QImage; asking for it starts making it, and dataChanged() follows
        ModifiedRole,  // a QDateTime, local time
    };

    explicit LibraryModel(Thumbnailer *thumbnailer, QObject *parent = nullptr);
    ~LibraryModel() override;

    void setFolders(const QStringList &folders); // scans at once when they changed
    void rescan();                               // shortly, so changes in quick succession take one scan
    void remove(const QString &path);            // at once, for a file the library itself moved away
    bool isLoaded() const { return loaded_; }    // the first scan is in: no rows now means no files

    int rowCount(const QModelIndex &parent = {}) const override;
    QVariant data(const QModelIndex &index, int role) const override;
    Qt::ItemFlags flags(const QModelIndex &index) const override;
    QStringList mimeTypes() const override;
    QMimeData *mimeData(const QModelIndexList &indexes) const override; // the files, for dragging into other apps

Q_SIGNALS:
    void loaded();

private:
    using Entry = Thumbnailer::File;

    void scan();
    void watch(const QStringList &directories);
    void apply(QList<Entry> entries);
    int row(const QString &path) const;

    Thumbnailer *thumbnailer_;
    QStringList folders_;
    QList<Entry> entries_;
    bool loaded_ = false;
    QFileSystemWatcher watcher_;
    QTimer rescan_;
    int scan_ = 0; // the latest scan; results of older ones are dropped
    std::atomic_bool stopping_ = false;
    QThreadPool scanner_; // last: destroyed first, after the scan in progress ends
};

} // namespace blink
