#include "settingsdialog.h"

#include "uploadpath.h"

#include <KGlobalAccel>

#include <QAction>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDialogButtonBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QProcess>
#include <QPushButton>
#include <QStyle>
#include <QTabWidget>
#include <QToolButton>
#include <QVBoxLayout>

#include <tuple>
#include <utility>
#include <vector>

namespace blink {
namespace {

using namespace Qt::StringLiterals;

auto fields(const S3Config &c)
{
    return std::tie(c.endpoint, c.region, c.bucket, c.accessKey, c.secretKey, c.path, c.publicUrl, c.pathStyle, c.publicRead);
}

auto fields(const Settings &s)
{
    return std::tuple_cat(std::tie(s.startAtLogin, s.notifications, s.captureCursor, s.windowDecorations, s.windowShadow, s.magnifier,
                                   s.saveScreenshots, s.screenshotFolder, s.recordingFolder, s.fps, s.quality, s.encoder, s.recordCursor,
                                   s.systemAudio, s.microphone, s.uploadScreenshots, s.uploadRecordings, s.copyUrl),
                          fields(s.s3));
}

bool same(const Settings &a, const Settings &b)
{
    return fields(a) == fields(b);
}

QCheckBox *checkBox(const QString &text, bool checked)
{
    auto *box = new QCheckBox(text);
    box->setChecked(checked);
    return box;
}

QLineEdit *lineEdit(const QString &text, const QString &placeholder = {})
{
    auto *edit = new QLineEdit(text);
    edit->setPlaceholderText(placeholder);
    return edit;
}

// Secondary text under a field, like KDE's contextual help.
QLabel *hintLabel(const QString &text)
{
    auto *label = new QLabel(text);
    label->setWordWrap(true);
    label->setForegroundRole(QPalette::PlaceholderText);
    return label;
}

template<typename T>
void addItem(QComboBox *combo, const QString &text, T value)
{
    combo->addItem(text, QVariant::fromValue(static_cast<int>(value)));
}

template<typename T>
T currentValue(const QComboBox *combo)
{
    return static_cast<T>(combo->currentData().toInt());
}

void selectValue(QComboBox *combo, int value)
{
    combo->setCurrentIndex(combo->findData(value));
}

// Drops the action's mnemonic marker but keeps "&&" escaped: form labels are buddies and would read a
// lone '&' as their own mnemonic.
QString withoutMnemonic(const QString &text)
{
    QString result;
    result.reserve(text.size());
    for (qsizetype i = 0; i < text.size(); ++i) {
        if (text[i] == u'&') {
            if (++i == text.size())
                break;
            if (text[i] == u'&')
                result += u'&';
        }
        result += text[i];
    }
    return result;
}

// Separated the way System Settings lists them.
QString keysText(const QAction *action)
{
    const QList<QKeySequence> keys = KGlobalAccel::self()->shortcut(action);
    if (keys.isEmpty())
        return SettingsDialog::tr("None");
    QString text = keys.first().toString(QKeySequence::NativeText);
    for (qsizetype i = 1; i < keys.size(); ++i)
        text += ", "_L1 + keys[i].toString(QKeySequence::NativeText);
    return text;
}

// The kglobalaccel component the actions are registered under, worked out the way KGlobalAccel does.
QString shortcutComponent(const QList<QAction *> &actions)
{
    if (!actions.isEmpty()) {
        const QVariant name = actions.first()->property("componentName");
        if (name.isValid())
            return name.toString();
    }
    return QCoreApplication::applicationName();
}

// kcm_keys selects the component named by its first argument once its list has loaded. kcmshell6
// shows the page on its own, for when System Settings is not installed.
void openShortcutSettings(const QString &component)
{
    const QStringList arguments{u"kcm_keys"_s, u"--args"_s, component};
    if (!QProcess::startDetached(u"systemsettings"_s, arguments))
        QProcess::startDetached(u"kcmshell6"_s, arguments);
}

} // namespace

SettingsDialog::SettingsDialog(const Settings &settings, const QList<QAction *> &shortcuts, QWidget *parent) : QDialog(parent)
{
    setAttribute(Qt::WA_DeleteOnClose);
    setWindowTitle(tr("Settings")); // Qt appends " — Blink"

    auto *tabs = new QTabWidget;
    tabs->setUsesScrollButtons(false);
    tabs->addTab(createGeneralTab(settings), QIcon::fromTheme(u"preferences-system"_s), tr("General"));
    tabs->addTab(createScreenshotsTab(settings), QIcon::fromTheme(u"camera-photo"_s), tr("Screenshots"));
    tabs->addTab(createRecordingTab(settings), QIcon::fromTheme(u"media-record"_s), tr("Recording"));
    tabs->addTab(createUploadTab(settings), QIcon::fromTheme(u"cloud-upload"_s), tr("Upload"));
    tabs->addTab(createShortcutsTab(shortcuts), QIcon::fromTheme(u"configure-shortcuts"_s), tr("Shortcuts"));

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Apply | QDialogButtonBox::Cancel);
    ok_ = buttons->button(QDialogButtonBox::Ok);
    apply_ = buttons->button(QDialogButtonBox::Apply);
    connect(buttons, &QDialogButtonBox::accepted, this, [this] {
        apply();
        accept();
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(apply_, &QPushButton::clicked, this, &SettingsDialog::apply);

    auto *layout = new QVBoxLayout(this);
    layout->addWidget(tabs);
    layout->addWidget(buttons);

    // Every checkbox, combo box and line edit in the dialog is a setting.
    for (const QCheckBox *box : findChildren<QCheckBox *>())
        connect(box, &QCheckBox::toggled, this, &SettingsDialog::updateApply);
    for (const QComboBox *combo : findChildren<QComboBox *>())
        connect(combo, &QComboBox::currentIndexChanged, this, &SettingsDialog::updateApply);
    for (const QLineEdit *edit : findChildren<QLineEdit *>())
        connect(edit, &QLineEdit::textChanged, this, &SettingsDialog::updateApply);

    // The form trims what it reads back, so start from that rather than `settings`.
    applied_ = formSettings();
    updatePath();
}

QWidget *SettingsDialog::createGeneralTab(const Settings &settings)
{
    startAtLogin_ = checkBox(tr("Start Blink when I log in"), settings.startAtLogin);
    notifications_ = checkBox(tr("Show notifications"), settings.notifications);

    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    form->addRow(tr("Startup:"), startAtLogin_);
    form->addRow(tr("Notifications:"), notifications_);
    return page;
}

QWidget *SettingsDialog::createScreenshotsTab(const Settings &settings)
{
    captureCursor_ = checkBox(tr("Include the mouse pointer"), settings.captureCursor);
    windowDecorations_ = checkBox(tr("Include title bar and borders"), settings.windowDecorations);
    windowShadow_ = checkBox(tr("Include shadow"), settings.windowShadow);
    magnifier_ = checkBox(tr("Show a magnifier while selecting"), settings.magnifier);
    saveScreenshots_ = checkBox(tr("Save screenshots to this folder:"), settings.saveScreenshots);
    screenshotFolder_ = lineEdit(settings.screenshotFolder);
    QWidget *folder = createFolderField(screenshotFolder_, tr("Save Screenshots To"));

    // KWin only draws the shadow of a window together with its decoration.
    windowShadow_->setEnabled(settings.windowDecorations);
    connect(windowDecorations_, &QCheckBox::toggled, windowShadow_, &QWidget::setEnabled);
    folder->setEnabled(settings.saveScreenshots);
    connect(saveScreenshots_, &QCheckBox::toggled, folder, &QWidget::setEnabled);

    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    form->addRow(tr("Mouse pointer:"), captureCursor_);
    form->addRow(tr("Active window:"), windowDecorations_);
    form->addRow(QString(), windowShadow_);
    form->addRow(tr("Region selection:"), magnifier_);
    form->addRow(tr("Saving:"), saveScreenshots_);
    form->addRow(QString(), folder);
    return page;
}

QWidget *SettingsDialog::createRecordingTab(const Settings &settings)
{
    recordingFolder_ = lineEdit(settings.recordingFolder);

    fps_ = new QComboBox;
    for (const int fps : {30, 60})
        addItem(fps_, tr("%1 FPS").arg(fps), fps);
    if (fps_->findData(settings.fps) < 0)
        addItem(fps_, tr("%1 FPS").arg(settings.fps), settings.fps);
    selectValue(fps_, settings.fps);

    quality_ = new QComboBox;
    addItem(quality_, tr("Low"), Quality::Low);
    addItem(quality_, tr("Medium"), Quality::Medium);
    addItem(quality_, tr("High"), Quality::High);
    selectValue(quality_, static_cast<int>(settings.quality));

    encoder_ = new QComboBox;
    addItem(encoder_, tr("Automatic"), EncoderPreference::Auto);
    addItem(encoder_, tr("NVIDIA NVENC"), EncoderPreference::Nvenc);
    addItem(encoder_, tr("VA-API"), EncoderPreference::Vaapi);
    addItem(encoder_, tr("Software"), EncoderPreference::Software);
    selectValue(encoder_, static_cast<int>(settings.encoder));

    recordCursor_ = checkBox(tr("Record the mouse pointer"), settings.recordCursor);
    systemAudio_ = checkBox(tr("Record system audio"), settings.systemAudio);
    microphone_ = checkBox(tr("Record the microphone"), settings.microphone);

    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    form->addRow(tr("Save recordings to:"), createFolderField(recordingFolder_, tr("Save Recordings To")));
    form->addRow(tr("Frame rate:"), fps_);
    form->addRow(tr("Quality:"), quality_);
    form->addRow(tr("Encoder:"), encoder_);
    form->addRow(QString(), hintLabel(tr("Automatic uses the first hardware encoder that works, and software otherwise.")));
    form->addRow(tr("Mouse pointer:"), recordCursor_);
    form->addRow(tr("Audio:"), systemAudio_);
    form->addRow(QString(), microphone_);
    return page;
}

QWidget *SettingsDialog::createUploadTab(const Settings &settings)
{
    uploadScreenshots_ = checkBox(tr("Upload screenshots"), settings.uploadScreenshots);
    uploadRecordings_ = checkBox(tr("Upload recordings"), settings.uploadRecordings);
    copyUrl_ = checkBox(tr("Copy the link after uploading"), settings.copyUrl);

    const S3Config &s3 = settings.s3;
    endpoint_ = lineEdit(s3.endpoint, u"https://s3.us-east-1.amazonaws.com"_s);
    region_ = lineEdit(s3.region, tr("us-east-1, or auto for Cloudflare R2"));
    bucket_ = lineEdit(s3.bucket);
    accessKey_ = lineEdit(s3.accessKey);
    secretKey_ = lineEdit(s3.secretKey);
    QWidget *path = createPathField(s3.path);
    publicUrl_ = lineEdit(s3.publicUrl, tr("Optional, e.g. https://i.example.com/"));
    pathStyle_ = checkBox(tr("Use path-style URLs"), s3.pathStyle);
    pathStyle_->setToolTip(tr("Address objects as https://endpoint/bucket/key instead of https://bucket.endpoint/key."));
    publicRead_ = checkBox(tr("Make uploads public"), s3.publicRead);
    publicRead_->setToolTip(tr("Sends x-amz-acl: public-read with every upload."));
    publicUrl_->setToolTip(tr("Links are built from this instead of the bucket's own address, e.g. for a custom domain or CDN."));

    secretKey_->setEchoMode(QLineEdit::Password);
    QAction *reveal = secretKey_->addAction(QIcon::fromTheme(u"view-visible"_s), QLineEdit::TrailingPosition);
    reveal->setToolTip(tr("Show the secret key"));
    connect(reveal, &QAction::triggered, this, [this, reveal] {
        const bool hidden = secretKey_->echoMode() == QLineEdit::Password;
        secretKey_->setEchoMode(hidden ? QLineEdit::Normal : QLineEdit::Password);
        reveal->setIcon(QIcon::fromTheme(hidden ? u"view-hidden"_s : u"view-visible"_s));
        reveal->setToolTip(hidden ? tr("Hide the secret key") : tr("Show the secret key"));
    });

    test_ = new QPushButton(QIcon::fromTheme(u"network-connect"_s), tr("Test"));
    test_->setToolTip(tr("Upload and delete a small file with these settings"));
    connect(test_, &QPushButton::clicked, this, &SettingsDialog::testUpload);
    testIcon_ = new QLabel;
    testIcon_->hide();
    testResult_ = new QLabel;
    testResult_->setWordWrap(true);
    testResult_->setTextFormat(Qt::PlainText);
    testResult_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    testResult_->setAlignment(Qt::AlignLeft | Qt::AlignTop); // stays beside the icon when the row is taller than the text
    pathIcon_ = new QLabel;
    pathIcon_->hide();
    pathExample_ = hintLabel(QString());
    pathExample_->setTextFormat(Qt::PlainText); // shows what was typed, which may look like markup
    auto *example = new QHBoxLayout;
    example->addWidget(pathIcon_, 0, Qt::AlignTop);
    example->addWidget(pathExample_, 1);
    for (const QLineEdit *edit : {endpoint_, bucket_, publicUrl_})
        connect(edit, &QLineEdit::textChanged, this, &SettingsDialog::updateExample);
    connect(pathStyle_, &QCheckBox::toggled, this, &SettingsDialog::updateExample);
    auto *result = new QHBoxLayout;
    result->addWidget(testIcon_, 0, Qt::AlignTop);
    result->addWidget(testResult_, 1);

    auto *storage = new QGroupBox(tr("S3-compatible storage"));
    auto *storageForm = new QFormLayout(storage);
    storageForm->addRow(tr("Endpoint:"), endpoint_);
    storageForm->addRow(tr("Region:"), region_);
    storageForm->addRow(tr("Bucket:"), bucket_);
    storageForm->addRow(tr("Access key:"), accessKey_);
    storageForm->addRow(tr("Secret key:"), secretKey_);
    storageForm->addRow(tr("Path in bucket:"), path);
    storageForm->addRow(QString(), example);
    storageForm->addRow(tr("Public URL:"), publicUrl_);
    storageForm->addRow(QString(), pathStyle_);
    storageForm->addRow(QString(), publicRead_);
    storageForm->addRow(QString(), test_);
    storageForm->addRow(QString(), result);

    auto *page = new QWidget;
    auto *layout = new QVBoxLayout(page);
    auto *form = new QFormLayout;
    form->addRow(tr("Upload:"), uploadScreenshots_);
    form->addRow(QString(), uploadRecordings_);
    form->addRow(tr("Clipboard:"), copyUrl_);
    layout->addLayout(form);
    layout->addWidget(storage);
    layout->addStretch();
    return page;
}

QWidget *SettingsDialog::createShortcutsTab(const QList<QAction *> &shortcuts)
{
    auto *page = new QWidget;
    auto *form = new QFormLayout(page);
    form->addRow(hintLabel(tr("Global shortcuts work everywhere in Plasma and are changed in System Settings.")));

    std::vector<std::pair<QAction *, QLabel *>> rows;
    rows.reserve(static_cast<size_t>(shortcuts.size()));
    for (QAction *action : shortcuts) {
        auto *keys = new QLabel(keysText(action));
        keys->setTextInteractionFlags(Qt::TextSelectableByMouse);
        form->addRow(withoutMnemonic(action->text()) + u':', keys);
        rows.emplace_back(action, keys);
    }
    connect(KGlobalAccel::self(), &KGlobalAccel::globalShortcutChanged, page,
            [rows = std::move(rows)](QAction *action, const QKeySequence &) {
                for (const auto &[rowAction, keys] : rows) {
                    if (rowAction == action)
                        keys->setText(keysText(action));
                }
            });

    auto *change = new QPushButton(QIcon::fromTheme(u"preferences-desktop-keyboard-shortcut"_s), tr("Change in System Settings…"));
    connect(change, &QPushButton::clicked, this, [component = shortcutComponent(shortcuts)] { openShortcutSettings(component); });
    form->addRow(QString(), change);
    return page;
}

QWidget *SettingsDialog::createFolderField(QLineEdit *edit, const QString &title)
{
    auto *browse = new QToolButton;
    browse->setIcon(QIcon::fromTheme(u"document-open-folder"_s));
    browse->setToolTip(tr("Choose a folder"));
    connect(browse, &QToolButton::clicked, this, [this, edit, title] {
        const QString folder = QFileDialog::getExistingDirectory(this, title, edit->text());
        if (!folder.isEmpty())
            edit->setText(folder);
    });

    auto *field = new QWidget;
    auto *layout = new QHBoxLayout(field);
    layout->setContentsMargins({});
    layout->addWidget(edit);
    layout->addWidget(browse);
    return field;
}

QWidget *SettingsDialog::createPathField(const QString &pattern)
{
    path_ = new QLineEdit(pattern);
    path_->setToolTip(tr("Where uploads go in the bucket. Placeholders in braces are filled in for each upload, and / separates "
                         "folders."));
    connect(path_, &QLineEdit::textChanged, this, &SettingsDialog::updatePath);

    // One list in sections: every placeholder is a glance away, with no submenus to hover through.
    auto *menu = new QMenu(this);
    for (const UploadPath::Group &group : UploadPath::groups()) {
        menu->addSection(group.title);
        for (const UploadPath::Placeholder &placeholder : group.placeholders) {
            // Text after a tab goes in the shortcut column.
            const QAction *action = menu->addAction(placeholder.description + u'\t' + placeholder.text);
            connect(action, &QAction::triggered, this, [this, text = placeholder.text] {
                path_->insert(text);
                path_->setFocus();
            });
        }
    }
    auto *insert = new QToolButton;
    insert->setIcon(QIcon::fromTheme(u"insert-text"_s));
    insert->setText(tr("Insert")); // shown without an icon theme, and read by screen readers
    insert->setToolTip(tr("Insert a placeholder"));
    insert->setPopupMode(QToolButton::InstantPopup);
    insert->setMenu(menu);

    auto *field = new QWidget;
    auto *layout = new QHBoxLayout(field);
    layout->setContentsMargins({});
    layout->addWidget(path_);
    layout->addWidget(insert);
    return field;
}

Settings SettingsDialog::formSettings() const
{
    Settings s;
    s.startAtLogin = startAtLogin_->isChecked();
    s.notifications = notifications_->isChecked();

    s.captureCursor = captureCursor_->isChecked();
    s.windowDecorations = windowDecorations_->isChecked();
    s.windowShadow = windowShadow_->isChecked();
    s.magnifier = magnifier_->isChecked();
    s.saveScreenshots = saveScreenshots_->isChecked();
    s.screenshotFolder = screenshotFolder_->text();

    s.recordingFolder = recordingFolder_->text();
    s.fps = fps_->currentData().toInt();
    s.quality = currentValue<Quality>(quality_);
    s.encoder = currentValue<EncoderPreference>(encoder_);
    s.recordCursor = recordCursor_->isChecked();
    s.systemAudio = systemAudio_->isChecked();
    s.microphone = microphone_->isChecked();

    s.uploadScreenshots = uploadScreenshots_->isChecked();
    s.uploadRecordings = uploadRecordings_->isChecked();
    s.copyUrl = copyUrl_->isChecked();
    // Pasted credentials and URLs often carry stray whitespace that would break signing.
    s.s3.endpoint = endpoint_->text().trimmed();
    s.s3.region = region_->text().trimmed();
    s.s3.bucket = bucket_->text().trimmed();
    s.s3.accessKey = accessKey_->text().trimmed();
    s.s3.secretKey = secretKey_->text().trimmed();
    s.s3.path = path_->text().trimmed();
    s.s3.publicUrl = publicUrl_->text().trimmed();
    s.s3.pathStyle = pathStyle_->isChecked();
    s.s3.publicRead = publicRead_->isChecked();
    return s;
}

void SettingsDialog::updateApply()
{
    const bool valid = pathError_.isEmpty();
    ok_->setEnabled(valid);
    apply_->setEnabled(valid && !same(formSettings(), applied_));
}

void SettingsDialog::apply()
{
    Settings settings = formSettings();
    if (same(settings, applied_))
        return;
    applied_ = std::move(settings);
    apply_->setEnabled(false);
    Q_EMIT applied(applied_);
}

void SettingsDialog::updatePath()
{
    pathError_.clear();
    pathKey_ = UploadPath::expand(path_->text().trimmed(), UploadPath::example(u"png"_s), &pathError_);
    updateExample();
    updateApply();
}

void SettingsDialog::updateExample()
{
    if (!pathError_.isEmpty()) {
        pathIcon_->setPixmap(smallIcon(u"emblem-error"_s));
        pathIcon_->show();
        pathExample_->setForegroundRole(QPalette::WindowText);
        pathExample_->setText(pathError_);
        return;
    }
    pathIcon_->hide();
    pathExample_->setForegroundRole(QPalette::PlaceholderText);
    const S3Config s3 = formSettings().s3;
    // Without an endpoint and bucket there is only the key to show.
    QString example = (s3.endpoint.isEmpty() || s3.bucket.isEmpty()) && s3.publicUrl.isEmpty() ? pathKey_ : Uploader::link(s3, pathKey_);
    // Zero-width spaces let a long link wrap at its slashes rather than widen the dialog.
    example.replace(u'/', u"/\u200B"_s);
    pathExample_->setText(tr("Example: %1").arg(example));
}

void SettingsDialog::testUpload()
{
    const S3Config config = formSettings().s3;
    if (!config.isComplete()) {
        showTestResult(u"emblem-error"_s, tr("Fill in the endpoint, region, bucket and both keys first."));
        return;
    }
    if (!uploader_)
        uploader_ = std::make_unique<Uploader>();
    test_->setEnabled(false);
    showTestResult(QString(), tr("Testing…"));
    uploader_->test(config, [this](const Uploader::Result &result) {
        test_->setEnabled(true);
        if (result.error.isEmpty())
            showTestResult(u"emblem-success"_s, tr("Uploaded and deleted a test file."));
        else
            showTestResult(u"emblem-error"_s, result.error);
    });
}

void SettingsDialog::showTestResult(const QString &iconName, const QString &text)
{
    if (iconName.isEmpty()) {
        testIcon_->hide();
    } else {
        testIcon_->setPixmap(smallIcon(iconName));
        testIcon_->show();
    }
    testResult_->setText(text);
    // Word-wrapped text does not make a window grow by itself. The layouts above the label only learn of
    // its new height through posted layout requests, so deliver those before asking.
    QCoreApplication::sendPostedEvents(nullptr, QEvent::LayoutRequest);
    if (const int needed = heightForWidth(width()); needed > height())
        resize(width(), needed);
}

QPixmap SettingsDialog::smallIcon(const QString &name) const
{
    const int size = style()->pixelMetric(QStyle::PM_SmallIconSize, nullptr, this);
    return QIcon::fromTheme(name).pixmap(QSize(size, size), devicePixelRatio());
}

} // namespace blink
