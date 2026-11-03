#pragma once

#include "settings.h"

#include <QWidget>

class QAction;
class QComboBox;

namespace blink {

class LibraryModel;
class LibraryProxy;
class LibraryView;
class Thumbnailer;

// The screenshots and recordings in the save folders, as a grid of thumbnails, newest first and optionally grouped by
// day, month or year: open them, copy them, drag them into other apps, upload them or move them to the trash. Closing
// it frees all it holds.
class LibraryWindow final : public QWidget {
    Q_OBJECT

public:
    explicit LibraryWindow(const Settings &settings, QWidget *parent = nullptr);

    void setSettings(const Settings &settings);
    void rescan(); // Blink saved a capture

Q_SIGNALS:
    void uploadRequested(const QString &path);

protected:
    bool event(QEvent *event) override;

private:
    QStringList selectedPaths() const; // in the order shown
    void updateActions();
    void updateEmptyText();
    void showMenu(const QPoint &position);
    void open();
    void copy();
    void showInFolder();
    void moveToTrash();

    bool saveScreenshots_ = false;
    bool uploadable_ = false; // S3 storage is set up
    Thumbnailer *thumbnailer_;
    LibraryModel *model_;
    LibraryProxy *filter_;
    QComboBox *kind_;
    QComboBox *grouping_;
    LibraryView *view_;
    QAction *open_;
    QAction *copy_;
    QAction *showInFolder_;
    QAction *upload_;
    QAction *trash_;
};

} // namespace blink
