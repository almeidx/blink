#pragma once

#include <QCoreApplication>
#include <QDateTime>
#include <QList>
#include <QSize>
#include <QString>

namespace blink {

// What the placeholders in an upload path stand for.
struct UploadInfo {
    QDateTime time;    // when the capture was taken, local time
    QString fileName;  // the capture's file name without extension, e.g. Blink_2026-10-14_13-26-43
    QString extension; // png or mp4
    QSize size;        // in pixels; empty when unknown
};

// "Blink_2026-10-14_13-26-43": how Blink names the capture taken at `time`.
QString captureName(const QDateTime &time);

// Where uploads go in the bucket: a pattern such as "{random:8}/{filename}.{extension}", expanded into an S3 object
// key for each upload. Placeholders follow ShareX's file name codes where they apply to Blink.
class UploadPath {
    Q_DECLARE_TR_FUNCTIONS(blink::UploadPath)

public:
    struct Placeholder {
        QString text; // as inserted, e.g. "{random:8}"
        QString description;
    };
    struct Group {
        QString title;
        QList<Placeholder> placeholders;
    };

    // The object key for one upload: every placeholder expanded, each random one afresh, and empty folders dropped.
    // Empty, with `error` set, when the pattern is invalid.
    static QString expand(QStringView pattern, const UploadInfo &info, QString *error);
    // Values for previews and test uploads.
    static UploadInfo example(const QString &extension);
    // Every placeholder, grouped for the settings dialog's menu.
    static QList<Group> groups();
};

} // namespace blink
