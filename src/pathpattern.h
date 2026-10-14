#pragma once

#include <QCoreApplication>
#include <QDateTime>
#include <QList>
#include <QSize>
#include <QString>

namespace blink {

// What the placeholders in a path pattern stand for.
struct CaptureInfo {
    QDateTime time;    // when the capture was taken, local time
    QString fileName;  // the capture's file name without extension, e.g. Blink_2026-10-14_13-26-43
    QString extension; // png or mp4
    QSize size;        // in pixels; empty when unknown
};

// "Konsole_2026-10-14_13-26-43": how Blink names a capture taken at `time` of a window of `app` (see appName()), and
// "Blink_2026-10-14_13-26-43" when it isn't of one window. Of `app`, only letters, digits, '-', '_' and '.' stay; other
// characters become dashes, so names work in links and on every file system: "Visual-Studio-Code_...".
QString captureName(const QDateTime &time, QStringView app = {});

// Where a capture goes: a pattern such as "{random:8}/{filename}.{extension}" or "{year}-{month}", expanded for each
// capture. Placeholders follow ShareX's file name codes where they apply to Blink.
class PathPattern {
    Q_DECLARE_TR_FUNCTIONS(blink::PathPattern)

public:
    enum class Target {
        ObjectKey, // where uploads go in the bucket; never empty
        // The folder saved captures go in, inside the save folder; empty for the save folder itself. Takes only the
        // date, time and computer placeholders: the others would give each capture a folder of its own, or aren't
        // known yet when a recording starts.
        Subfolder,
    };
    struct Placeholder {
        QString text; // as inserted, e.g. "{random:8}"
        QString description;
    };
    struct Group {
        QString title;
        QList<Placeholder> placeholders;
    };

    // `pattern` for one capture: every placeholder expanded, each random one afresh, and empty folders dropped.
    // Empty, with `error` set, when the pattern is invalid for `target`.
    static QString expand(QStringView pattern, Target target, const CaptureInfo &info, QString *error);
    // Values for previews and test uploads.
    static CaptureInfo example(const QString &extension);
    // The placeholders `target` takes, grouped for the settings dialog's menus.
    static QList<Group> groups(Target target);
};

} // namespace blink
