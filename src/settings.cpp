#include "settings.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

#include <algorithm>

#include <fcntl.h>
#include <unistd.h>

namespace blink {
namespace {

using namespace Qt::StringLiterals;

QString defaultFolder(QStandardPaths::StandardLocation location)
{
    return QStandardPaths::writableLocation(location) + u"/Blink"_s;
}

QLatin1StringView qualityName(Quality quality)
{
    switch (quality) {
    case Quality::Low:
        return "low"_L1;
    case Quality::Medium:
        return "medium"_L1;
    case Quality::High:
        break;
    }
    return "high"_L1;
}

Quality qualityFromName(const QString &name)
{
    if (name == "low"_L1)
        return Quality::Low;
    if (name == "medium"_L1)
        return Quality::Medium;
    return Quality::High;
}

QLatin1StringView encoderName(EncoderPreference encoder)
{
    switch (encoder) {
    case EncoderPreference::Nvenc:
        return "nvenc"_L1;
    case EncoderPreference::Vaapi:
        return "vaapi"_L1;
    case EncoderPreference::Software:
        return "software"_L1;
    case EncoderPreference::Auto:
        break;
    }
    return "auto"_L1;
}

EncoderPreference encoderFromName(const QString &name)
{
    if (name == "nvenc"_L1)
        return EncoderPreference::Nvenc;
    if (name == "vaapi"_L1)
        return EncoderPreference::Vaapi;
    if (name == "software"_L1)
        return EncoderPreference::Software;
    return EncoderPreference::Auto;
}

// Creates the file readable by its owner only, so the secret key is never on disk with wider permissions.
void createPrivate(const QString &path)
{
    if (QFileInfo::exists(path))
        return;
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
    const QSettings settings;
    Settings s;
    // Ungrouped keys are what QSettings files under [General]; a group of that name would be written as [%General].
    s.startAtLogin = settings.value(u"startAtLogin"_s, s.startAtLogin).toBool();
    s.notifications = settings.value(u"notifications"_s, s.notifications).toBool();

    s.captureCursor = settings.value(u"Screenshots/cursor"_s, s.captureCursor).toBool();
    s.windowDecorations = settings.value(u"Screenshots/windowDecorations"_s, s.windowDecorations).toBool();
    s.windowShadow = settings.value(u"Screenshots/windowShadow"_s, s.windowShadow).toBool();
    s.magnifier = settings.value(u"Screenshots/magnifier"_s, s.magnifier).toBool();
    s.saveScreenshots = settings.value(u"Screenshots/save"_s, s.saveScreenshots).toBool();
    s.screenshotFolder = normalizeFolder(settings.value(u"Screenshots/folder"_s).toString(), QStandardPaths::PicturesLocation);
    if (!QDir::isAbsolutePath(s.screenshotFolder)) // a hand-edited config must not send captures to the working directory
        s.screenshotFolder = defaultFolder(QStandardPaths::PicturesLocation);
    s.screenshotSubfolder = settings.value(u"Screenshots/subfolder"_s, s.screenshotSubfolder).toString();

    s.recordingFolder = normalizeFolder(settings.value(u"Recording/folder"_s).toString(), QStandardPaths::MoviesLocation);
    if (!QDir::isAbsolutePath(s.recordingFolder))
        s.recordingFolder = defaultFolder(QStandardPaths::MoviesLocation);
    s.recordingSubfolder = settings.value(u"Recording/subfolder"_s, s.recordingSubfolder).toString();
    s.fps = std::clamp(settings.value(u"Recording/fps"_s, s.fps).toInt(), 1, 240);
    s.quality = qualityFromName(settings.value(u"Recording/quality"_s).toString());
    s.encoder = encoderFromName(settings.value(u"Recording/encoder"_s).toString());
    s.recordCursor = settings.value(u"Recording/cursor"_s, s.recordCursor).toBool();
    s.systemAudio = settings.value(u"Recording/systemAudio"_s, s.systemAudio).toBool();
    s.microphone = settings.value(u"Recording/microphone"_s, s.microphone).toBool();

    s.uploadScreenshots = settings.value(u"Upload/screenshots"_s, s.uploadScreenshots).toBool();
    s.uploadRecordings = settings.value(u"Upload/recordings"_s, s.uploadRecordings).toBool();
    s.copyUrl = settings.value(u"Upload/copyUrl"_s, s.copyUrl).toBool();
    s.s3.endpoint = settings.value(u"S3/endpoint"_s).toString();
    s.s3.region = settings.value(u"S3/region"_s).toString();
    s.s3.bucket = settings.value(u"S3/bucket"_s).toString();
    s.s3.accessKey = settings.value(u"S3/accessKey"_s).toString();
    s.s3.secretKey = settings.value(u"S3/secretKey"_s).toString();
    s.s3.path = settings.value(u"S3/path"_s, s.s3.path).toString();
    s.s3.publicUrl = settings.value(u"S3/publicUrl"_s).toString();
    s.s3.pathStyle = settings.value(u"S3/pathStyle"_s, s.s3.pathStyle).toBool();
    s.s3.publicRead = settings.value(u"S3/publicRead"_s, s.s3.publicRead).toBool();
    return s;
}

void Settings::save() const
{
    QSettings settings;
    createPrivate(settings.fileName());

    settings.setValue(u"startAtLogin"_s, startAtLogin);
    settings.setValue(u"notifications"_s, notifications);

    settings.setValue(u"Screenshots/cursor"_s, captureCursor);
    settings.setValue(u"Screenshots/windowDecorations"_s, windowDecorations);
    settings.setValue(u"Screenshots/windowShadow"_s, windowShadow);
    settings.setValue(u"Screenshots/magnifier"_s, magnifier);
    settings.setValue(u"Screenshots/save"_s, saveScreenshots);
    settings.setValue(u"Screenshots/folder"_s, screenshotFolder);
    settings.setValue(u"Screenshots/subfolder"_s, screenshotSubfolder);

    settings.setValue(u"Recording/folder"_s, recordingFolder);
    settings.setValue(u"Recording/subfolder"_s, recordingSubfolder);
    settings.setValue(u"Recording/fps"_s, fps);
    settings.setValue(u"Recording/quality"_s, qualityName(quality));
    settings.setValue(u"Recording/encoder"_s, encoderName(encoder));
    settings.setValue(u"Recording/cursor"_s, recordCursor);
    settings.setValue(u"Recording/systemAudio"_s, systemAudio);
    settings.setValue(u"Recording/microphone"_s, microphone);

    settings.setValue(u"Upload/screenshots"_s, uploadScreenshots);
    settings.setValue(u"Upload/recordings"_s, uploadRecordings);
    settings.setValue(u"Upload/copyUrl"_s, copyUrl);
    settings.setValue(u"S3/endpoint"_s, s3.endpoint);
    settings.setValue(u"S3/region"_s, s3.region);
    settings.setValue(u"S3/bucket"_s, s3.bucket);
    settings.setValue(u"S3/accessKey"_s, s3.accessKey);
    settings.setValue(u"S3/secretKey"_s, s3.secretKey);
    settings.setValue(u"S3/path"_s, s3.path);
    settings.setValue(u"S3/publicUrl"_s, s3.publicUrl);
    settings.setValue(u"S3/pathStyle"_s, s3.pathStyle);
    settings.setValue(u"S3/publicRead"_s, s3.publicRead);

    settings.sync();
    QFile::setPermissions(settings.fileName(), QFile::ReadOwner | QFile::WriteOwner);
}

} // namespace blink
