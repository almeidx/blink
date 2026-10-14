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

    // Stores `data` as <prefix><name>.
    void upload(const S3Config &config, const QByteArray &data, const QString &name, const QString &contentType, Callback done);
    // Stores the file at `path` as <prefix><name>, streaming it from disk.
    void uploadFile(const S3Config &config, const QString &path, const QString &name, const QString &contentType, Callback done);
    // Stores and then deletes a small text object, to check the configuration.
    void test(const S3Config &config, Callback done);

private:
    QNetworkAccessManager *network_;
};

} // namespace blink
