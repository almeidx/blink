#include "recorderclient.h"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QProcess>

namespace blink {
namespace {

using namespace Qt::StringLiterals;

constexpr int kThumbnailTimeoutMs = 10'000;

} // namespace

QString recorderPath()
{
    if (QString local = QCoreApplication::applicationDirPath() + u"/blink-recorder"_s; QFileInfo(local).isExecutable())
        return local;
    return QStringLiteral(BLINK_RECORDER_PATH);
}

QImage videoThumbnail(const QString &path, int size, const std::atomic_bool &stopping)
{
    QProcess process;
    process.setStandardErrorFile(QProcess::nullDevice());
    process.start(recorderPath(), {latin1(recorder::kOptThumbnail), QString::number(size), path}, QIODevice::ReadOnly);
    QElapsedTimer timer;
    timer.start();
    while (!process.waitForFinished(100)) {
        if (process.state() == QProcess::NotRunning)
            return {};
        if (stopping || timer.hasExpired(kThumbnailTimeoutMs)) {
            process.kill();
            process.waitForFinished();
            return {};
        }
    }
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
        return {};
    return QImage::fromData(process.readAllStandardOutput(), "PPM");
}

} // namespace blink
