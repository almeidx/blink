#pragma once

#include "pathpattern.h"
#include "settings.h"
#include "uploader.h"

#include <QDialog>
#include <QList>

#include <memory>
#include <utility>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPixmap;
class QPushButton;
class QWidget;

namespace blink {

class SettingsDialog final : public QDialog {
    Q_OBJECT

public:
    // `shortcuts` are the global shortcut actions, listed with their current keys.
    SettingsDialog(const Settings &settings, const QList<QAction *> &shortcuts, QWidget *parent = nullptr);

Q_SIGNALS:
    void applied(const blink::Settings &settings); // OK or Apply

private:
    // A line edit for a path pattern, and the line under it that shows an example or what is wrong with the pattern.
    struct PatternField {
        PathPattern::Target target = PathPattern::Target::ObjectKey;
        QString extension;           // of the example capture
        QLineEdit *folder = nullptr; // a subfolder's save folder, which its example starts with
        QLineEdit *edit = nullptr;
        QLabel *icon = nullptr;
        QLabel *example = nullptr;
        QString expanded = {}; // for the example; kept while the pattern is unchanged, so its random parts hold still
        QString error = {};    // what is wrong with the pattern; OK and Apply are disabled while it is set
    };

    QWidget *createGeneralTab(const Settings &settings);
    QWidget *createScreenshotsTab(const Settings &settings);
    QWidget *createRecordingTab(const Settings &settings);
    QWidget *createUploadTab(const Settings &settings);
    QWidget *createShortcutsTab(const QList<QAction *> &shortcuts);
    // A line edit with a button that picks a folder.
    QWidget *createFolderField(QLineEdit *edit, const QString &title);
    // `field`'s line edit, holding `pattern`, with a menu that inserts the placeholders its target takes; and the row
    // under it for the example. Connected after the edit holds `pattern`: their updates read the whole form.
    std::pair<QWidget *, QWidget *> createPatternField(PatternField &field, const QString &pattern, const QString &toolTip);

    Settings formSettings() const;
    void updateApply();
    void apply();
    void updatePattern(PatternField &field); // expands the pattern into a new example, or finds what is wrong with it
    void updateExample(PatternField &field); // shows the example, which also depends on other fields, or the error
    void testUpload();
    void showTestResult(const QString &iconName, const QString &text);
    QPixmap smallIcon(const QString &name) const;

    Settings applied_; // what the app last received; Apply is enabled while the form differs from it

    QCheckBox *startAtLogin_ = nullptr;
    QCheckBox *notifications_ = nullptr;

    QCheckBox *captureCursor_ = nullptr;
    QCheckBox *windowDecorations_ = nullptr;
    QCheckBox *windowShadow_ = nullptr;
    QCheckBox *magnifier_ = nullptr;
    QCheckBox *saveScreenshots_ = nullptr;
    QLineEdit *screenshotFolder_ = nullptr;
    PatternField screenshotSubfolder_;

    QLineEdit *recordingFolder_ = nullptr;
    PatternField recordingSubfolder_;
    QComboBox *fps_ = nullptr;
    QComboBox *quality_ = nullptr;
    QComboBox *encoder_ = nullptr;
    QCheckBox *recordCursor_ = nullptr;
    QCheckBox *systemAudio_ = nullptr;
    QCheckBox *microphone_ = nullptr;

    QCheckBox *uploadScreenshots_ = nullptr;
    QCheckBox *uploadRecordings_ = nullptr;
    QCheckBox *copyUrl_ = nullptr;
    QLineEdit *endpoint_ = nullptr;
    QLineEdit *region_ = nullptr;
    QLineEdit *bucket_ = nullptr;
    QLineEdit *accessKey_ = nullptr;
    QLineEdit *secretKey_ = nullptr;
    PatternField path_;
    QLineEdit *publicUrl_ = nullptr;
    QCheckBox *pathStyle_ = nullptr;
    QCheckBox *publicRead_ = nullptr;
    QPushButton *test_ = nullptr;
    QLabel *testIcon_ = nullptr;
    QLabel *testResult_ = nullptr;

    QPushButton *ok_ = nullptr;
    QPushButton *apply_ = nullptr;

    // Created on the first test. Last member: destroyed first, so a callback it runs while aborting a
    // test still finds the widgets alive.
    std::unique_ptr<Uploader> uploader_;
};

} // namespace blink
