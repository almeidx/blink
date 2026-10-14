#include "appname.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStandardPaths>

namespace blink {
namespace {

using namespace Qt::StringLiterals;

// Where KWin finds it (Window::findDesktopFile): an absolute path, or a file in the applications folders.
QString desktopFile(const QString &app)
{
    const QString file = app.endsWith(".desktop"_L1) ? app : app + ".desktop"_L1;
    if (QDir::isAbsolutePath(file))
        return QFileInfo::exists(file) ? file : QString();
    return QStandardPaths::locate(QStandardPaths::ApplicationsLocation, file);
}

} // namespace

QString appName(const QString &app)
{
    if (app.isEmpty())
        return {};
    const QString path = desktopFile(app);
    QFile file(path);
    if (path.isEmpty() || !file.open(QIODevice::ReadOnly))
        return app;
    bool entry = false; // in the [Desktop Entry] group
    while (!file.atEnd()) {
        const QByteArray line = file.readLine().trimmed();
        if (line.startsWith('[')) {
            if (entry)
                break;
            entry = line == "[Desktop Entry]";
            continue;
        }
        const qsizetype equals = line.indexOf('=');
        if (entry && equals > 0 && line.first(equals).trimmed() == "Name") {
            const QString name = QString::fromUtf8(line.sliced(equals + 1).trimmed());
            return name.isEmpty() ? app : name;
        }
    }
    return app;
}

} // namespace blink
