#include "clipboard.h"

#include <KSystemClipboard>

#include <QMimeData>
#include <QUrl>

namespace blink::clipboard {
namespace {

using namespace Qt::StringLiterals;

class PngMimeData final : public QMimeData {
public:
    explicit PngMimeData(QFuture<QByteArray> png) : png_(std::move(png)) {}

    QStringList formats() const override { return {u"image/png"_s}; }
    bool hasFormat(const QString &mimeType) const override { return mimeType == "image/png"_L1; }

protected:
    QVariant retrieveData(const QString &mimeType, QMetaType type) const override
    {
        Q_UNUSED(type)
        if (mimeType != "image/png"_L1)
            return {};
        return png_.result(); // the encoder always reports a result, empty if it failed
    }

private:
    QFuture<QByteArray> png_;
};

} // namespace

void setPng(const QFuture<QByteArray> &png)
{
    KSystemClipboard::instance()->setMimeData(new PngMimeData(png), QClipboard::Clipboard);
}

void setFile(const QString &path)
{
    const QUrl url = QUrl::fromLocalFile(path);
    auto *mime = new QMimeData;
    mime->setUrls({url});
    mime->setData(u"x-special/gnome-copied-files"_s, "copy\n" + url.toEncoded());
    KSystemClipboard::instance()->setMimeData(mime, QClipboard::Clipboard);
}

void setText(const QString &text)
{
    auto *mime = new QMimeData;
    mime->setText(text);
    KSystemClipboard::instance()->setMimeData(mime, QClipboard::Clipboard);
}

} // namespace blink::clipboard
