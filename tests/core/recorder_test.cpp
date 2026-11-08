#include "service/shortcut_recorder.h"

#include <QtTest>

#include <linux/input-event-codes.h>

namespace
{
using namespace xkeys;

KeyboardSelection selection()
{
    KeyboardSelection keyboard;
    keyboard.id = QStringLiteral("synthetic");
    keyboard.nodes = {QStringLiteral("keyboard-a"), QStringLiteral("keyboard-b")};
    return keyboard;
}
} // namespace

class RecorderTest : public QObject
{
    Q_OBJECT

  private slots:
    void capturesChord_data();
    void capturesChord();
    void deduplicatesRepeatAndOverlappingSources();
    void rejectsInvalidOrSequentialInput_data();
    void rejectsInvalidOrSequentialInput();
    void cancelsAndStartsAgain();
    void rejectsIncompleteSelection();
};

void RecorderTest::capturesChord_data()
{
    QTest::addColumn<QList<int>>("pressOrder");
    QTest::addColumn<QList<int>>("expected");
    QTest::newRow("Ctrl Alt F") << QList<int>{KEY_LEFTCTRL, KEY_LEFTALT, KEY_F}
                                << QList<int>{KEY_LEFTCTRL, KEY_LEFTALT, KEY_F};
    QTest::newRow("desktop shortcut") << QList<int>{KEY_LEFTMETA, KEY_RIGHTSHIFT, KEY_J}
                                      << QList<int>{KEY_LEFTMETA, KEY_RIGHTSHIFT, KEY_J};
    QTest::newRow("VT-shaped shortcut") << QList<int>{KEY_RIGHTCTRL, KEY_RIGHTALT, KEY_F4}
                                        << QList<int>{KEY_RIGHTCTRL, KEY_RIGHTALT, KEY_F4};
    QTest::newRow("Esc") << QList<int>{KEY_ESC} << QList<int>{KEY_ESC};
    QTest::newRow("bare key") << QList<int>{KEY_B} << QList<int>{KEY_B};
    QTest::newRow("modifier only") << QList<int>{KEY_RIGHTSHIFT} << QList<int>{KEY_RIGHTSHIFT};
    QTest::newRow("modifiers moved before ordinary keys")
        << QList<int>{KEY_E, KEY_RIGHTCTRL, KEY_LEFTCTRL, KEY_F, KEY_LEFTALT}
        << QList<int>{KEY_RIGHTCTRL, KEY_LEFTCTRL, KEY_LEFTALT, KEY_E, KEY_F};
}

void RecorderTest::capturesChord()
{
    QFETCH(QList<int>, pressOrder);
    QFETCH(QList<int>, expected);
    Recorder recorder;
    QVERIFY(recorder.begin(selection()));
    for (const int code : pressOrder) {
        QCOMPARE(recorder.consume({0, code, 1}).phase, CapturePhase::Recording);
    }
    QCOMPARE(recorder.snapshot().chord, expected);
    for (qsizetype index = 0; index < pressOrder.size(); ++index) {
        const auto update = recorder.consume({0, pressOrder.at(index), 0});
        QCOMPARE(update.phase,
                 index == pressOrder.size() - 1 ? CapturePhase::Complete : CapturePhase::Recording);
        QCOMPARE(update.chord, expected);
    }
    QCOMPARE(recorder.consume({0, KEY_SPACE, 1}).chord, expected);
    QCOMPARE(recorder.snapshot().phase, CapturePhase::Complete);
}

void RecorderTest::deduplicatesRepeatAndOverlappingSources()
{
    Recorder recorder;
    QVERIFY(recorder.begin(selection()));
    QCOMPARE(recorder.consume({0, KEY_LEFTSHIFT, 1}).chord, QList<int>{KEY_LEFTSHIFT});
    QCOMPARE(recorder.consume({0, KEY_LEFTSHIFT, 2}).chord, QList<int>{KEY_LEFTSHIFT});
    QCOMPARE(recorder.consume({0, KEY_LEFTSHIFT, 1}).chord, QList<int>{KEY_LEFTSHIFT});
    QCOMPARE(recorder.consume({1, KEY_LEFTSHIFT, 1}).chord, QList<int>{KEY_LEFTSHIFT});
    QCOMPARE(recorder.consume({0, KEY_LEFTSHIFT, 0}).phase, CapturePhase::Recording);
    QCOMPARE(recorder.consume({1, KEY_LEFTSHIFT, 1}).phase, CapturePhase::Recording);
    const auto complete = recorder.consume({1, KEY_LEFTSHIFT, 0});
    QCOMPARE(complete.phase, CapturePhase::Complete);
    QCOMPARE(complete.chord, QList<int>{KEY_LEFTSHIFT});
}

void RecorderTest::rejectsInvalidOrSequentialInput_data()
{
    QTest::addColumn<int>("source");
    QTest::addColumn<int>("code");
    QTest::addColumn<int>("value");
    QTest::addColumn<bool>("releasing");
    QTest::newRow("unknown source") << 2 << KEY_E << 1 << false;
    QTest::newRow("negative source") << -1 << KEY_E << 1 << false;
    QTest::newRow("orphan release") << 1 << KEY_E << 0 << false;
    QTest::newRow("wrong key value") << 0 << KEY_E << 3 << false;
    QTest::newRow("reserved code") << 0 << KEY_RESERVED << 1 << false;
    QTest::newRow("unknown code") << 0 << KEY_UNKNOWN << 1 << false;
    QTest::newRow("mouse code") << 0 << BTN_LEFT << 1 << false;
    QTest::newRow("sequence after release") << 0 << KEY_W << 1 << true;
}

void RecorderTest::rejectsInvalidOrSequentialInput()
{
    QFETCH(int, source);
    QFETCH(int, code);
    QFETCH(int, value);
    QFETCH(bool, releasing);
    Recorder recorder;
    QVERIFY(recorder.begin(selection()));
    recorder.consume({0, KEY_LEFTCTRL, 1});
    if (releasing) {
        recorder.consume({0, KEY_E, 1});
        recorder.consume({0, KEY_E, 0});
    }
    const auto rejected = recorder.consume({source, code, value});
    QCOMPARE(rejected.phase, CapturePhase::Rejected);
    QVERIFY(rejected.chord.isEmpty());
    QVERIFY(!rejected.message.isEmpty());
    QCOMPARE(recorder.consume({0, KEY_F, 1}).phase, CapturePhase::Rejected);
}

void RecorderTest::cancelsAndStartsAgain()
{
    Recorder recorder;
    QVERIFY(recorder.begin(selection()));
    for (int code = 1; code <= 16; ++code) {
        QCOMPARE(recorder.consume({0, code, 1}).phase, CapturePhase::Recording);
    }
    QCOMPARE(recorder.consume({0, 17, 1}).phase, CapturePhase::Rejected);
    QVERIFY(recorder.snapshot().chord.isEmpty());

    QVERIFY(recorder.begin(selection()));
    recorder.consume({1, KEY_RIGHTALT, 1});
    recorder.cancel();
    QCOMPARE(recorder.snapshot().phase, CapturePhase::Cancelled);
    QVERIFY(recorder.snapshot().chord.isEmpty());
    QCOMPARE(recorder.consume({1, KEY_RIGHTALT, 0}).phase, CapturePhase::Cancelled);

    QVERIFY(recorder.begin(selection()));
    QCOMPARE(recorder.consume({0, KEY_ENTER, 1}).chord, QList<int>{KEY_ENTER});
    QCOMPARE(recorder.consume({0, KEY_ENTER, 0}).phase, CapturePhase::Complete);
}

void RecorderTest::rejectsIncompleteSelection()
{
    Recorder recorder;
    auto keyboard = selection();
    keyboard.id.clear();
    QVERIFY(!recorder.begin(keyboard));
    keyboard = selection();
    keyboard.nodes.append(keyboard.nodes.first());
    QVERIFY(!recorder.begin(keyboard));
    keyboard = selection();
    keyboard.nodes.clear();
    QVERIFY(!recorder.begin(keyboard));
    for (int index = 0; index < 17; ++index) {
        keyboard.nodes.append(QString::number(index));
    }
    QVERIFY(!recorder.begin(keyboard));
    QCOMPARE(recorder.snapshot().phase, CapturePhase::Idle);
}

QTEST_GUILESS_MAIN(RecorderTest)
#include "recorder_test.moc"
