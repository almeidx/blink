#pragma once

#include "settings.h"
#include "uploader.h"

#include <QDialog>
#include <QList>

#include <memory>

class QAction;
class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
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
    QWidget *createGeneralTab(const Settings &settings);
    QWidget *createScreenshotsTab(const Settings &settings);
    QWidget *createRecordingTab(const Settings &settings);
    QWidget *createUploadTab(const Settings &settings);
    QWidget *createShortcutsTab(const QList<QAction *> &shortcuts);
    // A line edit with a button that picks a folder.
    QWidget *createFolderField(QLineEdit *edit, const QString &title);

    Settings formSettings() const;
    void updateApply();
    void apply();
    void testUpload();
    void showTestResult(const QString &iconName, const QString &text);

    Settings applied_; // what the app last received; Apply is enabled while the form differs from it

    QCheckBox *startAtLogin_ = nullptr;
    QCheckBox *notifications_ = nullptr;

    QCheckBox *captureCursor_ = nullptr;
    QCheckBox *windowDecorations_ = nullptr;
    QCheckBox *windowShadow_ = nullptr;
    QCheckBox *magnifier_ = nullptr;
    QCheckBox *saveScreenshots_ = nullptr;
    QLineEdit *screenshotFolder_ = nullptr;

    QLineEdit *recordingFolder_ = nullptr;
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
    QLineEdit *prefix_ = nullptr;
    QLineEdit *publicUrl_ = nullptr;
    QCheckBox *pathStyle_ = nullptr;
    QCheckBox *publicRead_ = nullptr;
    QPushButton *test_ = nullptr;
    QLabel *testIcon_ = nullptr;
    QLabel *testResult_ = nullptr;

    QPushButton *apply_ = nullptr;

    // Created on the first test. Last member: destroyed first, so a callback it runs while aborting a
    // test still finds the widgets alive.
    std::unique_ptr<Uploader> uploader_;
};

} // namespace blink
