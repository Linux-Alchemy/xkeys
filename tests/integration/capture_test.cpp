#include "linux/keyboard_capture.h"
#include "linux/keyboard_inventory.h"

#include <QtTest>

#include <functional>
#include <limits>
#include <linux/input-event-codes.h>

namespace
{
using namespace xkeys;
using namespace xkeys::keyboard;

KeyboardSelection selection(int count = 2)
{
    KeyboardSelection keyboard;
    keyboard.id = QStringLiteral("synthetic");
    keyboard.name = QStringLiteral("Synthetic keyboard");
    keyboard.accessible = true;
    for (int index = 0; index < count; ++index) {
        keyboard.nodes.append(QStringLiteral("fake%1").arg(index));
    }
    return keyboard;
}

Interface interface(const QString &parent, int number, bool keyboard, bool typing)
{
    Interface result;
    result.parentId = parent;
    result.product = QStringLiteral("Test board");
    result.path = QStringLiteral("/dev/input/event%1").arg(number);
    result.name = typing ? QStringLiteral("Typing") : QStringLiteral("Control");
    result.syspath = parent + QStringLiteral("/input/input%1/event%1").arg(number);
    result.deviceNumber = static_cast<quint64>(number);
    result.inode = static_cast<quint64>(100 + number);
    result.ctimeSeconds = 1000;
    result.vendor = 0x1234;
    result.productId = 0x1234;
    result.complete = true;
    result.keyboard = keyboard;
    result.typing = typing;
    result.readable = true;
    return result;
}

Interface keychronInterface(const QString &parent, int number, int product, bool typing)
{
    auto result = interface(parent, number, true, typing);
    result.vendor = 0x3434;
    result.productId = product;
    return result;
}

struct Frame {
    QString type;
    QList<int> codes;
    QString reason;
    qint64 time;
};

struct ScheduledInput {
    int source;
    qint64 time;
    DeviceRead read;
    bool duringRecord = true;
};

class FakeCaptureIo final : public CaptureIo
{
  public:
    Result<void> openDevice(int source) override
    {
        now += acquisitionCost;
        if (source == failOpen) {
            return Result<void>::failure(ErrorCode::Io, QStringLiteral("Open denied."));
        }
        open.insert(source);
        return Result<void>::success();
    }

    Result<void> grabDevice(int source) override
    {
        now += acquisitionCost;
        if (source == failGrab) {
            return Result<void>::failure(ErrorCode::Conflict, QStringLiteral("Already grabbed."));
        }
        grabbed.insert(source);
        if (afterGrab) {
            afterGrab(source);
        }
        return Result<void>::success();
    }

    Result<void> validateGroup() override
    {
        if (++verifications == failValidation) {
            return Result<void>::failure(ErrorCode::Invalid, QStringLiteral("Group changed."));
        }
        return Result<void>::success();
    }

    Result<bool> neutral(int source) override
    {
        if (checkingNeutral) {
            checkingNeutral(source);
        }
        if (source == faultySource && now >= faultAt && now < faultUntil) {
            return Result<bool>::failure(ErrorCode::Io, QStringLiteral("Transient state error."));
        }
        return Result<bool>::success(source != heldBeforeGrab &&
                                     (source != heldAfterGrab || !grabbed.contains(source)) &&
                                     held.value(source).isEmpty());
    }

    Result<DeviceRead> readDevice(int source) override
    {
        if (source == faultySource && now >= faultAt && now < faultUntil) {
            return Result<DeviceRead>::failure(ErrorCode::Io, QStringLiteral("Read failed."));
        }
        for (auto item = scheduled.begin(); item != scheduled.end(); ++item) {
            if (item->source != source || item->time > now || (item->duringRecord && !ready)) {
                continue;
            }
            const auto event = item->read;
            scheduled.erase(item);
            if (event.kind == ReadKind::Key) {
                if (event.value == 1) {
                    held[source].insert(event.code);
                } else if (event.value == 0) {
                    held[source].remove(event.code);
                }
            }
            if (afterRead) {
                afterRead(source, event);
            }
            return Result<DeviceRead>::success(event);
        }
        return Result<DeviceRead>::success({});
    }

    void closeDevice(int source) override
    {
        open.remove(source);
        grabbed.remove(source);
        closed.append(source);
        closedAt.append(now);
    }

    qint64 elapsedMs() const override { return now; }
    qint64 eventTimeUs() const override { return now * 1000; }

    ParentInput readParent() override
    {
        if (now >= cancelAt) {
            return {false, cancelReason};
        }
        return {now < heartbeatUntil, {}};
    }

    bool publish(const QString &type, const QList<int> &codes, const QString &reason) override
    {
        if (type == QStringLiteral("ready")) {
            ready = true;
            readyAfterWholeGroupGrabbed = open.size() == expectedCount && open == grabbed;
        } else if (type == QStringLiteral("progress")) {
            if (failProgress) {
                return false;
            }
        } else {
            terminalAfterRelease = open.isEmpty() && grabbed.isEmpty();
        }
        frames.append({type, codes, reason, now});
        return true;
    }

    void wait(int milliseconds) override
    {
        if (faultySource >= 0 && now >= faultAt && now < faultUntil) {
            ++busyWaits;
            ++now;
        } else {
            now += milliseconds;
        }
    }

    void waitForDrain(int milliseconds) override
    {
        ++drainWaits;
        now += milliseconds;
    }

    void key(int source, qint64 at, int code, int value, qint64 timestampUs = -1,
             bool duringRecord = true)
    {
        scheduled.append({source,
                          at,
                          {ReadKind::Key, code, value, timestampUs < 0 ? at * 1000 : timestampUs},
                          duringRecord});
    }

    QSet<int> open;
    QSet<int> grabbed;
    QList<int> closed;
    QList<qint64> closedAt;
    QList<Frame> frames;
    QList<ScheduledInput> scheduled;
    QMap<int, QSet<int>> held;
    qint64 now = 0;
    qint64 cancelAt = std::numeric_limits<qint64>::max();
    qint64 heartbeatUntil = std::numeric_limits<qint64>::max();
    QString cancelReason = QStringLiteral("The editor cancelled.");
    int expectedCount = 2;
    int failOpen = -1;
    int failGrab = -1;
    int failValidation = -1;
    int heldBeforeGrab = -1;
    int heldAfterGrab = -1;
    int verifications = 0;
    int acquisitionCost = 0;
    int faultySource = -1;
    int busyWaits = 0;
    int drainWaits = 0;
    qint64 faultAt = 0;
    qint64 faultUntil = 0;
    std::function<void(int)> afterGrab;
    std::function<void(int)> checkingNeutral;
    std::function<void(int, const DeviceRead &)> afterRead;
    bool failProgress = false;
    bool ready = false;
    bool readyAfterWholeGroupGrabbed = false;
    bool terminalAfterRelease = false;
};

} // namespace

class CaptureTest : public QObject
{
    Q_OBJECT

  private slots:
    void groupsOnlyPhysicalUsbKeyboardInterfaces();
    void inventoryRejectsIncompleteOrChangedGroups();
    void aggregatesKeychronRoutesAndDetectsChanges();
    void rollsBackAllRoutesAndCancelsOnTopologyChange();
    void ordersEventsAcrossSources();
    void refusesPreArmInput_data();
    void refusesPreArmInput();
    void defersEventsNewerThanSweepWatermark();
    void rejectsLateEventsAcrossSources();
    void rejectsAmbiguousEqualTimestamps();
    void rollsBackBeforeReady_data();
    void rollsBackBeforeReady();
    void cancelsAndDrainsHeldKeys_data();
    void cancelsAndDrainsHeldKeys();
    void enforcesHeartbeatAndDrainDeadlines();
    void enforcesRecordingAndAbsoluteDeadlines();
    void checksCancellationAfterFinalNeutral_data();
    void checksCancellationAfterFinalNeutral();
    void preReadyRollbackDoesNotDrainUnknownHolds();
    void preservesGrabThroughTransientReadAndStateErrors();
};

void CaptureTest::groupsOnlyPhysicalUsbKeyboardInterfaces()
{
    const QString usb = QStringLiteral("/sys/devices/pci0000:00/usb1/1-3");
    auto pad = interface(QStringLiteral("/sys/devices/pci0000:00/usb1/1-4"), 4, true, true);
    pad.vendor = 0x1532;
    pad.productId = 0x022b;
    auto keyboard = interface(usb, 6, true, true);
    auto consumer = interface(usb, 2, true, false);
    const auto pointer = interface(usb, 3, false, false);
    const auto virtualKeyboard =
        interface(QStringLiteral("/sys/devices/virtual/input"), 8, true, true);
    const auto noUsb = interface(QString{}, 9, true, true);
    const auto groups = groupInterfaces({pad, keyboard, consumer, pointer, virtualKeyboard, noUsb});
    QVERIFY(groups);
    QCOMPARE(groups.value().size(), 1);
    const auto &group = groups.value().first();
    QCOMPARE(group.selection.id, usb);
    QCOMPARE(group.selection.nodes, (QStringList{consumer.path, keyboard.path}));
    QCOMPARE(group.selection.interfaceNames, (QStringList{consumer.name, keyboard.name}));
    QCOMPARE(group.selection.typingNames, QStringList{keyboard.name});
    QVERIFY(group.selection.accessible);
    QCOMPARE(group.selection.fingerprint.size(), 64);
    const auto reordered = groupInterfaces({consumer, pointer, keyboard});
    QVERIFY(reordered);
    QCOMPARE(reordered.value().first().selection.fingerprint, group.selection.fingerprint);

    const auto renumbered = interface(usb, 11, true, true);
    const auto moved = groupInterfaces({consumer, pointer, renumbered});
    QVERIFY(moved);
    QCOMPARE(moved.value().first().selection.id, group.selection.id);
    QVERIFY(!renumbered.sameIdentity(keyboard));
    QVERIFY(consumer.sameIdentity(group.interfaces.first()));
}

void CaptureTest::inventoryRejectsIncompleteOrChangedGroups()
{
    const QString usb = QStringLiteral("/sys/devices/pci0000:00/usb1/1-3");
    auto keyboard = interface(usb, 6, true, true);
    auto consumer = interface(usb, 2, true, false);
    const auto assertUnavailable = [](const QList<Interface> &input) {
        const auto groups = groupInterfaces(input);
        QVERIFY(groups);
        QCOMPARE(groups.value().size(), 1);
        QVERIFY(!groups.value().first().selection.accessible);
        QVERIFY(!groups.value().first().selection.reason.isEmpty());
    };

    consumer.readable = false;
    assertUnavailable({keyboard, consumer});
    consumer.readable = true;
    consumer.complete = false;
    assertUnavailable({keyboard, consumer});
    consumer.complete = true;
    consumer.path = QStringLiteral("/dev/input/by-id/symlink");
    assertUnavailable({keyboard, consumer});
    consumer = interface(usb, 2, true, false);
    consumer.deviceNumber = keyboard.deviceNumber;
    assertUnavailable({keyboard, consumer});
    consumer = interface(usb, 2, true, false);
    consumer.productId++;
    assertUnavailable({keyboard, consumer});
    QList<Interface> oversized{keyboard};
    for (int index = 10; index <= 25; ++index) {
        oversized.append(interface(usb, index, true, false));
    }
    assertUnavailable(oversized);
    QVERIFY(!groupInterfaces(QList<Interface>(129, keyboard)));
}

void CaptureTest::aggregatesKeychronRoutesAndDetectsChanges()
{
    const QString wiredUsb = QStringLiteral("/sys/devices/pci0000:00/usb1/1-3");
    const QString linkUsb = QStringLiteral("/sys/devices/pci0000:00/usb1/1-4");
    const QString secondUsb = QStringLiteral("/sys/devices/pci0000:00/usb1/1-5");
    const auto wired = keychronInterface(wiredUsb, 3, 0x0810, true);
    const auto control = keychronInterface(wiredUsb, 4, 0x0810, false);
    const auto link = keychronInterface(linkUsb, 7, 0xd030, true);
    auto pointer = keychronInterface(linkUsb, 8, 0xd030, false);
    pointer.keyboard = false;

    const auto both = groupInterfaces({link, wired, control, pointer});
    const auto wireOnly = groupInterfaces({wired, control});
    const auto linkOnly = groupInterfaces({link, pointer});
    QVERIFY(both && wireOnly && linkOnly);
    QCOMPARE(both.value().size(), 1);
    QCOMPARE(wireOnly.value().size(), 1);
    QCOMPARE(linkOnly.value().size(), 1);
    const auto &combined = both.value().first().selection;
    QCOMPARE(combined.id, QStringLiteral("keyboard:keychron-q1-max"));
    QCOMPARE(combined.id, wireOnly.value().first().selection.id);
    QCOMPARE(combined.id, linkOnly.value().first().selection.id);
    QCOMPARE(combined.nodes, (QStringList{wired.path, control.path, link.path}));
    QCOMPARE(combined.typingNames, (QStringList{wired.name, link.name}));
    QVERIFY(combined.name.contains(QStringLiteral("wired / 2.4 GHz")));
    QVERIFY(wireOnly.value().first().selection.name.endsWith(QStringLiteral("wired")));
    QVERIFY(linkOnly.value().first().selection.name.endsWith(QStringLiteral("2.4 GHz")));
    QVERIFY(combined.accessible);
    QVERIFY(wireOnly.value().first().selection.accessible);
    QVERIFY(linkOnly.value().first().selection.accessible);
    QVERIFY(combined.fingerprint != wireOnly.value().first().selection.fingerprint);
    QVERIFY(combined.fingerprint != linkOnly.value().first().selection.fingerprint);

    auto replaced = link;
    ++replaced.inode;
    ++replaced.ctimeNanoseconds;
    const auto replacement = groupInterfaces({wired, control, replaced, pointer});
    QVERIFY(replacement);
    QCOMPARE(replacement.value().first().selection.id, combined.id);
    QVERIFY(replacement.value().first().selection.fingerprint != combined.fingerprint);
    QVERIFY(!link.sameIdentity(replaced));

    for (const int duplicate : {0x0810, 0xd030}) {
        const auto other = keychronInterface(secondUsb, 9, duplicate, true);
        const auto ambiguous = groupInterfaces({wired, control, link, other});
        QVERIFY(ambiguous);
        QCOMPARE(ambiguous.value().size(), 1);
        QVERIFY(!ambiguous.value().first().selection.accessible);
        QVERIFY(ambiguous.value().first().selection.reason.contains(QStringLiteral("Multiple")));
    }

    replaced = link;
    replaced.typing = false;
    const auto partial = groupInterfaces({wired, control, replaced, pointer});
    QVERIFY(partial);
    QVERIFY(!partial.value().first().selection.accessible);
}

void CaptureTest::rollsBackAllRoutesAndCancelsOnTopologyChange()
{
    const QString wiredUsb = QStringLiteral("/sys/devices/pci0000:00/usb1/1-3");
    const QString linkUsb = QStringLiteral("/sys/devices/pci0000:00/usb1/1-4");
    const auto devices = groupInterfaces({keychronInterface(wiredUsb, 3, 0x0810, true),
                                          keychronInterface(wiredUsb, 4, 0x0810, false),
                                          keychronInterface(linkUsb, 7, 0xd030, true)});
    QVERIFY(devices);
    const auto &keyboard = devices.value().first().selection;
    QVERIFY(keyboard.accessible);

    FakeCaptureIo busyLink;
    busyLink.failGrab = 2;
    QCOMPARE(runCapture(keyboard, busyLink), 1);
    QVERIFY(!busyLink.ready);
    QVERIFY(busyLink.terminalAfterRelease);
    QCOMPARE(busyLink.closed, (QList<int>{0, 1, 2}));
    QCOMPARE(busyLink.drainWaits, 0);

    FakeCaptureIo changed;
    changed.expectedCount = 3;
    changed.failValidation = 3;
    changed.key(1, 20, KEY_E, 1);
    changed.key(1, 400, KEY_E, 0);
    QCOMPARE(runCapture(keyboard, changed), 0);
    QVERIFY(changed.readyAfterWholeGroupGrabbed);
    QCOMPARE(changed.frames.last().type, QStringLiteral("cancelled"));
    QVERIFY(changed.frames.last().reason.contains(QStringLiteral("Group changed")));
    QVERIFY(changed.terminalAfterRelease);
    QCOMPARE(changed.closedAt.size(), 3);
    for (const qint64 closed : std::as_const(changed.closedAt)) {
        QVERIFY(closed >= 400 && closed < 420);
    }
}

void CaptureTest::ordersEventsAcrossSources()
{
    FakeCaptureIo io;
    io.now = 10;
    io.key(0, 30, KEY_E, 1, 28000);
    io.key(0, 30, KEY_E, 0, 29000);
    io.key(1, 30, KEY_RIGHTCTRL, 1, 27000);
    io.key(1, 30, KEY_RIGHTCTRL, 0, 30000);
    QCOMPARE(runCapture(selection(), io), 0);
    QVERIFY(io.readyAfterWholeGroupGrabbed);
    QVERIFY(io.terminalAfterRelease);
    QCOMPARE(io.frames.first().type, QStringLiteral("ready"));
    QCOMPARE(io.frames.last().type, QStringLiteral("complete"));
    QCOMPARE(io.frames.last().codes, (QList<int>{KEY_RIGHTCTRL, KEY_E}));
    QCOMPARE(io.frames.size(), 4);
    QCOMPARE(io.frames.at(1).codes, QList<int>{KEY_RIGHTCTRL});
    QCOMPARE(io.closed, (QList<int>{0, 1}));
}

void CaptureTest::refusesPreArmInput_data()
{
    QTest::addColumn<bool>("delayed");
    QTest::newRow("down queued during final neutral") << false;
    QTest::newRow("pre-arm event delivered after ready") << true;
}

void CaptureTest::refusesPreArmInput()
{
    QFETCH(bool, delayed);
    FakeCaptureIo io;
    io.now = 10;
    if (delayed) {
        io.key(0, 10, KEY_LEFTCTRL, 1, 9000);
        io.key(0, 40, KEY_LEFTCTRL, 0);
    } else {
        io.checkingNeutral = [&](int source) {
            if (source == 1 && io.verifications == 2 && io.scheduled.isEmpty()) {
                io.key(0, 10, KEY_LEFTCTRL, 1, 10000, false);
            }
        };
    }
    QCOMPARE(runCapture(selection(), io), delayed ? 0 : 1);
    QCOMPARE(io.ready, delayed);
    QVERIFY(io.terminalAfterRelease);
    QCOMPARE(io.frames.size(), delayed ? 2 : 1);
    QVERIFY(io.frames.last().reason.contains(QStringLiteral("preparing")));
    QVERIFY(io.frames.last().codes.isEmpty());
}

void CaptureTest::defersEventsNewerThanSweepWatermark()
{
    FakeCaptureIo io;
    io.key(1, 20, KEY_E, 1, 22000);
    io.key(1, 60, KEY_E, 0, 60000);
    io.key(0, 80, KEY_LEFTCTRL, 0, 79000);
    io.afterRead = [&](int source, const DeviceRead &read) {
        if (source == 1 && read.kind == ReadKind::Key && read.value == 1) {
            // Source zero has already been swept. The E press must wait
            // until the older Ctrl press can be considered with it.
            io.key(0, 20, KEY_LEFTCTRL, 1, 21000);
        }
    };
    QCOMPARE(runCapture(selection(), io), 0);
    QCOMPARE(io.frames.last().type, QStringLiteral("complete"));
    QCOMPARE(io.frames.last().codes, (QList<int>{KEY_LEFTCTRL, KEY_E}));
    QVERIFY(io.terminalAfterRelease);
}

void CaptureTest::rejectsLateEventsAcrossSources()
{
    FakeCaptureIo io;
    io.key(1, 20, KEY_LEFTCTRL, 1);
    io.key(1, 40, KEY_LEFTCTRL, 0);
    io.afterRead = [&](int source, const DeviceRead &read) {
        if (source == 1 && read.kind == ReadKind::Key && read.value == 0) {
            io.key(0, 40, KEY_E, 1, 35000);
            io.key(0, 40, KEY_E, 0, 36000);
        }
    };
    QCOMPARE(runCapture(selection(), io), 0);
    QCOMPARE(io.frames.last().type, QStringLiteral("cancelled"));
    QVERIFY(io.frames.last().reason.contains(QStringLiteral("out of order")));
    QVERIFY(io.frames.last().codes.isEmpty());
    QVERIFY(io.terminalAfterRelease);
}

void CaptureTest::rejectsAmbiguousEqualTimestamps()
{
    FakeCaptureIo io;
    io.key(0, 20, KEY_LEFTCTRL, 1);
    io.key(0, 40, KEY_LEFTCTRL, 0);
    io.key(1, 40, KEY_E, 1);
    io.key(1, 60, KEY_E, 0);
    QCOMPARE(runCapture(selection(), io), 0);
    QCOMPARE(io.frames.last().type, QStringLiteral("cancelled"));
    QVERIFY(io.frames.last().reason.contains(QStringLiteral("ambiguous")));
    QVERIFY(io.frames.last().codes.isEmpty());
    QVERIFY(io.terminalAfterRelease);
}

void CaptureTest::rollsBackBeforeReady_data()
{
    QTest::addColumn<QString>("failure");
    for (const auto *failure : {"open", "grab", "change before grab", "change after grab",
                                "pre-neutral", "post-neutral", "unavailable"}) {
        QTest::newRow(failure) << QString::fromLatin1(failure);
    }
}

void CaptureTest::rollsBackBeforeReady()
{
    QFETCH(QString, failure);
    FakeCaptureIo io;
    auto keyboard = selection();
    if (failure == QStringLiteral("open")) {
        io.failOpen = 1;
    } else if (failure == QStringLiteral("grab")) {
        io.failGrab = 1;
    } else if (failure == QStringLiteral("change before grab")) {
        io.failValidation = 1;
    } else if (failure == QStringLiteral("change after grab")) {
        io.failValidation = 2;
    } else if (failure == QStringLiteral("pre-neutral")) {
        io.heldBeforeGrab = 0;
    } else if (failure == QStringLiteral("post-neutral")) {
        io.heldAfterGrab = 0;
    } else {
        keyboard.accessible = false;
    }
    QVERIFY(runCapture(keyboard, io) != 0);
    QVERIFY(!io.ready);
    QVERIFY(io.terminalAfterRelease);
    QCOMPARE(io.frames.size(), 1);
    QCOMPARE(io.frames.first().type, QStringLiteral("error"));
    QVERIFY(!io.frames.first().reason.isEmpty());
    QVERIFY(io.frames.first().codes.isEmpty());
    if (failure == QStringLiteral("open")) {
        QCOMPARE(io.closed, QList<int>{0});
    } else if (keyboard.accessible) {
        QCOMPARE(io.closed, (QList<int>{0, 1}));
    }
}

void CaptureTest::cancelsAndDrainsHeldKeys_data()
{
    QTest::addColumn<QString>("failure");
    for (const auto *failure : {"mouse cancel", "client loss", "invalid command", "sync loss",
                                "unplug", "unsupported key", "broken output"}) {
        QTest::newRow(failure) << QString::fromLatin1(failure);
    }
}

void CaptureTest::cancelsAndDrainsHeldKeys()
{
    QFETCH(QString, failure);
    FakeCaptureIo io;
    io.key(1, 20, KEY_LEFTCTRL, 1);
    io.key(1, 160, KEY_LEFTCTRL, 0);
    if (failure == QStringLiteral("sync loss")) {
        io.scheduled.append({0, 40, {ReadKind::SyncDropped}});
    } else if (failure == QStringLiteral("unplug")) {
        io.scheduled.append({0, 40, {ReadKind::Disconnected}});
    } else if (failure == QStringLiteral("unsupported key")) {
        io.key(0, 40, BTN_LEFT, 1);
        io.key(0, 160, BTN_LEFT, 0);
    } else if (failure == QStringLiteral("broken output")) {
        io.failProgress = true;
    } else {
        io.cancelAt = 40;
        io.cancelReason = failure;
    }

    QCOMPARE(runCapture(selection(), io), 0);
    QVERIFY(io.readyAfterWholeGroupGrabbed);
    QVERIFY(io.terminalAfterRelease);
    QCOMPARE(io.frames.last().type, QStringLiteral("cancelled"));
    QVERIFY(!io.frames.last().reason.isEmpty());
    QVERIFY(io.frames.last().codes.isEmpty());
    QVERIFY(io.frames.last().time >= 160);
    QVERIFY(io.frames.last().time < 200);
    QCOMPARE(io.closed.size(), 2);
}

void CaptureTest::enforcesHeartbeatAndDrainDeadlines()
{
    FakeCaptureIo io;
    io.heartbeatUntil = 0;
    io.key(1, 20, KEY_LEFTCTRL, 1);
    QCOMPARE(runCapture(selection(), io), 0);
    QVERIFY(io.readyAfterWholeGroupGrabbed);
    QVERIFY(io.terminalAfterRelease);
    QCOMPARE(io.frames.last().type, QStringLiteral("cancelled"));
    QVERIFY(io.frames.last().reason.contains(QStringLiteral("lease")));
    QVERIFY(io.frames.last().reason.contains(QStringLiteral("safety deadline")));
    QCOMPARE(io.frames.last().time, 3000);
    QVERIFY(io.frames.last().codes.isEmpty());
}

void CaptureTest::enforcesRecordingAndAbsoluteDeadlines()
{
    FakeCaptureIo idle;
    QCOMPARE(runCapture(selection(), idle), 0);
    QCOMPARE(idle.frames.last().type, QStringLiteral("cancelled"));
    QCOMPARE(idle.frames.last().time, 10000);
    QVERIFY(idle.terminalAfterRelease);

    FakeCaptureIo delayed;
    delayed.expectedCount = 8;
    delayed.acquisitionCost = 500;
    delayed.key(1, 8020, KEY_LEFTCTRL, 1);
    QCOMPARE(runCapture(selection(8), delayed), 0);
    QVERIFY(delayed.readyAfterWholeGroupGrabbed);
    QCOMPARE(delayed.frames.first().time, 8000);
    QCOMPARE(delayed.frames.last().type, QStringLiteral("cancelled"));
    QCOMPARE(delayed.frames.last().time, 15000);
    QVERIFY(delayed.terminalAfterRelease);
}

void CaptureTest::checksCancellationAfterFinalNeutral_data()
{
    QTest::addColumn<QString>("fault");
    for (const auto *fault : {"cancel", "heartbeat", "deadline"}) {
        QTest::newRow(fault) << QString::fromLatin1(fault);
    }
}

void CaptureTest::checksCancellationAfterFinalNeutral()
{
    QFETCH(QString, fault);
    FakeCaptureIo io;
    io.key(0, 20, KEY_E, 1);
    io.key(0, 40, KEY_E, 0);
    io.checkingNeutral = [&](int source) {
        if (source != 0 || !io.ready || io.now < 80) {
            return;
        }
        if (fault == QStringLiteral("cancel")) {
            io.cancelAt = io.now;
        } else if (fault == QStringLiteral("heartbeat")) {
            io.heartbeatUntil = 0;
            io.now = 1080;
        } else {
            io.now = 10000;
        }
    };
    QCOMPARE(runCapture(selection(), io), 0);
    QCOMPARE(io.frames.last().type, QStringLiteral("cancelled"));
    QVERIFY(io.frames.last().codes.isEmpty());
    QVERIFY(io.terminalAfterRelease);
}

void CaptureTest::preReadyRollbackDoesNotDrainUnknownHolds()
{
    FakeCaptureIo io;
    io.failGrab = 1;
    io.afterGrab = [&](int source) { io.held[source].insert(KEY_LEFTCTRL); };
    QCOMPARE(runCapture(selection(), io), 1);
    QVERIFY(!io.ready);
    QCOMPARE(io.frames.last().type, QStringLiteral("error"));
    QCOMPARE(io.closedAt, (QList<qint64>{0, 0}));
    QCOMPARE(io.drainWaits, 0);
    QVERIFY(io.terminalAfterRelease);
}

void CaptureTest::preservesGrabThroughTransientReadAndStateErrors()
{
    FakeCaptureIo io;
    io.key(1, 20, KEY_LEFTCTRL, 1);
    io.key(1, 160, KEY_LEFTCTRL, 0);
    io.faultySource = 1;
    io.faultAt = 40;
    io.faultUntil = 160;
    QCOMPARE(runCapture(selection(), io), 0);
    QCOMPARE(io.frames.last().type, QStringLiteral("cancelled"));
    QVERIFY(io.frames.last().reason.contains(QStringLiteral("Read failed")));
    QCOMPARE(io.closedAt, (QList<qint64>{160, 160}));
    QCOMPARE(io.busyWaits, 0);
    QCOMPARE(io.drainWaits, 6);
    QVERIFY(io.terminalAfterRelease);
}

QTEST_GUILESS_MAIN(CaptureTest)
#include "capture_test.moc"
