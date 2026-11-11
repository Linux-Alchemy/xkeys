#include "src/core/profile_store.h"
#include "src/core/remapper.h"
#include "src/ui/capture_backend.h"
#include "src/ui/controller.h"
#include "src/ui/hyprland_context.h"

#include <QGuiApplication>
#include <QImage>
#include <QLocalSocket>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QTimer>
#include <QtTest>

#include <linux/input-event-codes.h>
#include <memory>

namespace
{

using namespace xkeys;

const auto key01 = QStringLiteral("key01");
const auto key03 = QStringLiteral("key03");
const auto keyboardId = QStringLiteral("synthetic-usb");
const auto keyboardName = QStringLiteral("Synthetic keyboard");
const QList<int> ordinaryChord{KEY_LEFTCTRL, KEY_LEFTALT, KEY_F};
const QList<int> existingChord{KEY_LEFTCTRL, KEY_LEFTALT, KEY_E};
const QList<int> layerTwoChord{KEY_W};
const QList<int> conflictChord{KEY_LEFTMETA, KEY_LEFTSHIFT, KEY_J};
const auto conflictDescription = QStringLiteral("Synthetic Hyprland binding");

class FakeCaptureBackend final : public CaptureBackend
{
  public:
    bool available() const override { return supported; }
    QString unavailableReason() const override
    {
        return supported ? QString{} : QStringLiteral("Synthetic capture unavailable.");
    }
    Result<QList<KeyboardSelection>> keyboards() override
    {
        QList<KeyboardSelection> keyboards{KeyboardSelection{
            .id = keyboardId,
            .name = keyboardName,
            .nodes = {QStringLiteral("fake0")},
            .interfaceNames = {QStringLiteral("synthetic-keyboard")},
            .typingNames = {QStringLiteral("synthetic-keyboard")},
            .accessible = accessible,
            .reason = accessible ? QString{} : QStringLiteral("Synthetic access denied."),
            .fingerprint = fingerprint,
        }};
        if (includeInaccessibleKeyboard) {
            keyboards.append(KeyboardSelection{
                .id = QStringLiteral("inaccessible-usb"),
                .name = QStringLiteral("Inaccessible keyboard"),
                .nodes = {QStringLiteral("fake1")},
                .interfaceNames = {QStringLiteral("inaccessible-keyboard")},
                .typingNames = {QStringLiteral("inaccessible-keyboard")},
                .accessible = false,
                .reason = QStringLiteral("Synthetic access denied."),
                .fingerprint = QString(64, QLatin1Char('c')),
            });
        }
        return Result<QList<KeyboardSelection>>::success(std::move(keyboards));
    }
    void start(const KeyboardSelection &keyboard) override
    {
        ++starts;
        startedKeyboard = keyboard.id;
        startedFingerprint = keyboard.fingerprint;
        running = true;
        if (autoReady) {
            QTimer::singleShot(0, this, [this] {
                if (running)
                    emit ready();
            });
        }
    }
    void cancel() override
    {
        ++cancels;
        running = false;
        QTimer::singleShot(0, this, [this] { emit cancelled(QStringLiteral("Cancelled.")); });
    }
    void preview(const QList<int> &codes)
    {
        QVERIFY(running);
        emit progress(codes);
    }
    void finish(const QList<int> &codes)
    {
        QVERIFY(running);
        running = false;
        emit completed(codes);
    }

    bool supported = true;
    bool accessible = true;
    bool includeInaccessibleKeyboard = false;
    bool autoReady = true;
    bool running = false;
    int starts = 0;
    int cancels = 0;
    QString fingerprint = QString(64, QLatin1Char('a'));
    QString startedKeyboard;
    QString startedFingerprint;
};

class FakeShortcutContext final : public ShortcutContext
{
  public:
    void refresh(const KeyboardSelection &keyboard) override
    {
        ++refreshes;
        refreshedKeyboard = keyboard.id;
        QTimer::singleShot(0, this, [this] { emit prepared(true, {}); });
    }
    QString label(const QList<int> &codes) const override { return chordLabel(codes); }
    QStringList conflicts(const QList<int> &codes) const override
    {
        return checked && codes == conflictChord ? QStringList{conflictDescription} : QStringList{};
    }
    bool bindingsAvailable() const override { return checked; }
    QString advisory() const override
    {
        return checked ? QStringLiteral("Application shortcuts may be missing.")
                       : QStringLiteral("Synthetic Hyprland inventory unavailable.");
    }

    bool checked = true;
    int refreshes = 0;
    QString refreshedKeyboard;
};

} // namespace

class RecordingUiTest : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void cleanup();
    void dpadTargetsStayUsableAtBothWindowSizes();
    void selectionWaitsForExplicitRecording();
    void previewsSavedLayersWithoutEditingOrCapture();
    void previewDoesNotChangeActiveProfile();
    void previewsAndSavesOnlyAfterRelease();
    void cancelsWithoutChangingSavedBinding_data();
    void cancelsWithoutChangingSavedBinding();
    void rejectsStaleSaveAndKeepsDraft_data();
    void rejectsStaleSaveAndKeepsDraft();
    void lossCancelsRecording_data();
    void lossCancelsRecording();
    void unavailableCaptureDoesNotRequestKeys_data();
    void unavailableCaptureDoesNotRequestKeys();
    void rejectedKeyboardClearsPreviousSelection();
    void replacementRequiresExplicitReselection_data();
    void replacementRequiresExplicitReselection();
    void choosesHowToResolveConflictWarning_data();
    void choosesHowToResolveConflictWarning();
    void recordsEscapeAndModifierOnly_data();
    void recordsEscapeAndModifierOnly();
    void recordsBothLayersAndSimulatesHolds();
    void switchingLayersDiscardsDraftAndGuardsCapture();
    void thumbRolePreventsRecordingOnBothLayers();

  private:
    void enterEditor();
    void chooseKeyboard();
    void click(const QString &name);
    void verifyDisabled(const QString &name) const;
    QQuickItem *findItem(const QString &name) const;
    QString itemText(const QString &name) const;
    QString tileShortcut(const QString &id) const;
    void verifyStored(const QString &id, const Binding &binding, int layer = 1) const;
    void saveImage(const QString &name);

    std::unique_ptr<QTemporaryDir> directory_;
    QString socketPath_;
    QString profilePath_;
    QProcess service_;
    std::unique_ptr<Controller> controller_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    FakeCaptureBackend *capture_ = nullptr;
    FakeShortcutContext *context_ = nullptr;
    QQuickWindow *window_ = nullptr;
    QStringList warnings_;
};

void RecordingUiTest::init()
{
    warnings_.clear();
    directory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(directory_->isValid());
    socketPath_ = directory_->filePath(QStringLiteral("recording.sock"));
    profilePath_ = directory_->filePath(QStringLiteral("profiles"));
    service_.start(QCoreApplication::applicationDirPath() + QStringLiteral("/xkeysd"),
                   {QStringLiteral("--demo"), QStringLiteral("--socket"), socketPath_,
                    QStringLiteral("--profiles-dir"), profilePath_});
    QVERIFY2(service_.waitForStarted(), qPrintable(service_.errorString()));
    const auto listening = [this] {
        QLocalSocket probe;
        probe.connectToServer(socketPath_);
        return probe.waitForConnected(100);
    };
    QTRY_VERIFY_WITH_TIMEOUT(listening() || service_.state() == QProcess::NotRunning, 5000);
    QVERIFY2(service_.state() == QProcess::Running, service_.readAllStandardError().constData());

    auto capture = std::make_unique<FakeCaptureBackend>();
    auto context = std::make_unique<FakeShortcutContext>();
    capture_ = capture.get();
    context_ = context.get();
    controller_ = std::make_unique<Controller>(std::move(capture), std::move(context));

    engine_ = std::make_unique<QQmlApplicationEngine>();
    connect(engine_.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
        for (const auto &error : errors)
            warnings_.append(error.toString());
    });
    engine_->setInitialProperties(
        {{QStringLiteral("controller"), QVariant::fromValue(controller_.get())}});
    engine_->loadFromModule("XKeys", "Main");
    QVERIFY(!engine_->rootObjects().isEmpty());
    window_ = qobject_cast<QQuickWindow *>(engine_->rootObjects().first());
    QVERIFY(window_ != nullptr);
    QVERIFY(QTest::qWaitForWindowExposed(window_));
    controller_->setWindowActive(true);
    controller_->startDemo(socketPath_, profilePath_);
    QTRY_VERIFY_WITH_TIMEOUT(controller_->connected() && !controller_->pending(), 5000);
    QCOMPARE(capture_->starts, 0);
    QCOMPARE(context_->refreshes, 0);

    controller_->createProfile(QStringLiteral("Recording fixture"));
    QTRY_VERIFY(controller_->hasProfile() && !controller_->pending());
    ProfileStore store(profilePath_);
    const auto loaded = store.load(controller_->selectedProfileId());
    QVERIFY(loaded);
    auto profile = loaded.value();
    profile.layers[0].insert(key01, {QStringLiteral("Previous"), existingChord});
    profile.layers[0].insert(QStringLiteral("dpadLeft"), {{}, ordinaryChord});
    profile.layers[1].insert(key01, {QStringLiteral("Previous"), existingChord});
    profile.layers[1].insert(key03, {QStringLiteral("Layer two"), layerTwoChord});
    QVERIFY(store.save(profile, profile.revision));
    controller_->selectProfile(profile.id);
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->mode(), QStringLiteral("Inactive"));
    QCOMPARE(capture_->starts, 0);
    QCOMPARE(context_->refreshes, 0);
}

void RecordingUiTest::cleanup()
{
    bool editorSettled = true;
    if (controller_) {
        controller_->setWindowActive(false);
        editorSettled = QTest::qWaitFor(
            [this] { return !controller_->pending() && !controller_->recordingBusy(); }, 1000);
        if (editorSettled && controller_->connected()) {
            controller_->stopDemo();
            editorSettled = QTest::qWaitFor([this] { return !controller_->connected(); }, 1000);
        }
    }
    engine_.reset();
    window_ = nullptr;
    controller_.reset();
    capture_ = nullptr;
    context_ = nullptr;
    QByteArray stopError;
    if (service_.state() != QProcess::NotRunning && !service_.waitForFinished(100)) {
        QProcess stop;
        stop.start(service_.program(), {QStringLiteral("--demo"), QStringLiteral("--stop"),
                                        QStringLiteral("--socket"), socketPath_});
        if (!stop.waitForFinished(1500) && stop.state() != QProcess::NotRunning) {
            stop.kill();
            stop.waitForFinished(1000);
        }
        stopError = stop.readAllStandardError();
    }
    if (service_.state() != QProcess::NotRunning && !service_.waitForFinished(1000)) {
        service_.terminate();
        if (!service_.waitForFinished(1000)) {
            service_.kill();
            service_.waitForFinished(1000);
        }
    }
    const auto serviceError = service_.readAllStandardError() + stopError;
    directory_.reset();
    QVERIFY2(service_.state() == QProcess::NotRunning &&
                 service_.exitStatus() == QProcess::NormalExit && service_.exitCode() == 0,
             serviceError.constData());
    QVERIFY2(editorSettled, "The editor did not settle during cleanup.");
    QVERIFY2(warnings_.isEmpty(), qPrintable(warnings_.join(QLatin1Char('\n'))));
}

void RecordingUiTest::enterEditor()
{
    click(QStringLiteral("editProfileButton"));
    QTRY_VERIFY(controller_->editing() && !controller_->pending());
    controller_->setWindowActive(false);
    controller_->setWindowActive(true);
    QCOMPARE(capture_->starts, 0);
    QCOMPARE(context_->refreshes, 0);
}

void RecordingUiTest::chooseKeyboard()
{
    click(QStringLiteral("refreshKeyboardsButton"));
    auto *selector = findItem(QStringLiteral("recordingKeyboardSelector"));
    QVERIFY(selector != nullptr);
    QTRY_VERIFY(selector->isVisible() && selector->isEnabled());
    QTRY_COMPARE(selector->property("count").toInt(), 1);
    // Deliver the ComboBox selection in-process. The fake captures only test signals.
    QVERIFY(selector->setProperty("currentIndex", 0));
    QVERIFY(QMetaObject::invokeMethod(selector, "activated", Q_ARG(int, 0)));
    QTRY_COMPARE(controller_->recordingKeyboardId(), keyboardId);
    QCOMPARE(capture_->starts, 0);
}

QQuickItem *RecordingUiTest::findItem(const QString &name) const
{
    QList<QQuickItem *> pending{window_->contentItem()};
    while (!pending.isEmpty()) {
        auto *item = pending.takeLast();
        if (item->objectName() == name)
            return item;
        pending.append(item->childItems());
    }
    return nullptr;
}

void RecordingUiTest::click(const QString &name)
{
    auto *item = findItem(name);
    QVERIFY2(item != nullptr, qPrintable(name));
    QTRY_VERIFY(item->isVisible() && item->isEnabled() && item->width() > 0 && item->height() > 0);
    // Visible controls can still have their previous geometry until the next layout frame.
    QSignalSpy frame(window_, &QQuickWindow::frameSwapped);
    window_->update();
    QVERIFY(frame.wait());
    const auto position =
        item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, position);
}

void RecordingUiTest::verifyDisabled(const QString &name) const
{
    const auto *item = findItem(name);
    QVERIFY2(item != nullptr && !item->isEnabled(), qPrintable(name));
}

QString RecordingUiTest::itemText(const QString &name) const
{
    const auto *item = findItem(name);
    return item ? item->property("text").toString() : QString{};
}

QString RecordingUiTest::tileShortcut(const QString &id) const
{
    const auto *item = findItem(QStringLiteral("pad-") + id);
    return item ? item->property("savedShortcut").toString() : QString{};
}

void RecordingUiTest::verifyStored(const QString &id, const Binding &binding, int layer) const
{
    const ProfileStore store(profilePath_);
    const auto saved = store.load(controller_->selectedProfileId());
    QVERIFY(saved);
    QCOMPARE(saved.value().layers.at(static_cast<std::size_t>(layer - 1)).value(id), binding);
}

void RecordingUiTest::saveImage(const QString &name)
{
    const auto root = qEnvironmentVariable("XKEYS_SCREENSHOT_DIR");
    if (!root.isEmpty()) {
        QTest::qWait(100);
        const auto image = window_->grabWindow();
        QVERIFY(!image.isNull());
        QVERIFY(image.save(root + QLatin1Char('/') + name));
    }
}

void RecordingUiTest::dpadTargetsStayUsableAtBothWindowSizes()
{
    enterEditor();
    QTRY_COMPARE(tileShortcut(QStringLiteral("dpadLeft")), chordLabel(ordinaryChord));
    const QStringList directions{QStringLiteral("dpadUp"), QStringLiteral("dpadLeft"),
                                 QStringLiteral("dpadRight"), QStringLiteral("dpadDown")};
    for (const auto &size : {window_->size(), window_->minimumSize()}) {
        window_->resize(size);
        QCOMPARE(window_->size(), size);
        QTest::qWait(100);
        const QRectF viewport(0, 0, size.width(), size.height());
        for (const auto &direction : directions) {
            const auto *item = findItem(QStringLiteral("pad-") + direction);
            QVERIFY2(item != nullptr, qPrintable(direction));
            QVERIFY2(item->isVisible() && item->isEnabled(), qPrintable(direction));
            const auto target = item->mapRectToScene(QRectF(0, 0, item->width(), item->height()));
            const auto description = QStringLiteral("%1 is %2 x %3 in a %4 x %5 window")
                                         .arg(direction)
                                         .arg(target.width())
                                         .arg(target.height())
                                         .arg(size.width())
                                         .arg(size.height());
            QVERIFY2(target.width() >= 28 && target.height() >= 28, qPrintable(description));
            QVERIFY2(viewport.contains(target), qPrintable(description));
        }
    }
}

void RecordingUiTest::previewsSavedLayersWithoutEditingOrCapture()
{
    // init selected a persisted profile without entering Edit or choosing a keyboard.
    const auto id = controller_->selectedProfileId();
    const auto saved = ProfileStore(profilePath_).load(id);
    QVERIFY(saved);
    QVERIFY(controller_->previewing());
    QVERIFY(!controller_->editing());
    QCOMPARE(controller_->mode(), QStringLiteral("Inactive"));
    QCOMPARE(controller_->recordingState(), QStringLiteral("Idle"));
    QCOMPARE(tileShortcut(key01), chordLabel(existingChord));
    QCOMPARE(tileShortcut(key03), QStringLiteral("Blank"));
    QVERIFY(itemText(QStringLiteral("profileGuidance")).contains(QStringLiteral("read only")));
    verifyDisabled(QStringLiteral("pad-key01"));
    QVERIFY(!findItem(QStringLiteral("bindingEditor"))->isVisible());
    QVERIFY(!findItem(QStringLiteral("thumbLayerCheckbox"))->isVisible());
    QVERIFY(!findItem(QStringLiteral("testPopup"))->isVisible());
    saveImage(QStringLiteral("profile-preview-layer-one.png"));

    click(QStringLiteral("editLayerTwoButton"));
    QCOMPARE(controller_->editLayer(), 2);
    QCOMPARE(tileShortcut(key03), chordLabel(layerTwoChord));
    QVERIFY(itemText(QStringLiteral("profileGuidance")).contains(QStringLiteral("Layer 2")));
    window_->resize(window_->minimumSize());
    saveImage(QStringLiteral("profile-preview-layer-two-small.png"));
    controller_->beginEdit(key03, 2);
    controller_->recordAgain();
    controller_->clearBinding();
    controller_->setThumbLayer(true);
    controller_->selectControl(key03);
    controller_->simulate(key03, true);
    QVERIFY(!controller_->pending());
    QVERIFY(!controller_->editing());
    QVERIFY(controller_->activeProfileId().isEmpty());
    QVERIFY(controller_->heldControls().isEmpty());
    QCOMPARE(controller_->heldOutputLabel(), QStringLiteral("None"));
    QCOMPARE(capture_->starts, 0);
    QCOMPARE(context_->refreshes, 0);
    QCOMPARE(controller_->recordingRemainingSeconds(), 0);
    const auto unchanged = ProfileStore(profilePath_).load(id);
    QVERIFY(unchanged);
    QCOMPARE(unchanged.value(), saved.value());

    click(QStringLiteral("editLayerOneButton"));
    QCOMPARE(tileShortcut(key01), chordLabel(existingChord));
    click(QStringLiteral("editProfileButton"));
    QTRY_VERIFY(!controller_->pending() && controller_->editing());
    QVERIFY(!controller_->previewing());
    QVERIFY(findItem(QStringLiteral("bindingEditor"))->isVisible());
    QCOMPARE(capture_->starts, 0);
    QVERIFY(controller_->activeProfileId().isEmpty());

    // A disconnected editor must not continue to present cached labels as current.
    controller_->stopDemo();
    QTRY_VERIFY(!controller_->connected());
    QVERIFY(!controller_->previewing());
    QVERIFY(controller_->controlLabels().isEmpty());
}

void RecordingUiTest::previewDoesNotChangeActiveProfile()
{
    const auto firstId = controller_->selectedProfileId();
    controller_->createProfile(QStringLiteral("Other profile"));
    QTRY_VERIFY(!controller_->pending());
    const auto secondId = controller_->selectedProfileId();
    ProfileStore store(profilePath_);
    const auto second = store.load(secondId);
    QVERIFY(second);
    auto profile = second.value();
    profile.layers[0].insert(key01, {{}, layerTwoChord});
    QVERIFY(store.save(profile, profile.revision));
    controller_->selectProfile(firstId);
    QTRY_VERIFY(!controller_->pending());
    click(QStringLiteral("activateButton"));
    QTRY_VERIFY(!controller_->pending() && controller_->mode() == QStringLiteral("Active"));
    QVERIFY(!controller_->previewing());
    controller_->simulate(key01, true);
    QTRY_VERIFY(!controller_->pending());
    const auto held = controller_->heldControls();
    QCOMPARE(controller_->heldOutputLabel(), chordLabel(existingChord));

    controller_->selectProfile(secondId);
    QTRY_VERIFY(!controller_->pending());
    QVERIFY(controller_->previewing());
    QCOMPARE(controller_->activeProfileId(), firstId);
    QCOMPARE(controller_->mode(), QStringLiteral("Active"));
    QCOMPARE(tileShortcut(key01), chordLabel(layerTwoChord));
    QCOMPARE(controller_->heldControls(), held);
    QVERIFY(itemText(QStringLiteral("activeProfileBanner"))
                .contains(QStringLiteral("Recording fixture")));
    QVERIFY(itemText(QStringLiteral("profileGuidance")).contains(QStringLiteral("Other profile")));
    QVERIFY(findItem(QStringLiteral("keypad"))->property("heldControls").toStringList().isEmpty());
    verifyDisabled(QStringLiteral("pad-key01"));
    QVERIFY(!findItem(QStringLiteral("holdMode"))->isVisible());
    QVERIFY(!findItem(QStringLiteral("testPopup"))->isVisible());
    saveImage(QStringLiteral("profile-preview-with-another-active.png"));
    click(QStringLiteral("editLayerTwoButton"));
    QCOMPARE(tileShortcut(key01), QStringLiteral("Blank"));
    controller_->simulate(key03, true);
    controller_->recordAgain();
    QVERIFY(!controller_->pending());
    QCOMPARE(controller_->heldControls(), held);
    QCOMPARE(controller_->heldOutputLabel(), chordLabel(existingChord));
    QCOMPARE(controller_->activeProfileId(), firstId);
    QCOMPARE(capture_->starts, 0);
    QCOMPARE(context_->refreshes, 0);

    controller_->selectProfile(firstId);
    QTRY_VERIFY(!controller_->pending());
    QVERIFY(!controller_->previewing());
    QCOMPARE(tileShortcut(key01), chordLabel(existingChord));
    QCOMPARE(controller_->heldControls(), held);

    // A newer saved revision is a preview, even when its ID matches the active profile.
    const auto first = store.load(firstId);
    QVERIFY(first);
    profile = first.value();
    profile.layers[0].insert(key01, {{}, ordinaryChord});
    QVERIFY(store.save(profile, profile.revision));
    controller_->selectProfile(firstId);
    QTRY_VERIFY(!controller_->pending());
    QVERIFY(controller_->previewing());
    QCOMPARE(tileShortcut(key01), chordLabel(ordinaryChord));
    QCOMPARE(controller_->heldOutputLabel(), chordLabel(existingChord));
    controller_->simulate(key01, true);
    QVERIFY(!controller_->pending());
    QCOMPARE(controller_->heldControls(), held);
    controller_->releaseAll();
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->heldOutputLabel(), QStringLiteral("None"));
}

void RecordingUiTest::selectionWaitsForExplicitRecording()
{
    enterEditor();
    click(QStringLiteral("pad-key01"));
    QCOMPARE(controller_->selectedControl(), key01);
    QCOMPARE(controller_->recordingState(), QStringLiteral("Idle"));
    verifyDisabled(QStringLiteral("recordAgainButton"));
    chooseKeyboard();
    QCOMPARE(itemText(QStringLiteral("recordAgainButton")), QStringLiteral("Start recording"));
    click(QStringLiteral("pad-key03"));
    click(QStringLiteral("pad-key01"));
    QCOMPARE(controller_->selectedControl(), key01);
    QCOMPARE(controller_->recordingState(), QStringLiteral("Idle"));
    QCOMPARE(controller_->recordingRemainingSeconds(), 0);
    QCOMPARE(capture_->starts, 0);
    QCOMPARE(context_->refreshes, 0);
    QVERIFY(!capture_->running);
    verifyStored(key01, {QStringLiteral("Previous"), existingChord});
    saveImage(QStringLiteral("recording-selected-idle.png"));

    // Clearing a selected binding must not acquire the keyboard either.
    click(QStringLiteral("clearBindingButton"));
    QTRY_VERIFY(!controller_->pending());
    verifyStored(key01, {});
    QCOMPARE(capture_->starts, 0);
    QCOMPARE(context_->refreshes, 0);
    click(QStringLiteral("recordAgainButton"));
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Recording"));
    QCOMPARE(capture_->starts, 1);
    QVERIFY(controller_->recordingRemainingSeconds() > 0);
    click(QStringLiteral("cancelRecordingButton"));
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Idle"));
    QCOMPARE(controller_->recordingRemainingSeconds(), 0);
    QCOMPARE(capture_->starts, 1);
    QCOMPARE(itemText(QStringLiteral("recordAgainButton")), QStringLiteral("Start recording"));
}

void RecordingUiTest::previewsAndSavesOnlyAfterRelease()
{
    enterEditor();
    chooseKeyboard();
    const auto blankTile = tileShortcut(key03);
    click(QStringLiteral("pad-key03"));
    click(QStringLiteral("recordAgainButton"));
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Recording"));
    QCOMPARE(capture_->starts, 1);
    QCOMPARE(context_->refreshes, 1);
    QCOMPARE(capture_->startedKeyboard, keyboardId);
    QCOMPARE(context_->refreshedKeyboard, keyboardId);
    QCOMPARE(controller_->selectedControl(), key03);
    QCOMPARE(controller_->selectedLayer(), 1);
    QVERIFY(controller_->recordingBusy());
    QVERIFY(itemText(QStringLiteral("recordingStatus"))
                .contains(QStringLiteral("press"), Qt::CaseInsensitive));

    capture_->preview({KEY_LEFTCTRL, KEY_LEFTALT});
    QTRY_COMPARE(controller_->draftChord(), chordLabel({KEY_LEFTCTRL, KEY_LEFTALT}));
    capture_->preview(ordinaryChord);
    QTRY_VERIFY(itemText(QStringLiteral("recordedChord")).contains(chordLabel(ordinaryChord)));
    QVERIFY(itemText(QStringLiteral("savedChord")).contains(QStringLiteral("Blank")));
    QCOMPARE(controller_->bindingLabel(), QStringLiteral("Blank"));
    QCOMPARE(tileShortcut(key03), blankTile);
    QVERIFY(!controller_->canSaveDraft());
    verifyDisabled(QStringLiteral("saveBindingButton"));
    verifyDisabled(QStringLiteral("profileSelector"));
    verifyDisabled(QStringLiteral("activateButton"));
    controller_->saveDraft();
    QVERIFY(!controller_->pending());
    verifyStored(key03, {});

    capture_->finish(ordinaryChord);
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Draft"));
    QVERIFY(controller_->canSaveDraft());
    QVERIFY(!controller_->recordingBusy());
    QCOMPARE(tileShortcut(key03), blankTile);
    verifyDisabled(QStringLiteral("activateButton"));
    verifyDisabled(QStringLiteral("profileSelector"));
    verifyStored(key03, {});
    saveImage(QStringLiteral("recording-draft.png"));

    auto *label = findItem(QStringLiteral("draftActionLabel"));
    QVERIFY(label != nullptr);
    QVERIFY(label->isVisible() && label->isEnabled());
    const auto action = QStringLiteral("Focus search");
    QVERIFY(label->setProperty("text", action));
    QVERIFY(QMetaObject::invokeMethod(label, "textEdited"));
    QTRY_COMPARE(controller_->draftActionLabel(), action);
    click(QStringLiteral("saveBindingButton"));
    QTRY_VERIFY(!controller_->pending() && controller_->recordingState() == QStringLiteral("Idle"));
    QCOMPARE(controller_->mode(), QStringLiteral("Edit"));
    QVERIFY(controller_->activeProfileId().isEmpty());
    QTRY_COMPARE(tileShortcut(key03), chordLabel(ordinaryChord));
    QCOMPARE(capture_->starts, 1);
    verifyStored(key03, {action, ordinaryChord});
    verifyStored(key03, {QStringLiteral("Layer two"), layerTwoChord}, 2);
    saveImage(QStringLiteral("recording-saved.png"));
}

void RecordingUiTest::cancelsWithoutChangingSavedBinding_data()
{
    QTest::addColumn<QString>("phase");
    QTest::addColumn<int>("layer");
    for (int layer : {1, 2}) {
        QTest::newRow(qPrintable(QStringLiteral("before ready layer %1").arg(layer)))
            << QStringLiteral("Preparing") << layer;
        QTest::newRow(qPrintable(QStringLiteral("with keys held layer %1").arg(layer)))
            << QStringLiteral("Recording") << layer;
        QTest::newRow(qPrintable(QStringLiteral("after release layer %1").arg(layer)))
            << QStringLiteral("Draft") << layer;
    }
}

void RecordingUiTest::cancelsWithoutChangingSavedBinding()
{
    QFETCH(int, layer);
    QFETCH(QString, phase);
    enterEditor();
    click(layer == 1 ? QStringLiteral("editLayerOneButton") : QStringLiteral("editLayerTwoButton"));
    chooseKeyboard();
    capture_->autoReady = phase != QStringLiteral("Preparing");
    click(QStringLiteral("pad-key01"));
    click(QStringLiteral("recordAgainButton"));
    QTRY_COMPARE(capture_->starts, 1);
    if (capture_->autoReady) {
        QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Recording"));
        capture_->preview(ordinaryChord);
        if (phase == QStringLiteral("Draft"))
            capture_->finish(ordinaryChord);
    }
    QTRY_COMPARE(controller_->recordingState(), phase);
    if (!capture_->autoReady)
        QVERIFY(!itemText(QStringLiteral("recordingStatus"))
                     .contains(QStringLiteral("Press your shortcut")));
    QCOMPARE(tileShortcut(key01), chordLabel(existingChord));

    click(QStringLiteral("cancelRecordingButton"));
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Idle"));
    QCOMPARE(capture_->cancels, phase == QStringLiteral("Draft") ? 0 : 1);
    QVERIFY(!capture_->running);
    QVERIFY(!controller_->recordingBusy());
    QVERIFY(!controller_->canSaveDraft());
    QVERIFY(controller_->draftChord().isEmpty());
    QCOMPARE(tileShortcut(key01), chordLabel(existingChord));
    verifyStored(key01, {QStringLiteral("Previous"), existingChord}, layer);
}

void RecordingUiTest::rejectsStaleSaveAndKeepsDraft_data()
{
    QTest::addColumn<int>("layer");
    QTest::newRow("layer one") << 1;
    QTest::newRow("layer two") << 2;
}

void RecordingUiTest::rejectsStaleSaveAndKeepsDraft()
{
    QFETCH(int, layer);
    enterEditor();
    click(layer == 1 ? QStringLiteral("editLayerOneButton") : QStringLiteral("editLayerTwoButton"));
    chooseKeyboard();
    click(QStringLiteral("pad-key01"));
    click(QStringLiteral("recordAgainButton"));
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Recording"));
    capture_->finish(ordinaryChord);
    QTRY_VERIFY(controller_->canSaveDraft());

    ProfileStore external(profilePath_);
    const auto old = external.load(controller_->selectedProfileId());
    QVERIFY(old);
    const auto newRevision = external.save(old.value(), old.value().revision);
    QVERIFY(newRevision);

    click(QStringLiteral("saveBindingButton"));
    QTRY_VERIFY(!controller_->pending() && !controller_->errorText().isEmpty());
    QCOMPARE(controller_->recordingState(), QStringLiteral("Draft"));
    QCOMPARE(controller_->draftChord(), chordLabel(ordinaryChord));
    QVERIFY(itemText(QStringLiteral("recordedChord")).contains(chordLabel(ordinaryChord)));
    QCOMPARE(tileShortcut(key01), chordLabel(existingChord));
    verifyStored(key01, {QStringLiteral("Previous"), existingChord}, layer);
    const auto latest = external.load(controller_->selectedProfileId());
    QVERIFY(latest);
    QCOMPARE(latest.value().revision, newRevision.value());
    QVERIFY(controller_->activeProfileId().isEmpty());
}

void RecordingUiTest::lossCancelsRecording_data()
{
    QTest::addColumn<bool>("disconnect");
    QTest::addColumn<bool>("released");
    QTest::addColumn<int>("layer");
    for (int layer : {1, 2}) {
        QTest::newRow(qPrintable(QStringLiteral("focus loss during capture layer %1").arg(layer)))
            << false << false << layer;
        QTest::newRow(qPrintable(QStringLiteral("focus loss with draft layer %1").arg(layer)))
            << false << true << layer;
        QTest::newRow(qPrintable(QStringLiteral("service loss during capture layer %1").arg(layer)))
            << true << false << layer;
    }
}

void RecordingUiTest::lossCancelsRecording()
{
    QFETCH(int, layer);
    QFETCH(bool, disconnect);
    QFETCH(bool, released);
    enterEditor();
    click(layer == 1 ? QStringLiteral("editLayerOneButton") : QStringLiteral("editLayerTwoButton"));
    chooseKeyboard();
    click(QStringLiteral("pad-key01"));
    click(QStringLiteral("recordAgainButton"));
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Recording"));
    capture_->preview(ordinaryChord);
    if (released) {
        capture_->finish(ordinaryChord);
        QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Draft"));
    }
    QVERIFY(!controller_->draftChord().isEmpty());
    if (disconnect) {
        Controller peer;
        peer.startDemo(socketPath_, profilePath_);
        QTRY_VERIFY(peer.connected() && !peer.pending());
        peer.stopDemo();
        QTRY_VERIFY(!controller_->connected());
        QTRY_COMPARE(service_.state(), QProcess::NotRunning);
    } else {
        controller_->setWindowActive(false);
    }

    QTRY_VERIFY(!controller_->recordingBusy());
    QTRY_VERIFY(controller_->draftChord().isEmpty());
    QCOMPARE(capture_->cancels, released ? 0 : 1);
    QVERIFY(!capture_->running);
    QVERIFY(!controller_->canSaveDraft());
    if (!disconnect) {
        controller_->setWindowActive(true);
        QCoreApplication::processEvents();
        QCOMPARE(capture_->starts, 1);
        QCOMPARE(tileShortcut(key01), chordLabel(existingChord));
    }
    const auto saved = ProfileStore(profilePath_).list();
    QVERIFY(saved);
    QCOMPARE(saved.value().size(), 1);
    QCOMPARE(saved.value()
                 .constFirst()
                 .layers.at(static_cast<std::size_t>(layer - 1))
                 .value(key01)
                 .chord,
             existingChord);
}

void RecordingUiTest::unavailableCaptureDoesNotRequestKeys_data()
{
    QTest::addColumn<bool>("supported");
    QTest::newRow("unsupported backend") << false;
    QTest::newRow("inaccessible keyboard") << true;
}

void RecordingUiTest::unavailableCaptureDoesNotRequestKeys()
{
    QFETCH(bool, supported);
    capture_->supported = supported;
    capture_->accessible = false;
    enterEditor();
    controller_->refreshRecordingKeyboards();
    if (supported) {
        QCOMPARE(controller_->recordingKeyboards().size(), 1);
        QVERIFY(
            !controller_->recordingKeyboards().constFirst().toMap().value("accessible").toBool());
        controller_->setRecordingKeyboard(keyboardId);
    } else {
        QVERIFY(!controller_->recordingAvailable());
        QVERIFY(!controller_->recordingUnavailableReason().isEmpty());
    }
    QVERIFY(controller_->recordingKeyboardId().isEmpty());
    click(QStringLiteral("pad-key03"));
    QTRY_COMPARE(controller_->selectedControl(), key03);
    QTRY_VERIFY(!itemText(QStringLiteral("recordingStatus")).isEmpty());
    QVERIFY(!itemText(QStringLiteral("recordingStatus"))
                 .contains(QStringLiteral("press"), Qt::CaseInsensitive));
    QVERIFY(!controller_->recordingBusy());
    QVERIFY(!controller_->canSaveDraft());
    QCOMPARE(capture_->starts, 0);
    QCOMPARE(context_->refreshes, 0);
    verifyStored(key03, {});
}

void RecordingUiTest::rejectedKeyboardClearsPreviousSelection()
{
    enterEditor();
    chooseKeyboard();
    capture_->includeInaccessibleKeyboard = true;
    click(QStringLiteral("refreshKeyboardsButton"));
    QCOMPARE(controller_->recordingKeyboardId(), keyboardId);
    auto *selector = findItem(QStringLiteral("recordingKeyboardSelector"));
    QVERIFY(selector != nullptr);
    QTRY_COMPARE(selector->property("count").toInt(), 2);
    QTRY_COMPARE(selector->property("displayText").toString(), keyboardName);

    // Exercise the Controller guard through the ComboBox activation handler, as if
    // an activation raced with the delegate becoming unavailable.
    QVERIFY(selector->setProperty("currentIndex", 1));
    QVERIFY(QMetaObject::invokeMethod(selector, "activated", Q_ARG(int, 1)));
    QCOMPARE(controller_->recordingState(), QStringLiteral("Error"));
    QVERIFY(controller_->recordingMessage().contains(QStringLiteral("Synthetic access denied")));
    QTRY_VERIFY(controller_->recordingKeyboardId().isEmpty());
    QTRY_COMPARE(selector->property("displayText").toString(),
                 QStringLiteral("Choose recording device"));

    click(QStringLiteral("pad-key03"));
    QTRY_COMPARE(controller_->selectedControl(), key03);
    QVERIFY(!controller_->recordingBusy());
    QVERIFY(!controller_->canSaveDraft());
    QCOMPARE(context_->refreshes, 0);
    QCOMPARE(capture_->starts, 0);
    verifyStored(key03, {});
}

void RecordingUiTest::replacementRequiresExplicitReselection_data()
{
    QTest::addColumn<bool>("refreshBeforeClick");
    QTest::newRow("identity changed before capture") << false;
    QTest::newRow("identity changed before refresh") << true;
}

void RecordingUiTest::replacementRequiresExplicitReselection()
{
    QFETCH(bool, refreshBeforeClick);
    enterEditor();
    chooseKeyboard();
    capture_->fingerprint = QString(64, QLatin1Char('b'));
    if (refreshBeforeClick) {
        controller_->refreshRecordingKeyboards();
        QVERIFY(controller_->recordingKeyboardId().isEmpty());
        QTRY_VERIFY(
            itemText(QStringLiteral("recordingStatus")).contains(QStringLiteral("changed")));
    }
    click(QStringLiteral("pad-key03"));
    QCOMPARE(controller_->recordingState(), QStringLiteral("Idle"));
    if (refreshBeforeClick)
        verifyDisabled(QStringLiteral("recordAgainButton"));
    // Also exercise the controller guard if an explicit start races with inventory loss.
    controller_->recordAgain();
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Error"));
    if (!refreshBeforeClick)
        QVERIFY(controller_->recordingMessage().contains(QStringLiteral("changed")));
    QVERIFY(controller_->recordingKeyboardId().isEmpty());
    QVERIFY(!controller_->canSaveDraft());
    QCOMPARE(context_->refreshes, 0);
    QCOMPARE(capture_->starts, 0);
    verifyStored(key03, {});

    chooseKeyboard();
    click(QStringLiteral("pad-key03"));
    click(QStringLiteral("recordAgainButton"));
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Recording"));
    QCOMPARE(capture_->starts, 1);
    QCOMPARE(capture_->startedKeyboard, keyboardId);
    QCOMPARE(capture_->startedFingerprint, capture_->fingerprint);
    click(QStringLiteral("cancelRecordingButton"));
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Idle"));
}

void RecordingUiTest::choosesHowToResolveConflictWarning_data()
{
    QTest::addColumn<bool>("checked");
    QTest::addColumn<bool>("recordDifferent");
    QTest::addColumn<int>("layer");
    for (int layer : {1, 2}) {
        QTest::newRow(
            qPrintable(QStringLiteral("use known overlapping binding layer %1").arg(layer)))
            << true << false << layer;
        QTest::newRow(
            qPrintable(QStringLiteral("use binding without an inventory layer %1").arg(layer)))
            << false << false << layer;
        QTest::newRow(
            qPrintable(QStringLiteral("retry known overlapping binding layer %1").arg(layer)))
            << true << true << layer;
        QTest::newRow(
            qPrintable(QStringLiteral("retry binding without an inventory layer %1").arg(layer)))
            << false << true << layer;
    }
}

void RecordingUiTest::choosesHowToResolveConflictWarning()
{
    QFETCH(int, layer);
    QFETCH(bool, checked);
    QFETCH(bool, recordDifferent);
    context_->checked = checked;
    enterEditor();
    click(layer == 1 ? QStringLiteral("editLayerOneButton") : QStringLiteral("editLayerTwoButton"));
    chooseKeyboard();
    click(QStringLiteral("pad-key01"));
    click(QStringLiteral("recordAgainButton"));
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Recording"));
    capture_->finish(conflictChord);
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Draft"));
    QCOMPARE(controller_->conflictsChecked(), checked);
    if (checked)
        QCOMPARE(controller_->bindingConflicts(), QStringList{conflictDescription});
    else
        QVERIFY(!controller_->conflictCheckNote().isEmpty());
    QVERIFY(!controller_->conflictAcknowledged());
    QVERIFY(!controller_->canSaveDraft());
    if (!recordDifferent && !qEnvironmentVariableIsEmpty("XKEYS_SCREENSHOT_DIR")) {
        const auto originalSize = window_->size();
        saveImage(checked ? QStringLiteral("recording-conflict.png")
                          : QStringLiteral("recording-incomplete-check.png"));
        window_->resize(window_->minimumSize());
        saveImage(checked ? QStringLiteral("recording-conflict-small.png")
                          : QStringLiteral("recording-incomplete-check-small.png"));
        window_->resize(originalSize);
    }
    verifyDisabled(QStringLiteral("saveBindingButton"));
    const auto *ordinarySave = findItem(QStringLiteral("saveBindingButton"));
    QVERIFY(ordinarySave != nullptr);
    QVERIFY(!ordinarySave->isVisible());
    controller_->saveDraft();
    QVERIFY(!controller_->pending());
    const auto previousLabel = QStringLiteral("Previous");
    verifyStored(key01, {previousLabel, existingChord}, layer);

    if (recordDifferent) {
        click(QStringLiteral("recordDifferentShortcutButton"));
        QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Recording"));
        QCOMPARE(capture_->starts, 2);
        QVERIFY(controller_->draftChord().isEmpty());
        QCOMPARE(tileShortcut(key01), chordLabel(existingChord));
        verifyStored(key01, {previousLabel, existingChord}, layer);

        context_->checked = true;
        capture_->finish(ordinaryChord);
        QTRY_VERIFY(controller_->canSaveDraft());
        QCOMPARE(tileShortcut(key01), chordLabel(existingChord));
        verifyStored(key01, {previousLabel, existingChord}, layer);
        click(QStringLiteral("saveBindingButton"));
    } else {
        click(QStringLiteral("useConflictBindingButton"));
    }
    QTRY_VERIFY(!controller_->pending() && controller_->recordingState() == QStringLiteral("Idle"));
    const auto &chosenChord = recordDifferent ? ordinaryChord : conflictChord;
    QTRY_COMPARE(tileShortcut(key01), chordLabel(chosenChord));
    verifyStored(key01, {previousLabel, chosenChord}, layer);
    verifyStored(key01, {previousLabel, existingChord}, 3 - layer);
    QVERIFY(controller_->activeProfileId().isEmpty());
}

void RecordingUiTest::recordsEscapeAndModifierOnly_data()
{
    QTest::addColumn<QList<int>>("codes");
    QTest::newRow("Escape") << QList<int>{KEY_ESC};
    QTest::newRow("modifier only") << QList<int>{KEY_RIGHTCTRL};
}

void RecordingUiTest::recordsEscapeAndModifierOnly()
{
    QFETCH(QList<int>, codes);
    enterEditor();
    chooseKeyboard();
    click(QStringLiteral("pad-key03"));
    click(QStringLiteral("recordAgainButton"));
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Recording"));
    capture_->finish(codes);
    QTRY_VERIFY(controller_->canSaveDraft());
    QTRY_VERIFY(itemText(QStringLiteral("recordedChord")).contains(chordLabel(codes)));
    click(QStringLiteral("saveBindingButton"));
    QTRY_VERIFY(!controller_->pending() && controller_->recordingState() == QStringLiteral("Idle"));
    verifyStored(key03, {{}, codes});
}

void RecordingUiTest::recordsBothLayersAndSimulatesHolds()
{
    enterEditor();
    chooseKeyboard();
    click(QStringLiteral("pad-key01"));
    click(QStringLiteral("editLayerTwoButton"));
    QCOMPARE(controller_->editLayer(), 2);
    QCOMPARE(controller_->selectedControl(), key01);
    QVERIFY(itemText(QStringLiteral("recordingTarget")).contains(QStringLiteral("Layer 2")));
    QCOMPARE(tileShortcut(key03), chordLabel(layerTwoChord));
    QCOMPARE(controller_->recordingState(), QStringLiteral("Idle"));
    QCOMPARE(capture_->starts, 0);
    QCOMPARE(context_->refreshes, 0);
    click(QStringLiteral("recordAgainButton"));
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Recording"));
    capture_->preview(ordinaryChord);
    QCOMPARE(tileShortcut(key01), chordLabel(existingChord));
    QVERIFY(!controller_->canSaveDraft());
    capture_->finish(ordinaryChord);
    QTRY_VERIFY(controller_->canSaveDraft());
    saveImage(QStringLiteral("layer-two-draft.png"));
    const auto originalSize = window_->size();
    window_->resize(window_->minimumSize());
    saveImage(QStringLiteral("layer-two-draft-small.png"));
    window_->resize(originalSize);
    click(QStringLiteral("saveBindingButton"));
    QCOMPARE(controller_->recordingState(), QStringLiteral("Saving"));
    verifyDisabled(QStringLiteral("editLayerOneButton"));
    controller_->setEditLayer(1);
    QCOMPARE(controller_->editLayer(), 2);
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->recordingState(), QStringLiteral("Idle"));
    QVERIFY(controller_->activeProfileId().isEmpty());
    verifyStored(key01, {QStringLiteral("Previous"), ordinaryChord}, 2);
    verifyStored(key01, {QStringLiteral("Previous"), existingChord});
    QCOMPARE(tileShortcut(key01), chordLabel(ordinaryChord));

    // Reload from the service to prove the second-layer write survived serialization.
    const auto id = controller_->selectedProfileId();
    controller_->selectProfile(id);
    QTRY_VERIFY(!controller_->pending());
    controller_->editSelected();
    QTRY_VERIFY(!controller_->pending() && controller_->editing());
    click(QStringLiteral("editLayerTwoButton"));
    QCOMPARE(tileShortcut(key01), chordLabel(ordinaryChord));
    click(QStringLiteral("editLayerOneButton"));
    QCOMPARE(tileShortcut(key01), chordLabel(existingChord));
    click(QStringLiteral("editLayerTwoButton"));
    click(QStringLiteral("pad-key03"));
    click(QStringLiteral("clearBindingButton"));
    QTRY_VERIFY(!controller_->pending());
    verifyStored(key03, {}, 2);
    verifyStored(key03, {});
    QCOMPARE(capture_->starts, 1);
    click(QStringLiteral("thumbLayerCheckbox"));
    QTRY_VERIFY(!controller_->pending() && controller_->thumbLayer());
    QCOMPARE(tileShortcut(QStringLiteral("thumb")), QStringLiteral("Layer 2"));
    saveImage(QStringLiteral("layer-two-saved.png"));

    click(QStringLiteral("activateButton"));
    QTRY_VERIFY(!controller_->pending() && controller_->mode() == QStringLiteral("Active"));
    click(QStringLiteral("holdMode"));
    for (bool thumbFirst : {true, false}) {
        click(QStringLiteral("pad-thumb"));
        QTRY_VERIFY(!controller_->pending() && controller_->activeLayer() == 2);
        QCOMPARE(tileShortcut(key01), chordLabel(ordinaryChord));
        click(QStringLiteral("pad-key01"));
        QTRY_VERIFY(!controller_->pending());
        QCOMPARE(controller_->bindingLabel(), chordLabel(ordinaryChord));
        QCOMPARE(controller_->heldOutputLabel(), chordLabel(ordinaryChord));
        click(thumbFirst ? QStringLiteral("pad-thumb") : QStringLiteral("pad-key01"));
        QTRY_VERIFY(!controller_->pending());
        QCOMPARE(controller_->heldOutputLabel(),
                 thumbFirst ? chordLabel(ordinaryChord) : QStringLiteral("None"));
        click(thumbFirst ? QStringLiteral("pad-key01") : QStringLiteral("pad-thumb"));
        QTRY_VERIFY(!controller_->pending() && controller_->activeLayer() == 1);
        QCOMPARE(controller_->heldOutputLabel(), QStringLiteral("None"));
        QVERIFY(controller_->heldControls().isEmpty());
        QCOMPARE(tileShortcut(key01), chordLabel(existingChord));
    }

    controller_->editSelected();
    QTRY_VERIFY(!controller_->pending() && controller_->editing());
    click(QStringLiteral("editLayerTwoButton"));
    click(QStringLiteral("pad-key01"));
    click(QStringLiteral("clearBindingButton"));
    QTRY_VERIFY(!controller_->pending());
    verifyStored(key01, {}, 2);
    verifyStored(key01, {QStringLiteral("Previous"), existingChord});
    QCOMPARE(tileShortcut(key01), QStringLiteral("Blank"));
    click(QStringLiteral("editLayerOneButton"));
    QCOMPARE(tileShortcut(key01), chordLabel(existingChord));
    QCOMPARE(capture_->starts, 1);
}

void RecordingUiTest::switchingLayersDiscardsDraftAndGuardsCapture()
{
    enterEditor();
    chooseKeyboard();
    click(QStringLiteral("pad-key01"));
    for (int layer : {1, 2}) {
        controller_->setEditLayer(layer);
        capture_->autoReady = false;
        click(QStringLiteral("recordAgainButton"));
        QTRY_COMPARE(capture_->starts, layer);
        QCOMPARE(controller_->recordingState(), QStringLiteral("Preparing"));
        verifyDisabled(QStringLiteral("editLayerOneButton"));
        verifyDisabled(QStringLiteral("editLayerTwoButton"));
        controller_->setEditLayer(3 - layer);
        controller_->beginEdit(key03, 3 - layer);
        QCOMPARE(controller_->editLayer(), layer);
        QCOMPARE(controller_->selectedControl(), key01);
        emit capture_->ready();
        QCOMPARE(controller_->recordingState(), QStringLiteral("Recording"));
        controller_->setEditLayer(3 - layer);
        QCOMPARE(controller_->editLayer(), layer);
        capture_->finish(conflictChord);
        QCOMPARE(controller_->recordingState(), QStringLiteral("Draft"));
        controller_->setConflictAcknowledged(true);
        QVERIFY(controller_->canSaveDraft());
        click(layer == 1 ? QStringLiteral("editLayerTwoButton")
                         : QStringLiteral("editLayerOneButton"));
        QCOMPARE(controller_->editLayer(), 3 - layer);
        QCOMPARE(controller_->recordingState(), QStringLiteral("Idle"));
        QVERIFY(controller_->draftChord().isEmpty());
        QVERIFY(controller_->bindingConflicts().isEmpty());
        QVERIFY(!controller_->conflictAcknowledged());
        controller_->saveDraft();
        QVERIFY(!controller_->pending());
        verifyStored(key01, {QStringLiteral("Previous"), existingChord}, 1);
        verifyStored(key01, {QStringLiteral("Previous"), existingChord}, 2);
        QCOMPARE(capture_->starts, layer);
    }
    click(QStringLiteral("recordAgainButton"));
    QTRY_COMPARE(capture_->starts, 3);
    controller_->cancelDraft();
    QCOMPARE(controller_->recordingState(), QStringLiteral("Stopping"));
    controller_->setEditLayer(2);
    QCOMPARE(controller_->editLayer(), 1);
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Idle"));
    controller_->setEditLayer(0);
    controller_->setEditLayer(3);
    QCOMPARE(controller_->editLayer(), 1);
}

void RecordingUiTest::thumbRolePreventsRecordingOnBothLayers()
{
    enterEditor();
    chooseKeyboard();
    click(QStringLiteral("thumbLayerCheckbox"));
    QTRY_VERIFY(!controller_->pending() && controller_->thumbLayer());
    click(QStringLiteral("pad-thumb"));
    for (int layer : {1, 2}) {
        controller_->setEditLayer(layer);
        QCOMPARE(tileShortcut(QStringLiteral("thumb")), QStringLiteral("Layer 2"));
        verifyDisabled(QStringLiteral("recordAgainButton"));
        controller_->recordAgain();
        QCOMPARE(capture_->starts, 0);
        click(QStringLiteral("clearBindingButton"));
        QTRY_VERIFY(!controller_->pending());
        QVERIFY(controller_->thumbLayer());
        verifyStored(QStringLiteral("thumb"), {}, layer);
    }
    click(QStringLiteral("thumbLayerCheckbox"));
    QTRY_VERIFY(!controller_->pending() && !controller_->thumbLayer());
    click(QStringLiteral("recordAgainButton"));
    QTRY_COMPARE(controller_->recordingState(), QStringLiteral("Recording"));
    capture_->finish(ordinaryChord);
    click(QStringLiteral("saveBindingButton"));
    QTRY_VERIFY(!controller_->pending());
    verifyStored(QStringLiteral("thumb"), {{}, ordinaryChord}, 2);
    verifyStored(QStringLiteral("thumb"), {});

    // A stale save must not leave the checkbox presenting an unacknowledged role.
    ProfileStore external(profilePath_);
    const auto saved = external.load(controller_->selectedProfileId());
    QVERIFY(saved);
    QVERIFY(external.save(saved.value(), saved.value().revision));
    click(QStringLiteral("thumbLayerCheckbox"));
    QTRY_VERIFY(!controller_->pending() && !controller_->errorText().isEmpty());
    QVERIFY(!controller_->thumbLayer());
    const auto *checkbox = findItem(QStringLiteral("thumbLayerCheckbox"));
    QVERIFY(checkbox != nullptr && !checkbox->property("checked").toBool());
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    RecordingUiTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "recording_test.moc"
