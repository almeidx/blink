#pragma once

#include "settings.h"

#include <QObject>

#include <functional>

class QNetworkAccessManager;

namespace blink {

// Uploads to S3-compatible storage with single PUTs signed with AWS Signature Version 4.
class Uploader final : public QObject {
    Q_OBJECT

public:
    struct Result {
        QString url;   // the object's public link, on success
        QString error; // human-readable, on failure
    };
    using Callback = std::function<void(const Result &)>;

    explicit Uploader(QObject *parent = nullptr);

    // Stores `data` as the object `key`.
    void upload(const S3Config &config, const QByteArray &data, const QString &key, const QString &contentType, Callback done);
    // Stores the file at `path` as the object `key`, streaming it from disk.
    void uploadFile(const S3Config &config, const QString &path, const QString &key, const QString &contentType, Callback done);
    // Stores and then deletes a small text object in the folder that config.path puts uploads in, to check the
    // configuration.
    void test(const S3Config &config, Callback done);

    // The link handed out for the object `key`.
    static QString link(const S3Config &config, const QString &key);

private:
    QNetworkAccessManager *network_;
};

} // namespace blink
