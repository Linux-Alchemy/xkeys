#include "core/profile_store.h"
#include "core/remapper.h"

#include <QtTest>

#include <linux/input-event-codes.h>

namespace
{

using namespace xkeys;
using enum ControlEventType;
using enum KeyTransition;

const auto first = QStringLiteral("key01");
const auto second = QStringLiteral("key02");
const auto blank = QStringLiteral("key03");
const auto thumb = QStringLiteral("thumb");
const auto wheelUp = QStringLiteral("wheelUp");
const auto wheelDown = QStringLiteral("wheelDown");
const auto wheelClick = QStringLiteral("wheelClick");

Profile emptyProfile()
{
    return makeBlankProfile(QStringLiteral("abcdef00-1000-4000-8000-000000000001"),
                            QStringLiteral("Empty"));
}

Profile layeredProfile()
{
    auto profile = makeBlankProfile(QStringLiteral("abcdef00-1000-4000-8000-000000000002"),
                                    QStringLiteral("Layered"));
    profile.thumbLayer = true;
    profile.layers[0][first] = {QStringLiteral("First action"), {KEY_LEFTCTRL, KEY_E}};
    profile.layers[1][first] = {QStringLiteral("Second action"), {KEY_LEFTSHIFT, KEY_F}};
    profile.layers[0][thumb] = {{}, {KEY_A}};
    profile.layers[1][thumb] = {{}, {KEY_B}};
    return profile;
}

} // namespace

class RemapperTest : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void cleanup();
    void inactiveUnknownAndOrphanInputs();
    void derivesChordLabels();
    void emitsOrderedTransitions();
    void blankInBothLayers();
    void thumbAsOrdinaryButton();
    void thumbAndKeyReleaseOrder_data();
    void thumbAndKeyReleaseOrder();
    void sharedKeysReleaseWithLastOwner();
    void ignoresRepeatedAndDuplicateDown();
    void refusesActivationWhileControlsHeld();
    void invalidProfileLeavesActiveProfileInPlace();
    void balancesWheelTaps();
    void resetsOwnershipInReverseFirstDownOrder();

  private:
    Remapper m_remapper;
};

void RemapperTest::init() { m_remapper = Remapper{}; }

void RemapperTest::cleanup()
{
    QVERIFY(m_remapper.ownedKeys().isEmpty());
    QVERIFY(m_remapper.heldControls().isEmpty());
    QCOMPARE(m_remapper.activeLayer(), 1);
}

void RemapperTest::inactiveUnknownAndOrphanInputs()
{
    QVERIFY(!m_remapper.isActive());
    auto result = m_remapper.consume({first, Press});
    QVERIFY(!result.valid);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(result.output.isEmpty());
    QVERIFY(m_remapper.reset().isEmpty());

    QVERIFY(m_remapper.activate(emptyProfile()));
    QVERIFY(m_remapper.isActive());
    const auto unknown = QStringLiteral("key99");
    result = m_remapper.consume({unknown, Press});
    QCOMPARE(result.control, unknown);
    QVERIFY(!result.valid);
    QVERIFY(!result.error.isEmpty());
    QVERIFY(result.output.isEmpty());

    for (const auto type : {Release, Repeat}) {
        result = m_remapper.consume({first, type});
        QVERIFY(!result.valid);
        QVERIFY(!result.error.isEmpty());
        QVERIFY(result.output.isEmpty());
    }
    QVERIFY(!m_remapper.consume({thumb, Release}).valid);
}

void RemapperTest::derivesChordLabels()
{
    QCOMPARE(chordLabel({}), QStringLiteral("Blank"));
    QCOMPARE(chordLabel({KEY_LEFTCTRL, KEY_LEFTALT, KEY_E}), QStringLiteral("Ctrl+Alt+E"));
    QCOMPARE(chordLabel({KEY_RIGHTSHIFT, KEY_RIGHTMETA, KEY_9}),
             QStringLiteral("Right Shift+Right Super+9"));
    QCOMPARE(chordLabel({KEY_LEFTMETA, KEY_F12}), QStringLiteral("Super+F12"));
    QCOMPARE(chordLabel({KEY_RIGHTALT, KEY_ENTER}), QStringLiteral("Right Alt+Enter"));
    QCOMPARE(chordLabel({KEY_LEFT, KEY_SPACE}), QStringLiteral("Left+Space"));
    QCOMPARE(chordLabel({KEY_F13}), QStringLiteral("Key %1").arg(KEY_F13));
}

void RemapperTest::emitsOrderedTransitions()
{
    auto profile = layeredProfile();
    profile.layers[0][first] = {QStringLiteral("Action label"), {KEY_LEFTCTRL, KEY_LEFTALT, KEY_E}};
    QVERIFY(m_remapper.activate(profile));

    const auto press = m_remapper.consume({first, Press});
    QVERIFY(press.valid);
    QVERIFY(!press.blank);
    QVERIFY(!press.layerControl);
    QCOMPARE(press.control, first);
    QCOMPARE(press.layer, 1);
    QCOMPARE(press.label, QStringLiteral("Ctrl+Alt+E"));
    QCOMPARE(press.chord, (QList<int>{KEY_LEFTCTRL, KEY_LEFTALT, KEY_E}));
    QCOMPARE(press.output, (OutputBatch{{KEY_LEFTCTRL, Down}, {KEY_LEFTALT, Down}, {KEY_E, Down}}));
    QCOMPARE(m_remapper.heldControls(), QStringList{first});
    QCOMPARE(m_remapper.ownedKeys(), press.chord);

    const auto release = m_remapper.consume({first, Release});
    QVERIFY(release.valid);
    QCOMPARE(release.chord, press.chord);
    QCOMPARE(release.label, press.label);
    QCOMPARE(release.layer, 1);
    QCOMPARE(release.output, (OutputBatch{{KEY_E, Up}, {KEY_LEFTALT, Up}, {KEY_LEFTCTRL, Up}}));
}

void RemapperTest::blankInBothLayers()
{
    auto profile = emptyProfile();
    profile.thumbLayer = true;
    QVERIFY(m_remapper.activate(profile));
    const auto checkBlank = [&](int layer) {
        const auto press = m_remapper.consume({first, Press});
        QVERIFY(press.valid);
        QVERIFY(press.blank);
        QCOMPARE(press.label, QStringLiteral("Blank"));
        QCOMPARE(press.layer, layer);
        QVERIFY(press.output.isEmpty());
        QVERIFY(m_remapper.heldControls().contains(first));
        QVERIFY(m_remapper.ownedKeys().isEmpty());

        const auto release = m_remapper.consume({first, Release});
        QVERIFY(release.valid);
        QVERIFY(release.blank);
        QVERIFY(release.output.isEmpty());
    };

    checkBlank(1);
    QVERIFY(m_remapper.consume({thumb, Press}).layerControl);
    checkBlank(2);
    QVERIFY(m_remapper.consume({thumb, Release}).layerControl);
}

void RemapperTest::thumbAsOrdinaryButton()
{
    auto profile = layeredProfile();
    profile.thumbLayer = false;
    QVERIFY(m_remapper.activate(profile));

    const auto press = m_remapper.consume({thumb, Press});
    QVERIFY(press.valid);
    QVERIFY(!press.layerControl);
    QCOMPARE(press.output, (OutputBatch{{KEY_A, Down}}));
    QCOMPARE(m_remapper.heldControls(), QStringList{thumb});
    QCOMPARE(m_remapper.activeLayer(), 1);

    const auto release = m_remapper.consume({thumb, Release});
    QVERIFY(release.valid);
    QCOMPARE(release.output, (OutputBatch{{KEY_A, Up}}));
}

void RemapperTest::thumbAndKeyReleaseOrder_data()
{
    QTest::addColumn<bool>("thumbFirst");
    QTest::newRow("thumb first") << true;
    QTest::newRow("key first") << false;
}

void RemapperTest::thumbAndKeyReleaseOrder()
{
    QFETCH(bool, thumbFirst);
    QVERIFY(m_remapper.activate(layeredProfile()));

    const auto enterLayer = m_remapper.consume({thumb, Press});
    QVERIFY(enterLayer.valid);
    QVERIFY(enterLayer.layerControl);
    QVERIFY(!enterLayer.blank);
    QVERIFY(enterLayer.chord.isEmpty());
    QCOMPARE(enterLayer.label, QStringLiteral("Layer 2"));
    QCOMPARE(enterLayer.layer, 2);
    QVERIFY(enterLayer.output.isEmpty());
    QCOMPARE(m_remapper.activeLayer(), 2);

    for (const auto type : {Press, Repeat}) {
        const auto duplicate = m_remapper.consume({thumb, type});
        QVERIFY(duplicate.valid);
        QVERIFY(duplicate.layerControl);
        QCOMPARE(duplicate.layer, 2);
        QVERIFY(duplicate.output.isEmpty());
    }
    const auto press = m_remapper.consume({first, Press});
    QCOMPARE(press.label, QStringLiteral("Shift+F"));
    QCOMPARE(press.output, (OutputBatch{{KEY_LEFTSHIFT, Down}, {KEY_F, Down}}));
    QCOMPARE(m_remapper.heldControls(), (QStringList{first, thumb}));

    const auto releaseThumb = [&] {
        const auto leaveLayer = m_remapper.consume({thumb, Release});
        QVERIFY(leaveLayer.valid);
        QVERIFY(leaveLayer.layerControl);
        QVERIFY(!leaveLayer.blank);
        QVERIFY(leaveLayer.chord.isEmpty());
        QCOMPARE(leaveLayer.label, QStringLiteral("Layer 1"));
        QCOMPARE(leaveLayer.layer, 1);
        QVERIFY(leaveLayer.output.isEmpty());
        QCOMPARE(m_remapper.activeLayer(), 1);
    };
    if (thumbFirst) {
        releaseThumb();
        QCOMPARE(m_remapper.ownedKeys(), (QList<int>{KEY_LEFTSHIFT, KEY_F}));
        QCOMPARE(m_remapper.heldControls(), QStringList{first});
        const auto repeat = m_remapper.consume({first, Repeat});
        QCOMPARE(repeat.layer, 2);
        QCOMPARE(repeat.label, QStringLiteral("Shift+F"));
        QVERIFY(repeat.output.isEmpty());
    }
    const auto release = m_remapper.consume({first, Release});
    QVERIFY(release.valid);
    QCOMPARE(release.layer, 2);
    QCOMPARE(release.label, QStringLiteral("Shift+F"));
    QCOMPARE(release.output, (OutputBatch{{KEY_F, Up}, {KEY_LEFTSHIFT, Up}}));
    if (!thumbFirst) {
        QCOMPARE(m_remapper.heldControls(), QStringList{thumb});
        QCOMPARE(m_remapper.activeLayer(), 2);
        releaseThumb();
    }

    const auto next = m_remapper.consume({first, Press});
    QCOMPARE(next.layer, 1);
    QCOMPARE(next.label, QStringLiteral("Ctrl+E"));
    QVERIFY(m_remapper.consume({thumb, Press}).layerControl);
    const auto originalLayer = m_remapper.consume({first, Release});
    QCOMPARE(originalLayer.layer, 1);
    QCOMPARE(originalLayer.output, (OutputBatch{{KEY_E, Up}, {KEY_LEFTCTRL, Up}}));
    QCOMPARE(m_remapper.activeLayer(), 2);
    QVERIFY(m_remapper.consume({thumb, Release}).layerControl);
}

void RemapperTest::sharedKeysReleaseWithLastOwner()
{
    auto profile = emptyProfile();
    profile.layers[0][first] = {{}, {KEY_LEFTCTRL, KEY_E}};
    profile.layers[0][second] = {{}, {KEY_LEFTCTRL, KEY_E, KEY_F}};
    QVERIFY(m_remapper.activate(profile));

    QCOMPARE(m_remapper.consume({first, Press}).output,
             (OutputBatch{{KEY_LEFTCTRL, Down}, {KEY_E, Down}}));
    QCOMPARE(m_remapper.consume({second, Press}).output, (OutputBatch{{KEY_F, Down}}));
    QCOMPARE(m_remapper.heldControls(), (QStringList{first, second}));
    QCOMPARE(m_remapper.ownedKeys(), (QList<int>{KEY_LEFTCTRL, KEY_E, KEY_F}));

    QCOMPARE(m_remapper.consume({second, Release}).output, (OutputBatch{{KEY_F, Up}}));
    QCOMPARE(m_remapper.ownedKeys(), (QList<int>{KEY_LEFTCTRL, KEY_E}));
    QVERIFY(m_remapper.consume({second, Press}).valid);
    QVERIFY(m_remapper.consume({first, Release}).output.isEmpty());
    QCOMPARE(m_remapper.ownedKeys(), (QList<int>{KEY_LEFTCTRL, KEY_E, KEY_F}));
    QCOMPARE(m_remapper.consume({second, Release}).output,
             (OutputBatch{{KEY_F, Up}, {KEY_E, Up}, {KEY_LEFTCTRL, Up}}));
}

void RemapperTest::ignoresRepeatedAndDuplicateDown()
{
    QVERIFY(m_remapper.activate(layeredProfile()));
    const auto press = m_remapper.consume({first, Press});
    QCOMPARE(press.output, (OutputBatch{{KEY_LEFTCTRL, Down}, {KEY_E, Down}}));
    for (const auto type : {Repeat, Press, Repeat, Press}) {
        const auto repeated = m_remapper.consume({first, type});
        QVERIFY(repeated.valid);
        QCOMPARE(repeated.label, press.label);
        QCOMPARE(repeated.layer, press.layer);
        QCOMPARE(repeated.chord, press.chord);
        QVERIFY(repeated.output.isEmpty());
        QCOMPARE(m_remapper.heldControls(), QStringList{first});
        QCOMPARE(m_remapper.ownedKeys(), press.chord);
    }
    QCOMPARE(m_remapper.consume({first, Release}).output,
             (OutputBatch{{KEY_E, Up}, {KEY_LEFTCTRL, Up}}));
    QVERIFY(!m_remapper.consume({first, Release}).valid);
}

void RemapperTest::refusesActivationWhileControlsHeld()
{
    QVERIFY(m_remapper.activate(layeredProfile()));
    const auto replacement = emptyProfile();
    QVERIFY(m_remapper.consume({blank, Press}).blank);
    const auto blankConflict = m_remapper.activate(replacement);
    QVERIFY(!blankConflict);
    QCOMPARE(blankConflict.error().code, ErrorCode::Conflict);
    QVERIFY(m_remapper.ownedKeys().isEmpty());
    QCOMPARE(m_remapper.heldControls(), QStringList{blank});
    QVERIFY(m_remapper.consume({blank, Release}).blank);

    QVERIFY(m_remapper.consume({thumb, Press}).layerControl);
    const auto thumbConflict = m_remapper.activate(replacement);
    QVERIFY(!thumbConflict);
    QCOMPARE(thumbConflict.error().code, ErrorCode::Conflict);
    QCOMPARE(m_remapper.activeLayer(), 2);
    QCOMPARE(m_remapper.heldControls(), QStringList{thumb});
    QCOMPARE(m_remapper.consume({first, Press}).output,
             (OutputBatch{{KEY_LEFTSHIFT, Down}, {KEY_F, Down}}));
    const auto chordConflict = m_remapper.activate(replacement);
    QVERIFY(!chordConflict);
    QCOMPARE(chordConflict.error().code, ErrorCode::Conflict);
    QCOMPARE(m_remapper.ownedKeys(), (QList<int>{KEY_LEFTSHIFT, KEY_F}));
    QVERIFY(m_remapper.consume({first, Release}).valid);
    QVERIFY(m_remapper.consume({thumb, Release}).layerControl);

    QVERIFY(m_remapper.activate(replacement));
    QVERIFY(m_remapper.consume({first, Press}).blank);
    QVERIFY(m_remapper.consume({first, Release}).blank);
}

void RemapperTest::invalidProfileLeavesActiveProfileInPlace()
{
    auto invalid = emptyProfile();
    invalid.layers[1].remove(first);
    const auto firstAttempt = m_remapper.activate(invalid);
    QVERIFY(!firstAttempt);
    QCOMPARE(firstAttempt.error().code, ErrorCode::Invalid);
    QVERIFY(!m_remapper.isActive());

    QVERIFY(m_remapper.activate(layeredProfile()));
    const auto rejected = m_remapper.activate(invalid);
    QVERIFY(!rejected);
    QCOMPARE(rejected.error().code, ErrorCode::Invalid);
    QVERIFY(m_remapper.isActive());
    QCOMPARE(m_remapper.consume({first, Press}).output,
             (OutputBatch{{KEY_LEFTCTRL, Down}, {KEY_E, Down}}));
    QCOMPARE(m_remapper.consume({first, Release}).output,
             (OutputBatch{{KEY_E, Up}, {KEY_LEFTCTRL, Up}}));
}

void RemapperTest::balancesWheelTaps()
{
    auto profile = emptyProfile();
    profile.layers[0][wheelUp] = {{}, {KEY_LEFTCTRL, KEY_E}};
    profile.layers[0][wheelDown] = {{}, {KEY_LEFTCTRL, KEY_F}};
    profile.layers[0][wheelClick] = {{}, {KEY_LEFTCTRL, KEY_E}};
    QVERIFY(m_remapper.activate(profile));

    QCOMPARE(m_remapper.consume({wheelUp, Press}).output,
             (OutputBatch{{KEY_LEFTCTRL, Down}, {KEY_E, Down}, {KEY_E, Up}, {KEY_LEFTCTRL, Up}}));
    QCOMPARE(m_remapper.consume({wheelDown, Press}).output,
             (OutputBatch{{KEY_LEFTCTRL, Down}, {KEY_F, Down}, {KEY_F, Up}, {KEY_LEFTCTRL, Up}}));
    QVERIFY(m_remapper.heldControls().isEmpty());
    QVERIFY(m_remapper.ownedKeys().isEmpty());

    for (const auto &notch : {wheelUp, wheelDown}) {
        for (const auto type : {Release, Repeat}) {
            const auto ignored = m_remapper.consume({notch, type});
            QVERIFY(ignored.valid);
            QVERIFY(ignored.output.isEmpty());
        }
    }

    QCOMPARE(m_remapper.consume({wheelClick, Press}).output,
             (OutputBatch{{KEY_LEFTCTRL, Down}, {KEY_E, Down}}));
    QCOMPARE(m_remapper.heldControls(), QStringList{wheelClick});
    QVERIFY(m_remapper.consume({wheelUp, Press}).output.isEmpty());
    QCOMPARE(m_remapper.consume({wheelDown, Press}).output,
             (OutputBatch{{KEY_F, Down}, {KEY_F, Up}}));
    QCOMPARE(m_remapper.ownedKeys(), (QList<int>{KEY_LEFTCTRL, KEY_E}));
    QCOMPARE(m_remapper.heldControls(), QStringList{wheelClick});
    QCOMPARE(m_remapper.consume({wheelClick, Release}).output,
             (OutputBatch{{KEY_E, Up}, {KEY_LEFTCTRL, Up}}));
}

void RemapperTest::resetsOwnershipInReverseFirstDownOrder()
{
    auto profile = layeredProfile();
    profile.layers[0][second] = {{}, {KEY_LEFTCTRL, KEY_G}};
    QVERIFY(m_remapper.activate(profile));

    QCOMPARE(m_remapper.consume({second, Press}).output,
             (OutputBatch{{KEY_LEFTCTRL, Down}, {KEY_G, Down}}));
    QCOMPARE(m_remapper.consume({first, Press}).output, (OutputBatch{{KEY_E, Down}}));
    QVERIFY(m_remapper.consume({thumb, Press}).layerControl);
    QVERIFY(m_remapper.consume({blank, Press}).blank);
    QCOMPARE(m_remapper.heldControls(), (QStringList{first, second, blank, thumb}));
    QCOMPARE(m_remapper.ownedKeys(), (QList<int>{KEY_LEFTCTRL, KEY_G, KEY_E}));
    QCOMPARE(m_remapper.activeLayer(), 2);

    QCOMPARE(m_remapper.reset(), (OutputBatch{{KEY_E, Up}, {KEY_G, Up}, {KEY_LEFTCTRL, Up}}));
    QVERIFY(m_remapper.isActive());
    QCOMPARE(m_remapper.activeLayer(), 1);
    QVERIFY(m_remapper.heldControls().isEmpty());
    QVERIFY(m_remapper.ownedKeys().isEmpty());
    QVERIFY(m_remapper.reset().isEmpty());
    QVERIFY(!m_remapper.consume({first, Release}).valid);
    QVERIFY(!m_remapper.consume({thumb, Repeat}).valid);
    QCOMPARE(m_remapper.consume({first, Press}).output,
             (OutputBatch{{KEY_LEFTCTRL, Down}, {KEY_E, Down}}));
    QCOMPARE(m_remapper.consume({first, Release}).output,
             (OutputBatch{{KEY_E, Up}, {KEY_LEFTCTRL, Up}}));
}

QTEST_GUILESS_MAIN(RemapperTest)
#include "remapper_test.moc"
