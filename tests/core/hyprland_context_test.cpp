#include "ui/hyprland_context.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

#include <linux/input-event-codes.h>

namespace
{

const QString fakeName = QStringLiteral("Synthetic USB Keyboard");

QJsonObject keyboard(const QString &layout, int index, const QString &name = fakeName)
{
    return {{QStringLiteral("name"), name},
            {QStringLiteral("rules"), QString{}},
            {QStringLiteral("model"), QString{}},
            {QStringLiteral("layout"), layout},
            {QStringLiteral("variant"), QString{}},
            {QStringLiteral("options"), QString{}},
            {QStringLiteral("active_layout_index"), index}};
}

QJsonObject bind(const QString &key, int keycode, int mask, const QString &description,
                 bool release = false, const QString &submap = {})
{
    return {{QStringLiteral("key"), key},
            {QStringLiteral("keycode"), keycode},
            {QStringLiteral("modmask"), mask},
            {QStringLiteral("release"), release},
            {QStringLiteral("mouse"), false},
            {QStringLiteral("catch_all"), false},
            {QStringLiteral("submap"), submap},
            {QStringLiteral("description"), description},
            {QStringLiteral("dispatcher"), QStringLiteral("exec")},
            {QStringLiteral("arg"), QStringLiteral("raw arguments are private")}};
}

bool write(const QString &path, const QByteArray &bytes)
{
    QFile file(path);
    return file.open(QIODevice::WriteOnly) && file.write(bytes) == bytes.size();
}

QByteArray globalOptions(const QJsonObject &keyboard)
{
    QByteArray result;
    for (const auto &name :
         {QStringLiteral("layout"), QStringLiteral("rules"), QStringLiteral("model"),
          QStringLiteral("variant"), QStringLiteral("options"), QStringLiteral("file")}) {
        const bool file = name == QStringLiteral("file");
        const auto value = file ? QJsonValue(QStringLiteral("[[EMPTY]]")) : keyboard.value(name);
        const QJsonObject option{{QStringLiteral("option"), QStringLiteral("input:kb_") + name},
                                 {QStringLiteral("str"), value},
                                 {QStringLiteral("set"), !file}};
        result += QJsonDocument(option).toJson(QJsonDocument::Compact) + '\n';
    }
    return result;
}

QString fixture(const QTemporaryDir &dir, const QJsonArray &keyboards, const QByteArray &bindings,
                const QByteArray &global = {})
{
    const auto program = dir.filePath(QStringLiteral("hyprctl-fixture"));
    const auto devices = QJsonDocument(QJsonObject{{QStringLiteral("keyboards"), keyboards}});
    const auto options = global.isNull() ? globalOptions(keyboards.first().toObject()) : global;
    const QByteArray script = R"(#!/bin/sh
case "$1:$2" in
    -j:devices) cat "$(dirname "$0")/devices.json" ;;
    -j:binds) cat "$(dirname "$0")/binds.json" ;;
    --batch:*) cat "$(dirname "$0")/global.jsonl" ;;
    *) exit 2 ;;
esac
)";
    if (!write(dir.filePath(QStringLiteral("devices.json")), devices.toJson()) ||
        !write(dir.filePath(QStringLiteral("binds.json")), bindings) ||
        !write(dir.filePath(QStringLiteral("global.jsonl")), options) || !write(program, script) ||
        !QFile::setPermissions(program, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner)) {
        return {};
    }
    return program;
}

xkeys::KeyboardSelection selection(const QStringList &names = {fakeName})
{
    return {QStringLiteral("synthetic-usb"), fakeName, {}, {}, names, true, {}, {}};
}

} // namespace

class HyprlandContextTest : public QObject
{
    Q_OBJECT

  private slots:
    void formatsActiveLayoutGroup_data();
    void formatsActiveLayoutGroup();
    void rejectsAmbiguousAndInvalidLayout_data();
    void rejectsAmbiguousAndInvalidLayout();
    void checksPressReleasePrefixAndVirtualTerminal();
    void checksAltGrWithDeviceSpacing();
    void reportsUnavailableBindingInventory();
    void requiresAcknowledgementWhenTranslationUncertain_data();
    void requiresAcknowledgementWhenTranslationUncertain();
    void rejectsCustomGlobalKeymap();
};

void HyprlandContextTest::formatsActiveLayoutGroup_data()
{
    QTest::addColumn<QString>("layout");
    QTest::addColumn<int>("index");
    QTest::addColumn<QString>("label");
    QTest::newRow("US") << QStringLiteral("us") << 0 << QStringLiteral("Right Ctrl+Y+Z");
    QTest::newRow("German active group")
        << QStringLiteral("us,de") << 1 << QStringLiteral("Right Ctrl+Z+Y");
}

void HyprlandContextTest::formatsActiveLayoutGroup()
{
    QFETCH(QString, layout);
    QFETCH(int, index);
    QFETCH(QString, label);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = fixture(
        dir, {keyboard(layout, index, QStringLiteral("synthetic-usb-keyboard"))}, QByteArray("[]"));
    QVERIFY(!path.isEmpty());
    const auto context = xkeys::makeShortcutContext(path);
    QSignalSpy prepared(context.get(), &xkeys::ShortcutContext::prepared);
    context->refresh(selection());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 2000);
    QVERIFY2(prepared.first().at(0).toBool(), qPrintable(prepared.first().at(1).toString()));
    QCOMPARE(context->label({KEY_RIGHTCTRL, KEY_Y, KEY_Z}), label);
    QCOMPARE(context->label({KEY_LEFTALT, KEY_ESC, KEY_F9}), QStringLiteral("Alt+Esc+F9"));
    QCOMPARE(context->label({KEY_LEFTMETA, KEY_RIGHTSHIFT, KEY_SPACE}),
             QStringLiteral("Super+Right Shift+Space"));
    QVERIFY(context->label({KEY_UNKNOWN}).isEmpty());
    QCOMPARE(context->bindingsAvailable(), index == 0);
    if (index != 0) {
        QVERIFY(context->advisory().contains(QStringLiteral("different layout")));
    }
}

void HyprlandContextTest::rejectsAmbiguousAndInvalidLayout_data()
{
    QTest::addColumn<QJsonArray>("keyboards");
    QTest::addColumn<QStringList>("names");
    QTest::newRow("out-of-range group")
        << QJsonArray{keyboard(QStringLiteral("us"), 2)} << QStringList{fakeName};
    QTest::newRow("missing typing device")
        << QJsonArray{keyboard(QStringLiteral("us"), 0)} << QStringList{QStringLiteral("Other")};
    QTest::newRow("numbered duplicate")
        << QJsonArray{keyboard(QStringLiteral("us"), 0),
                      keyboard(QStringLiteral("us"), 0, QStringLiteral("synthetic-usb-keyboard-1"))}
        << QStringList{fakeName};
    QTest::newRow("conflicting typing layouts")
        << QJsonArray{keyboard(QStringLiteral("us"), 0),
                      keyboard(QStringLiteral("de"), 0, QStringLiteral("Other"))}
        << QStringList{fakeName, QStringLiteral("Other")};
    auto remapped = keyboard(QStringLiteral("us"), 0);
    remapped.insert(QStringLiteral("options"), QStringLiteral("ctrl:swapcaps"));
    QTest::newRow("remapped physical modifier") << QJsonArray{remapped} << QStringList{fakeName};
    auto malformed = keyboard(QStringLiteral("us"), 0);
    malformed.remove(QStringLiteral("options"));
    QTest::newRow("missing layout metadata") << QJsonArray{malformed} << QStringList{fakeName};
}

void HyprlandContextTest::rejectsAmbiguousAndInvalidLayout()
{
    QFETCH(QJsonArray, keyboards);
    QFETCH(QStringList, names);
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = fixture(dir, keyboards, QByteArray("[]"));
    QVERIFY(!path.isEmpty());
    const auto context = xkeys::makeShortcutContext(path);
    QSignalSpy prepared(context.get(), &xkeys::ShortcutContext::prepared);
    context->refresh(selection(names));
    QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 2000);
    QVERIFY(!prepared.first().at(0).toBool());
    QVERIFY(!prepared.first().at(1).toString().isEmpty());
    QVERIFY(context->label({KEY_F9}).isEmpty());
}

void HyprlandContextTest::checksPressReleasePrefixAndVirtualTerminal()
{
    const QJsonArray bindings{
        bind(QStringLiteral("j"), 0, 65, QStringLiteral("Prefix match")),
        bind({}, KEY_J + 8, 65, QStringLiteral("Physical release"), true,
             QStringLiteral("synthetic-submap")),
        bind({}, 0, 64, QStringLiteral("Synthetic workspace 1")),
        bind({}, 0, 64, QStringLiteral("Synthetic workspace 2")),
        bind({}, 0, 65, QStringLiteral("Synthetic menu")),
        bind({}, 0, 65, QStringLiteral("Synthetic release action"), true),
        bind(QStringLiteral("Alt_R"), 0, 8, QStringLiteral("Right Alt release"), true),
        bind(QStringLiteral("j"), 0, 0, QStringLiteral("Different modifier")),
    };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path =
        fixture(dir, {keyboard(QStringLiteral("us"), 0)}, QJsonDocument(bindings).toJson());
    QVERIFY(!path.isEmpty());
    const auto context = xkeys::makeShortcutContext(path);
    QSignalSpy prepared(context.get(), &xkeys::ShortcutContext::prepared);
    context->refresh(selection());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 2000);
    QVERIFY(prepared.first().at(0).toBool());
    QVERIFY(context->bindingsAvailable());

    const auto overlaps = context->conflicts({KEY_LEFTMETA, KEY_LEFTSHIFT, KEY_J, KEY_W});
    QVERIFY(overlaps.contains(QStringLiteral("Prefix match")));
    QVERIFY(overlaps.contains(
        QStringLiteral("Physical release (submap synthetic-submap) [on release]")));
    QCOMPARE(overlaps.size(), 3);
    QVERIFY(overlaps.contains(QStringLiteral(
        "Hyprland reports shortcuts with these modifiers but omits their triggers.")));
    QCOMPARE(context->conflicts({KEY_LEFTMETA, KEY_LEFTSHIFT, KEY_J}), overlaps);
    QVERIFY(context->conflicts({KEY_LEFTCTRL, KEY_LEFTALT, KEY_F}).isEmpty());
    QVERIFY(context->advisory().contains(QStringLiteral("omits some shortcut triggers")));
    QVERIFY(!overlaps.contains(QStringLiteral("Different modifier")));
    QVERIFY(!overlaps.join(u' ').contains(QStringLiteral("raw arguments")));
    QVERIFY(context->conflicts({KEY_RIGHTALT})
                .contains(QStringLiteral("Right Alt release [on release]")));
    QVERIFY(context->conflicts({KEY_LEFTCTRL, KEY_LEFTALT, KEY_F3})
                .join(u' ')
                .contains("virtual-terminal"));
}

void HyprlandContextTest::checksAltGrWithDeviceSpacing()
{
    const QJsonArray bindings{
        bind(QStringLiteral("ISO_Level3_Shift"), 0, 128, QStringLiteral("AltGr overlap"), true),
        bind(QStringLiteral("ISO_Level3_Shift"), 0, 8, QStringLiteral("Alt-only binding"), true),
    };
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = fixture(
        dir, {keyboard(QStringLiteral("de"), 0, QStringLiteral("synthetic--usb--keyboard"))},
        QJsonDocument(bindings).toJson());
    QVERIFY(!path.isEmpty());
    const auto context = xkeys::makeShortcutContext(path);
    QSignalSpy prepared(context.get(), &xkeys::ShortcutContext::prepared);
    context->refresh(selection({QStringLiteral("Synthetic  USB  Keyboard")}));
    QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 2000);
    QVERIFY2(prepared.first().at(0).toBool(), qPrintable(prepared.first().at(1).toString()));
    QCOMPARE(context->label({KEY_RIGHTALT, KEY_Y}), QStringLiteral("Right Alt (AltGr)+Z"));
    QCOMPARE(context->conflicts({KEY_RIGHTALT}),
             QStringList{QStringLiteral("AltGr overlap [on release]")});
}

void HyprlandContextTest::reportsUnavailableBindingInventory()
{
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = fixture(dir, {keyboard(QStringLiteral("us"), 0)}, QByteArray("invalid-json"));
    QVERIFY(!path.isEmpty());
    const auto context = xkeys::makeShortcutContext(path);
    QSignalSpy prepared(context.get(), &xkeys::ShortcutContext::prepared);
    context->refresh(selection());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 2000);
    QVERIFY(prepared.first().at(0).toBool());
    QVERIFY(!context->bindingsAvailable());
    QVERIFY(context->advisory().contains(QStringLiteral("Could not check")));
    QCOMPARE(context->label({KEY_F9}), QStringLiteral("F9"));
}

void HyprlandContextTest::requiresAcknowledgementWhenTranslationUncertain_data()
{
    QTest::addColumn<QByteArray>("global");
    QTest::newRow("different global layout") << globalOptions(keyboard(QStringLiteral("us"), 0));
    QTest::newRow("missing global metadata") << QByteArray("invalid");
}

void HyprlandContextTest::requiresAcknowledgementWhenTranslationUncertain()
{
    QFETCH(QByteArray, global);
    const auto keyboardLayout = keyboard(QStringLiteral("de"), 0);
    const QJsonArray bindings{
        bind(QStringLiteral("z"), 0, 0, QStringLiteral("Selected layout match"))};
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = fixture(dir, {keyboardLayout}, QJsonDocument(bindings).toJson(), global);
    QVERIFY(!path.isEmpty());
    const auto context = xkeys::makeShortcutContext(path);
    QSignalSpy prepared(context.get(), &xkeys::ShortcutContext::prepared);
    context->refresh(selection());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 2000);
    QVERIFY(prepared.first().at(0).toBool());
    QCOMPARE(context->label({KEY_Y}), QStringLiteral("Z"));
    QVERIFY(!context->bindingsAvailable());
    QVERIFY(context->advisory().contains(QStringLiteral("different layout")));
    QVERIFY(context->conflicts({KEY_Y}).contains(QStringLiteral("Selected layout match")));
}

void HyprlandContextTest::rejectsCustomGlobalKeymap()
{
    const auto keyboardLayout = keyboard(QStringLiteral("de"), 0);
    const auto global = globalOptions(keyboardLayout).replace("[[EMPTY]]", "/synthetic.xkb");
    const QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const auto path = fixture(dir, {keyboardLayout}, QByteArray("[]"), global);
    QVERIFY(!path.isEmpty());
    const auto context = xkeys::makeShortcutContext(path);
    QSignalSpy prepared(context.get(), &xkeys::ShortcutContext::prepared);
    context->refresh(selection());
    QTRY_COMPARE_WITH_TIMEOUT(prepared.count(), 1, 2000);
    QVERIFY(!prepared.first().at(0).toBool());
    QVERIFY(prepared.first().at(1).toString().contains(QStringLiteral("custom Hyprland keymap")));
    QVERIFY(context->label({KEY_F9}).isEmpty());
}

QTEST_GUILESS_MAIN(HyprlandContextTest)
#include "hyprland_context_test.moc"
