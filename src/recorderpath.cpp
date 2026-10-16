#include "recorderpath.h"

#include <QCoreApplication>
#include <QFileInfo>

namespace blink {

using namespace Qt::StringLiterals;

QString recorderPath()
{
    if (QString local = QCoreApplication::applicationDirPath() + u"/blink-recorder"_s; QFileInfo(local).isExecutable())
        return local;
    return QStringLiteral(BLINK_RECORDER_PATH);
}

} // namespace blink
