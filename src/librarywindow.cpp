#include "librarywindow.h"

#include "clipboard.h"
#include "librarymodel.h"
#include "thumbnailer.h"

#include <QAction>
#include <QApplication>
#include <QComboBox>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusPendingCallWatcher>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPromise>
#include <QSortFilterProxyModel>
#include <QStyledItemDelegate>
#include <QThreadPool>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>
#include <memory>

namespace blink {

// A grid that says why it is empty when it is.
class LibraryView final : public QListView {
public:
    using QListView::QListView;

    void setEmptyText(const QString &text)
    {
        if (text == emptyText_)
            return;
        emptyText_ = text;
        viewport()->update();
    }

protected:
    void paintEvent(QPaintEvent *event) override
    {
        QListView::paintEvent(event);
        if (emptyText_.isEmpty() || !model() || model()->rowCount() > 0)
            return;
        QPainter painter(viewport());
        painter.setPen(palette().color(QPalette::PlaceholderText));
        painter.drawText(viewport()->rect().adjusted(32, 32, -32, -32), Qt::AlignCenter | Qt::TextWordWrap, emptyText_);
    }

private:
    QString emptyText_;
};

namespace {

using namespace Qt::StringLiterals;

constexpr QSize kThumbnail(224, 140); // logical pixels; 16:10, and four to a row at the default width
constexpr int kPadding = 6;
constexpr int kPlaceholderIcon = 48;
constexpr qreal kBadge = 28;

// Tells a recording from a screenshot: a play sign on a dark disc in the thumbnail's bottom left corner.
void drawPlayBadge(QPainter *painter, const QRectF &thumbnail)
{
    const qreal size = std::min(kBadge, std::min(thumbnail.width(), thumbnail.height()) / 2);
    const qreal margin = size / 4;
    const QRectF disc(thumbnail.left() + margin, thumbnail.bottom() - margin - size, size, size);
    const QPointF center = disc.center();
    const qreal arm = size / 4.5;
    QPainterPath triangle;
    triangle.moveTo(center.x() - (arm * 0.7), center.y() - arm);
    triangle.lineTo(center.x() + arm, center.y());
    triangle.lineTo(center.x() - (arm * 0.7), center.y() + arm);
    triangle.closeSubpath();

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing);
    painter->setPen(Qt::NoPen);
    painter->setBrush(QColor(0, 0, 0, 150));
    painter->drawEllipse(disc);
    painter->setBrush(Qt::white);
    painter->drawPath(triangle);
    painter->restore();
}

// A thumbnail centered in a fixed box, with the file name under it.
class LibraryDelegate final : public QStyledItemDelegate {
public:
    using QStyledItemDelegate::QStyledItemDelegate;

    QSize sizeHint(const QStyleOptionViewItem &option, const QModelIndex &) const override
    {
        return {kThumbnail.width() + (2 * kPadding), kThumbnail.height() + (3 * kPadding) + option.fontMetrics.height()};
    }

    void paint(QPainter *painter, const QStyleOptionViewItem &option, const QModelIndex &index) const override
    {
        QStyleOptionViewItem item = option;
        initStyleOption(&item, index);
        const QWidget *widget = item.widget;
        const QStyle *style = widget ? widget->style() : QApplication::style();
        // The hover and selection background the style gives any item view.
        style->drawPrimitive(QStyle::PE_PanelItemViewItem, &item, painter, widget);

        const QRect box(item.rect.left() + kPadding, item.rect.top() + kPadding, item.rect.width() - (2 * kPadding), kThumbnail.height());
        const bool video = index.data(LibraryModel::VideoRole).toBool();
        const auto image = index.data(LibraryModel::ThumbnailRole).value<QImage>();
        if (image.isNull()) {
            const QRect icon(box.center() - QPoint(kPlaceholderIcon / 2, kPlaceholderIcon / 2), QSize(kPlaceholderIcon, kPlaceholderIcon));
            QIcon::fromTheme(video ? u"video-x-generic"_s : u"image-x-generic"_s).paint(painter, icon);
        } else {
            // On whole device pixels, so the image is drawn as it is rather than resampled.
            const qreal ratio = image.devicePixelRatio();
            const QSizeF size = image.deviceIndependentSize();
            const QPointF topLeft(std::round((box.left() + ((box.width() - size.width()) / 2)) * ratio) / ratio,
                                  std::round((box.top() + ((box.height() - size.height()) / 2)) * ratio) / ratio);
            painter->drawImage(topLeft, image);
            if (video)
                drawPlayBadge(painter, QRectF(topLeft, size));
        }

        const QRect text(item.rect.left() + kPadding, box.bottom() + 1 + kPadding, item.rect.width() - (2 * kPadding),
                         item.fontMetrics.height());
        const QPalette::ColorGroup group = !item.state.testFlag(QStyle::State_Enabled) ? QPalette::Disabled
                                         : item.state.testFlag(QStyle::State_Active)   ? QPalette::Active
                                                                                       : QPalette::Inactive;
        painter->save();
        painter->setFont(item.font);
        painter->setPen(
            item.palette.color(group, item.state.testFlag(QStyle::State_Selected) ? QPalette::HighlightedText : QPalette::Text));
        painter->drawText(text, Qt::AlignCenter, item.fontMetrics.elidedText(item.text, Qt::ElideMiddle, text.width()));
        painter->restore();
    }
};

} // namespace

LibraryWindow::LibraryWindow(const Settings &settings, QWidget *parent)
    : QWidget(parent)
    , thumbnailer_(new Thumbnailer(kThumbnail, this))
    , model_(new LibraryModel(thumbnailer_, this))
    , filter_(new QSortFilterProxyModel(this))
    , kind_(new QComboBox)
    , view_(new LibraryView)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Library")); // Qt appends " — Blink"
    resize(1000, 700);

    filter_->setSourceModel(model_);
    filter_->setFilterRole(LibraryModel::KindRole);
    kind_->addItem(tr("All"), QString());
    kind_->addItem(tr("Screenshots"), u"screenshot"_s);
    kind_->addItem(tr("Recordings"), u"recording"_s);
    connect(kind_, &QComboBox::currentIndexChanged, this, [this] {
        filter_->setFilterFixedString(kind_->currentData().toString());
        updateEmptyText();
    });

    view_->setModel(filter_);
    view_->setItemDelegate(new LibraryDelegate(view_));
    view_->setViewMode(QListView::IconMode);
    view_->setMovement(QListView::Static);
    view_->setResizeMode(QListView::Adjust);
    view_->setUniformItemSizes(true);
    view_->setSpacing(2);
    view_->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    view_->setSelectionMode(QAbstractItemView::ExtendedSelection);
    view_->setDragDropMode(QAbstractItemView::DragOnly); // into other apps, as files
    view_->setDefaultDropAction(Qt::CopyAction);
    view_->setContextMenuPolicy(Qt::CustomContextMenu);
    connect(view_, &QListView::activated, this, &LibraryWindow::open);
    connect(view_, &QWidget::customContextMenuRequested, this, &LibraryWindow::showMenu);
    connect(view_->selectionModel(), &QItemSelectionModel::selectionChanged, this, &LibraryWindow::updateActions);
    connect(model_, &LibraryModel::loaded, this, &LibraryWindow::updateEmptyText);

    open_ = new QAction(QIcon::fromTheme(u"document-open"_s), tr("Open"), this);
    copy_ = new QAction(QIcon::fromTheme(u"edit-copy"_s), tr("Copy"), this);
    copy_->setShortcut(QKeySequence::Copy);
    showInFolder_ = new QAction(QIcon::fromTheme(u"document-open-folder"_s), tr("Open Containing Folder"), this);
    upload_ = new QAction(QIcon::fromTheme(u"cloud-upload"_s), tr("Upload"), this);
    trash_ = new QAction(QIcon::fromTheme(u"user-trash"_s), tr("Move to Trash"), this);
    trash_->setShortcut(QKeySequence::Delete);
    connect(open_, &QAction::triggered, this, &LibraryWindow::open);
    connect(copy_, &QAction::triggered, this, &LibraryWindow::copy);
    connect(showInFolder_, &QAction::triggered, this, &LibraryWindow::showInFolder);
    connect(upload_, &QAction::triggered, this, [this] {
        if (const QStringList paths = selectedPaths(); paths.size() == 1)
            Q_EMIT uploadRequested(paths.first());
    });
    connect(trash_, &QAction::triggered, this, &LibraryWindow::moveToTrash);
    for (QAction *action : {open_, copy_, showInFolder_, upload_, trash_}) {
        action->setShortcutContext(Qt::WidgetWithChildrenShortcut);
        addAction(action);
    }

    auto *show = new QLabel(tr("Show:"));
    show->setBuddy(kind_);
    auto *bar = new QHBoxLayout;
    bar->addWidget(show);
    bar->addWidget(kind_);
    bar->addStretch();
    auto *layout = new QVBoxLayout(this);
    layout->addLayout(bar);
    layout->addWidget(view_);

    setSettings(settings);
}

void LibraryWindow::setSettings(const Settings &settings)
{
    saveScreenshots_ = settings.saveScreenshots;
    uploadable_ = settings.s3.isComplete();
    model_->setFolders({QDir::cleanPath(settings.screenshotFolder), QDir::cleanPath(settings.recordingFolder)});
    updateActions();
    updateEmptyText();
}

void LibraryWindow::rescan()
{
    model_->rescan();
}

bool LibraryWindow::event(QEvent *event)
{
    // Thumbnails are made for the scale of the screen the window is on, which is known once it is shown.
    if (event->type() == QEvent::Show || event->type() == QEvent::DevicePixelRatioChange) {
        thumbnailer_->setDevicePixelRatio(devicePixelRatio());
        view_->viewport()->update();
    }
    return QWidget::event(event);
}

QStringList LibraryWindow::selectedPaths() const
{
    QModelIndexList indexes = view_->selectionModel()->selectedIndexes();
    std::ranges::sort(indexes, {}, &QModelIndex::row);
    QStringList paths;
    paths.reserve(indexes.size());
    for (const QModelIndex &index : indexes)
        paths.append(index.data(LibraryModel::PathRole).toString());
    return paths;
}

void LibraryWindow::updateActions()
{
    const qsizetype selected = view_->selectionModel()->selectedIndexes().size();
    for (QAction *action : {open_, copy_, showInFolder_, trash_})
        action->setEnabled(selected > 0);
    upload_->setVisible(uploadable_);
    upload_->setEnabled(uploadable_ && selected == 1);
}

void LibraryWindow::updateEmptyText()
{
    // Nothing until the first scan is in: an empty grid then only means it has not looked yet.
    if (!model_->isLoaded()) {
        view_->setEmptyText({});
        return;
    }
    const QString kind = kind_->currentData().toString();
    QString text = kind == "recording"_L1 ? tr("No recordings") : kind == "screenshot"_L1 ? tr("No screenshots") : tr("No captures yet");
    if (kind != "recording"_L1 && !saveScreenshots_)
        text += u"\n\n"_s + tr("Screenshots are not being saved. Turn on “Save screenshots to this folder” in the settings to keep them.");
    view_->setEmptyText(text);
}

void LibraryWindow::showMenu(const QPoint &position)
{
    const QModelIndex index = view_->indexAt(position);
    if (!index.isValid())
        return;
    if (!view_->selectionModel()->isSelected(index))
        view_->setCurrentIndex(index);
    QMenu menu(this);
    menu.addAction(open_);
    menu.setDefaultAction(open_);
    menu.addAction(copy_);
    menu.addAction(showInFolder_);
    if (uploadable_) {
        menu.addSeparator();
        menu.addAction(upload_);
    }
    menu.addSeparator();
    menu.addAction(trash_);
    menu.exec(view_->viewport()->mapToGlobal(position));
}

void LibraryWindow::open()
{
    for (const QString &path : selectedPaths())
        QDesktopServices::openUrl(QUrl::fromLocalFile(path));
}

void LibraryWindow::copy()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;
    // One screenshot goes on as an image, as a capture does; anything else as files, as file managers copy them.
    if (paths.size() == 1 && paths.first().endsWith(".png"_L1, Qt::CaseInsensitive)) {
        auto promise = std::make_shared<QPromise<QByteArray>>();
        promise->start();
        clipboard::setPng(promise->future());
        QThreadPool::globalInstance()->start([path = paths.first(), promise] {
            QFile file(path);
            promise->addResult(file.open(QIODevice::ReadOnly) ? file.readAll() : QByteArray());
            promise->finish();
        });
        return;
    }
    clipboard::setFiles(paths);
}

void LibraryWindow::showInFolder()
{
    const QStringList paths = selectedPaths();
    if (paths.isEmpty())
        return;
    QStringList uris;
    uris.reserve(paths.size());
    for (const QString &path : paths)
        uris.append(QUrl::fromLocalFile(path).toString(QUrl::FullyEncoded));
    QDBusMessage message = QDBusMessage::createMethodCall(u"org.freedesktop.FileManager1"_s, u"/org/freedesktop/FileManager1"_s,
                                                          u"org.freedesktop.FileManager1"_s, u"ShowItems"_s);
    message << uris << QString();
    auto *watcher = new QDBusPendingCallWatcher(QDBusConnection::sessionBus().asyncCall(message), this);
    connect(watcher, &QDBusPendingCallWatcher::finished, this,
            [folder = QFileInfo(paths.first()).absolutePath()](QDBusPendingCallWatcher *watcher) {
                watcher->deleteLater();
                // Without a file manager that selects files, the folder opens.
                if (watcher->isError())
                    QDesktopServices::openUrl(QUrl::fromLocalFile(folder));
            });
}

void LibraryWindow::moveToTrash()
{
    QStringList failed;
    for (const QString &path : selectedPaths()) {
        if (QFile::moveToTrash(path))
            model_->remove(path);
        else
            failed.append(QFileInfo(path).fileName());
    }
    if (!failed.isEmpty())
        QMessageBox::warning(this, tr("Not Moved to the Trash"),
                             tr("These files could not be moved to the trash:\n%1").arg(failed.join(u'\n')));
}

} // namespace blink
