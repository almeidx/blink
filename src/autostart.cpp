#include "autostart.h"

#include "logging.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QGuiApplication>
#include <QSaveFile>
#include <QStandardPaths>

#include <algorithm>

namespace blink {
namespace {

using namespace Qt::StringLiterals;

// An Exec value per the Desktop Entry spec: quoted when needed, then escaped again as a string value.
QString desktopExec(const QString &program)
{
    static const QString reserved = u" \t\n\"'\\><~|&;$*?#()`"_s;
    if (std::ranges::none_of(program, [](QChar c) { return reserved.contains(c); }))
        return program;
    QString quoted = u"\""_s;
    for (const QChar c : program) {
        if (c == u'\\')
            quoted += u"\\\\\\\\"_s;
        else if (c == u'"' || c == u'`' || c == u'$')
            quoted += u"\\\\"_s + c;
        else
            quoted += c;
    }
    return quoted + u'"';
}

} // namespace

QString applyAutostart(bool enabled)
{
    // Only the installed binary manages the entry. From a build tree, writing it would make that tree start at every
    // login, and removing it would undo the installed Blink's setting.
    if (const QString self = QCoreApplication::applicationFilePath(); self != QStringLiteral(BLINK_INSTALLED_PATH)) {
        qCDebug(lcBlink) << "autostart entry left alone:" << self << "is not" << BLINK_INSTALLED_PATH;
        return {};
    }
    const QString desktopFile = QGuiApplication::desktopFileName();
    const QString path = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + u"/autostart/"_s + desktopFile + u".desktop"_s;
    if (!enabled) {
        QFile::remove(path);
        return {};
    }
    if (QFileInfo::exists(path))
        return {};
    const QByteArray entry = "[Desktop Entry]\nType=Application\nName=" + QGuiApplication::applicationDisplayName().toUtf8()
                           + "\nComment=Screenshots and screen recordings\nIcon=" + desktopFile.toUtf8()
                           + "\nExec=" + desktopExec(QCoreApplication::applicationFilePath()).toUtf8() + '\n';
    QDir().mkpath(QFileInfo(path).absolutePath());
    QSaveFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(entry) != entry.size() || !file.commit())
        return file.errorString();
    return {};
}

} // namespace blink
