#include "controller.h"

#include "ipc/endpoint.h"

#include <QCommandLineParser>
#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QTextStream>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <string_view>

int main(int argc, char **argv)
{
    const auto hasArgument = [argc, argv](std::string_view value) {
        return std::any_of(argv + 1, argv + argc,
                           [value](const char *argument) { return value == argument; });
    };
    const bool launch = hasArgument("--demo") && !hasArgument("--help") && !hasArgument("-h") &&
                        !hasArgument("--help-all");
    std::unique_ptr<QCoreApplication> app;
    if (launch) {
        app = std::make_unique<QGuiApplication>(argc, argv);
    } else {
        app = std::make_unique<QCoreApplication>(argc, argv);
    }
    QCoreApplication::setApplicationName(QStringLiteral("xkeys"));
    QTextStream diagnostic(stderr);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Synthetic xkeys demo editor"));
    parser.addHelpOption();
    const QCommandLineOption demo(QStringLiteral("demo"),
                                  QStringLiteral("Open the synthetic demo."));
    const QCommandLineOption socket(QStringLiteral("socket"),
                                    QStringLiteral("Use a private absolute socket path."),
                                    QStringLiteral("path"));
    const QCommandLineOption profilesDir(QStringLiteral("profiles-dir"),
                                         QStringLiteral("Use an absolute demo profiles directory."),
                                         QStringLiteral("path"));
    parser.addOptions({demo, socket, profilesDir});
    parser.process(*app);

    if (!launch || !parser.isSet(demo) || !parser.positionalArguments().isEmpty()) {
        diagnostic << "xkeys requires --demo. Use --help for usage." << Qt::endl;
        return 2;
    }
    if (parser.isSet(socket) && parser.value(socket).isEmpty()) {
        diagnostic << "--socket requires a private absolute path." << Qt::endl;
        return 2;
    }

    if (parser.isSet(profilesDir)) {
        QString error;
        if (parser.value(profilesDir).isEmpty() ||
            xkeys::ipc::demoProfilesDir(parser.value(profilesDir), {}, error).isEmpty()) {
            diagnostic << "--profiles-dir requires an absolute path." << Qt::endl;
            return 2;
        }
    }

    QGuiApplication::setDesktopFileName(QStringLiteral("xkeys-demo"));

    xkeys::Controller controller;
    QQmlApplicationEngine engine;
    engine.setInitialProperties({{QStringLiteral("controller"), QVariant::fromValue(&controller)}});
    engine.loadFromModule(QStringLiteral("XKeys"), QStringLiteral("Main"));
    if (engine.rootObjects().isEmpty()) {
        return 1;
    }

    controller.startDemo(parser.value(socket), parser.value(profilesDir));
    return QCoreApplication::exec();
}
