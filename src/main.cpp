#include "app.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QIcon>

#include <array>
#include <cstdio>

#include <malloc.h>

int main(int argc, char *argv[])
{
    using namespace Qt::StringLiterals;

    // Captures allocate tens of MB at a time (33 MB per 4K screen, more for the selection's backdrop). glibc
    // raises its mmap threshold after the first such buffer is freed, and later ones then stay in the heap once
    // freed: an idle tray app would keep the memory of its biggest capture. A fixed threshold keeps them mmapped.
    mallopt(M_MMAP_THRESHOLD, 1 << 20);

    QApplication app(argc, argv);
    QApplication::setApplicationName(u"blink"_s);
    QApplication::setApplicationDisplayName(u"Blink"_s);
    QApplication::setOrganizationName(u"blink"_s);
    QApplication::setApplicationVersion(QStringLiteral(BLINK_VERSION));
    QGuiApplication::setDesktopFileName(u"blink"_s);
    QApplication::setWindowIcon(QIcon::fromTheme(u"blink"_s, QIcon(u":/icons/hicolor/scalable/apps/blink.svg"_s)));
    QApplication::setQuitOnLastWindowClosed(false); // lives in the tray

    QCommandLineParser parser;
    parser.setApplicationDescription(QCoreApplication::translate("main", "Screenshots and screen recordings for KDE Plasma."));
    parser.addHelpOption();
    parser.addVersionOption();
    struct Command {
        QCommandLineOption option;
        QLatin1StringView method;
    };
    const std::array<Command, 5> commands{{
        {QCommandLineOption(u"region"_s, QCoreApplication::translate("main", "Select a region to capture.")), "captureRegion"_L1},
        {QCommandLineOption(u"window"_s, QCoreApplication::translate("main", "Capture the active window.")), "captureWindow"_L1},
        {QCommandLineOption(u"fullscreen"_s, QCoreApplication::translate("main", "Capture all screens.")), "captureFullscreen"_L1},
        {QCommandLineOption(u"record"_s, QCoreApplication::translate("main", "Select a region to record, or stop recording.")),
         "toggleRecording"_L1},
        {QCommandLineOption(u"settings"_s, QCoreApplication::translate("main", "Open the settings.")), "showSettings"_L1},
    }};
    for (const Command &command : commands)
        parser.addOption(command.option);
    parser.process(app);

    QLatin1StringView method;
    for (const Command &command : commands) {
        if (parser.isSet(command.option))
            method = command.method;
    }

    const QString service = u"io.github.almeidx.Blink"_s;
    QDBusConnection bus = QDBusConnection::sessionBus();
    if (!bus.isConnected()) {
        std::fputs("blink: no D-Bus session bus\n", stderr);
        return 1;
    }
    if (!bus.registerService(service)) {
        // Already running: hand the request over. Starting Blink again from the menu opens its settings.
        const QDBusMessage call = QDBusMessage::createMethodCall(service, u"/Blink"_s, service,
                                                                 method.isEmpty() ? u"showSettings"_s : QString(method));
        return bus.call(call).type() == QDBusMessage::ErrorMessage ? 1 : 0;
    }

    blink::App blink;
    if (!method.isEmpty())
        QMetaObject::invokeMethod(&blink, method.data(), Qt::QueuedConnection);
    return QApplication::exec();
}
