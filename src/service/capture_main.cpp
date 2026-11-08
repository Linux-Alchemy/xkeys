#include "linux/keyboard_capture.h"
#include "linux/keyboard_inventory.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTextStream>

#include <algorithm>
#include <cstdio>

namespace
{

int listKeyboards(QTextStream &diagnostic)
{
    const auto result = xkeys::discoverKeyboards();
    if (!result) {
        diagnostic << result.error().message << Qt::endl;
        return 1;
    }

    QJsonArray keyboards;
    for (const auto &keyboard : result.value()) {
        keyboards.append(QJsonObject{
            {QStringLiteral("id"), keyboard.id},
            {QStringLiteral("name"), keyboard.name},
            {QStringLiteral("nodes"), QJsonArray::fromStringList(keyboard.nodes)},
            {QStringLiteral("interfaceNames"), QJsonArray::fromStringList(keyboard.interfaceNames)},
            {QStringLiteral("typingNames"), QJsonArray::fromStringList(keyboard.typingNames)},
            {QStringLiteral("accessible"), keyboard.accessible},
            {QStringLiteral("reason"), keyboard.reason},
            {QStringLiteral("fingerprint"), keyboard.fingerprint},
        });
    }
    auto bytes = QJsonDocument(QJsonObject{{QStringLiteral("keyboards"), keyboards}})
                     .toJson(QJsonDocument::Compact);
    bytes.append('\n');
    if (bytes.size() > 256 * 1024) {
        diagnostic << "The keyboard inventory exceeds its response limit." << Qt::endl;
        return 1;
    }
    const auto size = static_cast<std::size_t>(bytes.size());
    return std::fwrite(bytes.constData(), 1, size, stdout) == size && std::fflush(stdout) == 0 ? 0
                                                                                               : 1;
}

} // namespace

int main(int argc, char **argv)
{
    QElapsedTimer started;
    started.start();
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("xkeys-record"));
    QTextStream diagnostic(stderr);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("One-shot xkeys keyboard recorder"));
    parser.addHelpOption();
    const QCommandLineOption list(QStringLiteral("list"),
                                  QStringLiteral("List USB keyboard metadata."));
    const QCommandLineOption capture(QStringLiteral("capture"),
                                     QStringLiteral("Capture a single chord over editor pipes."));
    const QCommandLineOption keyboard(QStringLiteral("keyboard"),
                                      QStringLiteral("Select a keyboard ID from the inventory."),
                                      QStringLiteral("id"));
    const QCommandLineOption fingerprint(
        QStringLiteral("fingerprint"), QStringLiteral("Match the selected inventory fingerprint."),
        QStringLiteral("sha256"));
    parser.addOptions({list, capture, keyboard, fingerprint});
    parser.process(application);

    if (parser.positionalArguments().isEmpty()) {
        if (parser.isSet(list) && !parser.isSet(capture) && !parser.isSet(keyboard) &&
            !parser.isSet(fingerprint)) {
            return listKeyboards(diagnostic);
        }
        const QString hash = parser.value(fingerprint);
        const bool validHash = hash.size() == 64 && std::ranges::all_of(hash, [](QChar ch) {
                                   return (ch >= u'0' && ch <= u'9') || (ch >= u'a' && ch <= u'f');
                               });
        if (parser.isSet(capture) && !parser.isSet(list) && !parser.value(keyboard).isEmpty() &&
            validHash) {
            return xkeys::keyboard::captureKeyboard(parser.value(keyboard), hash,
                                                    started.msecsSinceReference());
        }
    }

    diagnostic << "Use --list or --capture --keyboard ID --fingerprint SHA256. See --help."
               << Qt::endl;
    return 2;
}
