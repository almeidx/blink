#include "uploader.h"

#include "pathpattern.h"

#include <QCryptographicHash>
#include <QDateTime>
#include <QFile>
#include <QFuture>
#include <QMessageAuthenticationCode>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPromise>
#include <QRandomGenerator>
#include <QThreadPool>
#include <QUrl>
#include <QVarLengthArray>
#include <QXmlStreamReader>

#include <chrono>
#include <memory>
#include <span>
#include <utility>

namespace blink {
namespace {

using namespace Qt::StringLiterals;
using namespace std::chrono_literals;

// Without a byte moving in either direction for this long, a request fails. Big uploads keep moving, so they are fine.
constexpr auto kTransferTimeout = 30s;

// S3 accepts an unsigned body over TLS, which spares hashing it; over plain http the body's hash is signed.
const auto kUnsignedPayload = "UNSIGNED-PAYLOAD"_ba;

struct Header {
    QByteArray name; // lowercase
    QByteArray value;
};

// Where an object lives.
struct Object {
    QUrl url;
    QByteArray host;         // the Host header, which is signed
    QByteArray canonicalUri; // the URL's path, encoded the way Signature Version 4 wants it
    QString link;            // handed out to the user
    bool https = true;
};

QByteArray sha256Hex(const QByteArray &data)
{
    return QCryptographicHash::hash(data, QCryptographicHash::Sha256).toHex();
}

QByteArray hmac(const QByteArray &key, const QByteArray &message)
{
    return QMessageAuthenticationCode::hash(message, key, QCryptographicHash::Sha256);
}

// Percent-encodes each segment of `path` as AWS does (everything but RFC 3986's unreserved characters), keeping the
// slashes between them.
QByteArray encodePath(const QString &path)
{
    QByteArray encoded;
    for (const QStringView segment : QStringView(path).split(u'/'))
        encoded += segment.toUtf8().toPercentEncoding() + '/';
    encoded.chop(1);
    return encoded;
}

Object locate(const S3Config &config, const QString &key)
{
    const QString endpoint = config.endpoint.trimmed();
    QUrl url(endpoint.contains("://"_L1) ? endpoint : u"https://"_s + endpoint);
    // An endpoint may have a path of its own (a proxy's, say).
    QByteArray path = encodePath(url.path());
    while (path.endsWith('/'))
        path.chop(1);
    if (config.pathStyle)
        path += '/' + encodePath(config.bucket);
    else
        url.setHost(config.bucket + u'.' + url.host());
    const QByteArray encodedKey = encodePath(key);
    path += '/' + encodedKey;
    url.setPath(QString::fromLatin1(path), QUrl::StrictMode);
    url.setQuery(QString());
    url.setFragment(QString());

    Object object;
    object.url = url;
    object.host = url.authority(QUrl::FullyEncoded).toLatin1();
    object.canonicalUri = path;
    object.https = url.scheme() == "https"_L1;
    if (config.publicUrl.isEmpty()) {
        object.link = url.toString(QUrl::FullyEncoded);
    } else {
        QStringView base(config.publicUrl);
        while (base.endsWith(u'/'))
            base.chop(1);
        object.link = base + u'/' + QString::fromLatin1(encodedKey);
    }
    return object;
}

// The Authorization header of a Signature Version 4 request without a query string. `headers` are the signed ones,
// sorted by name; `timestamp` is their x-amz-date (yyyyMMddTHHmmssZ).
QByteArray authorization(const S3Config &config, const QByteArray &method, const QByteArray &canonicalUri, std::span<const Header> headers,
                         const QByteArray &payloadHash, const QByteArray &timestamp)
{
    QByteArray signedHeaders;
    QByteArray canonicalRequest = method + '\n' + canonicalUri + "\n\n";
    for (const Header &header : headers) {
        canonicalRequest += header.name + ':' + header.value + '\n';
        if (!signedHeaders.isEmpty())
            signedHeaders += ';';
        signedHeaders += header.name;
    }
    canonicalRequest += '\n' + signedHeaders + '\n' + payloadHash;

    const QByteArray date = timestamp.first(8);
    const QByteArray region = config.region.toUtf8();
    const QByteArray scope = date + '/' + region + "/s3/aws4_request";
    const QByteArray stringToSign = "AWS4-HMAC-SHA256\n" + timestamp + '\n' + scope + '\n' + sha256Hex(canonicalRequest);
    const QByteArray key = hmac(hmac(hmac(hmac("AWS4" + config.secretKey.toUtf8(), date), region), "s3"_ba), "aws4_request"_ba);
    return "AWS4-HMAC-SHA256 Credential=" + config.accessKey.toUtf8() + '/' + scope + ",SignedHeaders=" + signedHeaders
         + ",Signature=" + hmac(key, stringToSign).toHex();
}

QNetworkRequest signedRequest(const S3Config &config, const Object &object, const QByteArray &method, const QByteArray &payloadHash,
                              bool publicRead)
{
    const QByteArray timestamp = QDateTime::currentDateTimeUtc().toString(u"yyyyMMdd'T'HHmmss'Z'"_s).toLatin1();
    QVarLengthArray<Header, 4> headers;
    headers.append({"host"_ba, object.host});
    if (publicRead)
        headers.append({"x-amz-acl"_ba, "public-read"_ba});
    headers.append({"x-amz-content-sha256"_ba, payloadHash});
    headers.append({"x-amz-date"_ba, timestamp});

    QNetworkRequest request(object.url);
    for (const Header &header : headers)
        request.setRawHeader(header.name, header.value);
    request.setRawHeader("authorization"_ba, authorization(config, method, object.canonicalUri, headers, payloadHash, timestamp));
    request.setTransferTimeout(kTransferTimeout);
    // The signature covers the host, so a redirect can never succeed, and following one would hand the signature to
    // whatever host the server named. Qt's default policy does exactly that for https targets.
    request.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QVariant::fromValue(QNetworkRequest::ManualRedirectPolicy));
    return request;
}

QString errorText(QNetworkReply *reply)
{
    // The transfer timeout aborts the reply, and nothing else does while `reply` is alive.
    if (reply->error() == QNetworkReply::OperationCanceledError)
        return Uploader::tr("The server stopped responding");
    const int status = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    if (reply->error() == QNetworkReply::NoError) {
        if (status >= 200 && status < 300)
            return {};
        // Redirects are not followed (signedRequest), and Qt does not count an unfollowed one as an error. The object
        // was not stored, so the user must not get a link; the target host tells them which endpoint to configure.
        const QUrl location = reply->header(QNetworkRequest::LocationHeader).toUrl();
        if (status >= 300 && status < 400 && !location.host().isEmpty())
            return Uploader::tr("The server redirected to %1 (HTTP %2); use that endpoint in the settings")
                .arg(location.host())
                .arg(status);
        return Uploader::tr("Unexpected answer (HTTP %1)").arg(status);
    }
    QString code;
    QString message;
    QXmlStreamReader xml(reply->readAll());
    while (!xml.atEnd()) {
        if (xml.readNext() != QXmlStreamReader::StartElement)
            continue;
        if (xml.name() == "Code"_L1)
            code = xml.readElementText();
        else if (xml.name() == "Message"_L1)
            message = xml.readElementText();
    }
    if (code.isEmpty())
        return reply->errorString();
    return message.isEmpty() ? u"%1 (HTTP %2)"_s.arg(code).arg(status) : u"%1: %2 (HTTP %3)"_s.arg(code, message).arg(status);
}

// Calls `then` with the reply's error text (empty on success) once it finishes, unless `context` is gone by then.
void whenFinished(QNetworkReply *reply, QObject *context, std::function<void(const QString &error)> then)
{
    QObject::connect(reply, &QNetworkReply::finished, context, [reply, then = std::move(then)] {
        reply->deleteLater();
        then(errorText(reply));
    });
}

QFuture<QByteArray> hashFile(const QString &path)
{
    QPromise<QByteArray> promise;
    QFuture<QByteArray> future = promise.future();
    QThreadPool::globalInstance()->start([path, promise = std::move(promise)] mutable {
        promise.start();
        QFile file(path);
        QCryptographicHash hash(QCryptographicHash::Sha256);
        // Unreadable: the upload, which opens the file again, reports it.
        promise.addResult(file.open(QIODevice::ReadOnly) && hash.addData(&file) ? hash.result().toHex() : QByteArray());
        promise.finish();
    });
    return future;
}

} // namespace

Uploader::Uploader(QObject *parent) : QObject(parent), network_(new QNetworkAccessManager(this)) {}

void Uploader::upload(const S3Config &config, const QByteArray &data, const QString &key, const QString &contentType, Callback done)
{
    const Object object = locate(config, key);
    QNetworkRequest request = signedRequest(config, object, "PUT"_ba, object.https ? kUnsignedPayload : sha256Hex(data), config.publicRead);
    request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
    whenFinished(network_->put(request, data), this,
                 [link = object.link, done = std::move(done)](const QString &error) { done({error.isEmpty() ? link : QString(), error}); });
}

void Uploader::uploadFile(const S3Config &config, const QString &path, const QString &key, const QString &contentType, Callback done)
{
    const auto send = [this, config, path, contentType, done = std::move(done)](const Object &object, const QByteArray &payloadHash) {
        auto file = std::make_unique<QFile>(path);
        if (!file->open(QIODevice::ReadOnly)) {
            const Result result{{}, tr("Cannot read %1: %2").arg(path, file->errorString())};
            QMetaObject::invokeMethod(this, [done, result] { done(result); }, Qt::QueuedConnection);
            return;
        }
        QNetworkRequest request = signedRequest(config, object, "PUT"_ba, payloadHash, config.publicRead);
        request.setHeader(QNetworkRequest::ContentTypeHeader, contentType);
        request.setHeader(QNetworkRequest::ContentLengthHeader, file->size());
        QNetworkReply *reply = network_->put(request, file.get());
        file.release()->setParent(reply);
        whenFinished(reply, this, [link = object.link, done](const QString &error) { done({error.isEmpty() ? link : QString(), error}); });
    };

    Object object = locate(config, key);
    if (object.https) {
        send(object, kUnsignedPayload);
        return;
    }
    hashFile(path).then(this, [send, object = std::move(object)](const QByteArray &hash) { send(object, hash); });
}

void Uploader::test(const S3Config &config, Callback done)
{
    QString error;
    const QString example = PathPattern::expand(config.path, PathPattern::Target::ObjectKey, PathPattern::example(u"txt"_s), &error);
    if (!error.isEmpty()) {
        QMetaObject::invokeMethod(this, [done = std::move(done), error] { done({{}, error}); }, Qt::QueuedConnection);
        return;
    }
    // Where uploads go, so permissions limited to that folder are what is checked, but under a name no upload gets:
    // a path without random parts must not have the test replace and delete a real upload.
    const QString folder = example.first(example.lastIndexOf(u'/') + 1);
    const Object object = locate(config, folder + u"blink-test-%1.txt"_s.arg(QRandomGenerator::global()->generate64(), 16, 16, u'0'));
    const QByteArray body = "Blink checks that it can upload here.\n"_ba;
    QNetworkRequest put = signedRequest(config, object, "PUT"_ba, object.https ? kUnsignedPayload : sha256Hex(body), config.publicRead);
    put.setHeader(QNetworkRequest::ContentTypeHeader, u"text/plain"_s);
    whenFinished(network_->put(put, body), this, [this, config, object, done = std::move(done)](const QString &error) {
        if (!error.isEmpty()) {
            done({{}, error});
            return;
        }
        const QNetworkRequest remove = signedRequest(config, object, "DELETE"_ba, object.https ? kUnsignedPayload : sha256Hex({}), false);
        whenFinished(network_->deleteResource(remove), this,
                     [link = object.link, done](const QString &error) { done({error.isEmpty() ? link : QString(), error}); });
    });
}

QString Uploader::link(const S3Config &config, const QString &key)
{
    return locate(config, key).link;
}

} // namespace blink
