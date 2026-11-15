#include "settings.h"

#include "recorderclient.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>
#include <string_view>

#include <fcntl.h>
#include <unistd.h>

namespace blink {
namespace {

using namespace Qt::StringLiterals;

QString defaultFolder(QStandardPaths::StandardLocation location)
{
    return QStandardPaths::writableLocation(location) + u"/Blink"_s;
}

std::string_view utf8(const QByteArray &bytes)
{
    return {bytes.constData(), static_cast<size_t>(bytes.size())};
}

// Every stored setting with its key, for load() and save() alike. Ungrouped keys are what QSettings files under
// [General]; a group of that name would be written as [%General].
template<typename S, typename Visit>
void forEachKey(S &s, Visit visit)
{
    visit(u"startAtLogin"_s, s.startAtLogin);
    visit(u"notifications"_s, s.notifications);

    visit(u"Screenshots/cursor"_s, s.captureCursor);
    visit(u"Screenshots/windowDecorations"_s, s.windowDecorations);
    visit(u"Screenshots/windowShadow"_s, s.windowShadow);
    visit(u"Screenshots/magnifier"_s, s.magnifier);
    visit(u"Screenshots/save"_s, s.saveScreenshots);
    visit(u"Screenshots/folder"_s, s.screenshotFolder);
    visit(u"Screenshots/subfolder"_s, s.screenshotSubfolder);

    visit(u"Recording/folder"_s, s.recordingFolder);
    visit(u"Recording/subfolder"_s, s.recordingSubfolder);
    visit(u"Recording/fps"_s, s.recording.fps);
    visit(u"Recording/quality"_s, s.recording.quality);
    visit(u"Recording/encoder"_s, s.recording.encoder);
    visit(u"Recording/cursor"_s, s.recording.cursor);
    visit(u"Recording/systemAudio"_s, s.recording.systemAudio);
    visit(u"Recording/microphone"_s, s.recording.microphone);

    visit(u"Upload/screenshots"_s, s.uploadScreenshots);
    visit(u"Upload/recordings"_s, s.uploadRecordings);
    visit(u"Upload/copyUrl"_s, s.copyUrl);
    visit(u"S3/endpoint"_s, s.s3.endpoint);
    visit(u"S3/region"_s, s.s3.region);
    visit(u"S3/bucket"_s, s.s3.bucket);
    visit(u"S3/accessKey"_s, s.s3.accessKey);
    visit(u"S3/secretKey"_s, s.s3.secretKey);
    visit(u"S3/path"_s, s.s3.path);
    visit(u"S3/publicUrl"_s, s.s3.publicUrl);
    visit(u"S3/pathStyle"_s, s.s3.pathStyle);
    visit(u"S3/publicRead"_s, s.s3.publicRead);
}

// A stored value into `value`, which keeps its default when the stored one is missing or unknown, so an older config
// file loads as before.
void read(const QVariant &stored, bool &value)
{
    value = stored.toBool();
}
void read(const QVariant &stored, int &value)
{
    value = stored.toInt();
}
void read(const QVariant &stored, QString &value)
{
    value = stored.toString();
}
void read(const QVariant &stored, Quality &value)
{
    value = recorder::qualityFromName(utf8(stored.toString().toLatin1())).value_or(value);
}
void read(const QVariant &stored, Encoder &value)
{
    value = recorder::encoderFromName(utf8(stored.toString().toLatin1())).value_or(value);
}

template<typename T>
QVariant stored(const T &value)
{
    return QVariant::fromValue(value);
}
QVariant stored(Quality quality)
{
    return latin1(recorder::name(quality));
}
QVariant stored(Encoder encoder)
{
    return latin1(recorder::name(encoder));
}

// A loaded save folder, normalized; a hand-edited config must not send captures to the working directory.
QString loadedFolder(const QString &folder, QStandardPaths::StandardLocation location)
{
    const QString normalized = Settings::normalizeFolder(folder, location);
    return QDir::isAbsolutePath(normalized) ? normalized : defaultFolder(location);
}

// Makes the file readable by its owner only, creating it if needed, before the secret key is written to it.
void createPrivate(const QString &path)
{
    if (QFileInfo::exists(path)) {
        // A file from an older Blink or copied in may be wider; narrowing it only after sync() would leave the
        // secret key readable meanwhile.
        QFile::setPermissions(path, QFile::ReadOwner | QFile::WriteOwner);
        return;
    }
    QDir().mkpath(QFileInfo(path).absolutePath());
    const int fd = ::open(QFile::encodeName(path).constData(), O_WRONLY | O_CREAT | O_CLOEXEC, 0600);
    if (fd >= 0)
        ::close(fd);
}

} // namespace

bool S3Config::isComplete() const
{
    return !endpoint.isEmpty() && !region.isEmpty() && !bucket.isEmpty() && !accessKey.isEmpty() && !secretKey.isEmpty();
}

QString Settings::normalizeFolder(const QString &text, QStandardPaths::StandardLocation fallback)
{
    QString folder = text.trimmed();
    if (folder.isEmpty())
        return defaultFolder(fallback);
    // Shell habits: "~" and "~/..." mean the home directory, which QDir would otherwise create literally.
    if (folder == u"~"_s)
        folder = QDir::homePath();
    else if (folder.startsWith(u"~/"_s))
        folder = QDir::homePath() + folder.sliced(1);
    folder = QDir::cleanPath(folder); // also drops a trailing slash, except for "/" itself
    return folder;
}

Settings Settings::load()
{
    const QSettings file;
    Settings s;
    forEachKey(s, [&file](const QString &key, auto &value) {
        if (const QVariant saved = file.value(key); saved.isValid())
            read(saved, value);
    });
    s.screenshotFolder = loadedFolder(s.screenshotFolder, kScreenshotLocation);
    s.recordingFolder = loadedFolder(s.recordingFolder, kRecordingLocation);
    s.recording.fps = std::clamp(s.recording.fps, recorder::kMinFps, recorder::kMaxFps);
    return s;
}

void Settings::save() const
{
    QSettings file;
    createPrivate(file.fileName());
    forEachKey(*this, [&file](const QString &key, const auto &value) { file.setValue(key, stored(value)); });
    file.sync();
    QFile::setPermissions(file.fileName(), QFile::ReadOwner | QFile::WriteOwner);
}

} // namespace blink
