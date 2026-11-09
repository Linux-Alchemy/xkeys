#include "controller.h"

#include "core/profile_store.h"
#include "core/remapper.h"
#include "ipc/endpoint.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QProcess>
#include <QSet>
#include <QUuid>
#include <QVariantMap>

#include <algorithm>
#include <array>
#include <utility>

namespace xkeys
{

using namespace ipc;

namespace
{
const std::array<QList<int>, 5> sampleChords{{{29, 56, 18}, {42, 33}, {17}, {57}, {42}}};

bool refreshProfilesAfter(Operation operation)
{
    return operation == Operation::CreateProfile || operation == Operation::RenameProfile ||
           operation == Operation::SaveProfile || operation == Operation::DeleteProfile;
}

bool returnsProfile(Operation operation)
{
    return operation == Operation::CreateProfile || operation == Operation::LoadProfile ||
           operation == Operation::SaveProfile || operation == Operation::RenameProfile ||
           operation == Operation::Edit;
}
} // namespace

Controller::Controller(QObject *parent)
    : Controller(makeCaptureBackend(false), makeShortcutContext(), parent)
{
}

Controller::Controller(std::unique_ptr<CaptureBackend> capture,
                       std::unique_ptr<ShortcutContext> context, QObject *parent)
    : QObject(parent), m_capture(std::move(capture)), m_context(std::move(context))
{
    m_socket.setReadBufferSize(maxFrameBytes + 1);
    m_retry.setInterval(75);
    m_deadline.setSingleShot(true);

    connect(&m_retry, &QTimer::timeout, this, &Controller::tryConnect);
    connect(&m_deadline, &QTimer::timeout, this,
            [this] { fail(QStringLiteral("The demo service did not reply in time.")); });
    connect(&m_socket, &QLocalSocket::connected, this, [this] {
        m_retry.stop();
        setState(State::Handshake);
        send(Operation::Hello);
    });
    connect(&m_socket, &QLocalSocket::disconnected, this, [this] {
        if (m_state == State::Handshake || m_state == State::Ready) {
            fail(QStringLiteral("The demo service disconnected."));
        }
    });
    connect(&m_socket, &QLocalSocket::errorOccurred, this, &Controller::handleSocketError);
    connect(&m_socket, &QLocalSocket::readyRead, this, &Controller::readReplies);

    m_recordingTick.setInterval(250);
    connect(&m_recordingTick, &QTimer::timeout, this, [this] {
        if (recordingRemainingSeconds() == 0) {
            cancelRecording();
        } else {
            emit recordingChanged();
        }
    });
    connect(
        m_context.get(), &ShortcutContext::prepared, this, [this](bool ok, const QString &reason) {
            if (m_recordState != RecordState::Preparing || m_captureStarted) {
                return;
            }
            const auto *keyboard = selectedKeyboard();
            if (!ok || !validRecordTarget() || !keyboard || !keyboard->accessible) {
                discardDraft(RecordState::Error,
                             reason.isEmpty() ? QStringLiteral("Recording could not be prepared.")
                                              : reason);
                refreshRecordingKeyboards();
                return;
            }
            m_layoutKeyboardId = keyboard->id;
            m_captureStarted = true;
            emit recordingChanged();
            emit resolutionChanged();
            m_capture->start(*keyboard);
        });
    connect(m_capture.get(), &CaptureBackend::ready, this, [this] {
        if (m_recordState == RecordState::Preparing && m_captureStarted && validRecordTarget()) {
            m_captureClock.start();
            m_recordingTick.start();
            setRecordState(RecordState::Recording, QStringLiteral("Press your shortcut."));
        }
    });
    connect(m_capture.get(), &CaptureBackend::progress, this, [this](const QList<int> &codes) {
        if (m_recordState == RecordState::Recording && validRecordTarget()) {
            m_draftChord = m_context->label(codes);
            emit recordingChanged();
        }
    });
    connect(m_capture.get(), &CaptureBackend::completed, this, &Controller::finishCapture);
    connect(m_capture.get(), &CaptureBackend::cancelled, this,
            [this](const QString &reason) { captureEnded(reason, false); });
    connect(m_capture.get(), &CaptureBackend::failed, this,
            [this](const QString &reason) { captureEnded(reason, true); });
}

void Controller::startDemo(const QString &socketPath, const QString &profilesDir)
{
    setState(State::Disconnected);
    clearPending();
    m_retry.stop();
    m_socket.abort();
    m_socketOverride = socketPath;
    m_profilesOverride = profilesDir;
    m_launched = false;
    m_profiles.clear();
    m_selected.reset();
    emit profilesChanged();
    clearRuntime();

    QString error;
    m_endpoint = demoEndpoint(socketPath, error);
    if (m_endpoint.isEmpty()) {
        setError(error);
        return;
    }
    m_profilesDir = demoProfilesDir(profilesDir, socketPath, error);
    setError(error);
    if (m_profilesDir.isEmpty()) {
        return;
    }

    m_startClock.start();
    setState(State::Connecting);
    m_retry.start();
    tryConnect();
}

bool Controller::connected() const { return m_state == State::Ready; }
bool Controller::pending() const { return m_pending.has_value(); }
QString Controller::errorText() const { return m_error; }
QString Controller::selectedControl() const
{
    return editing() ? m_editControl : m_resolution.control;
}

QString Controller::bindingLabel() const
{
    if (editing()) {
        return m_editControl.isEmpty() ? QString{} : savedChordLabel(editBinding().chord);
    }
    return m_resolution.label;
}

int Controller::selectedLayer() const { return editing() ? m_editLayer : m_resolution.layer; }
bool Controller::blank() const
{
    return editing() ? !m_editControl.isEmpty() && editBinding().chord.isEmpty()
                     : m_resolution.blank;
}

QVariantList Controller::profiles() const { return m_profiles; }
QString Controller::selectedProfileId() const { return m_selected ? m_selected->id : QString{}; }
QString Controller::selectedProfileName() const
{
    return m_selected ? m_selected->name : QString{};
}
bool Controller::hasProfile() const { return m_selected.has_value(); }
bool Controller::thumbLayer() const { return m_selected && m_selected->thumbLayer; }
int Controller::editLayer() const { return m_editLayer; }
QString Controller::bindingActionLabel() const
{
    return editing() ? editBinding().label : QString{};
}
QString Controller::activeProfileId() const { return m_snapshot.activeId; }
QString Controller::activeProfileName() const { return m_snapshot.activeName; }
QString Controller::mode() const { return modeName(m_snapshot.mode); }
bool Controller::editing() const
{
    return m_editLease && m_snapshot.mode == Mode::Edit && m_selected &&
           m_snapshot.editProfileId == m_selected->id;
}
int Controller::activeLayer() const { return m_snapshot.activeLayer; }
QStringList Controller::heldControls() const { return m_snapshot.heldControls; }

QString Controller::heldOutputLabel() const
{
    return m_snapshot.ownedKeys.isEmpty() ? QStringLiteral("None")
                                          : chordLabel(m_snapshot.ownedKeys);
}

QStringList Controller::outputEvents() const
{
    QStringList events;
    for (const auto &event : m_snapshot.output) {
        const auto prefix = event.transition == KeyTransition::Down ? QStringLiteral("Down ")
                                                                    : QStringLiteral("Up ");
        events.append(prefix + chordLabel({event.code}));
    }
    return events;
}

bool Controller::recordingAvailable() const { return m_capture->available(); }

QString Controller::recordingUnavailableReason() const
{
    return recordingAvailable() ? m_inventoryError : m_capture->unavailableReason();
}

QVariantList Controller::recordingKeyboards() const
{
    QVariantList result;
    for (const auto &keyboard : m_keyboards) {
        result.append(QVariantMap{{QStringLiteral("id"), keyboard.id},
                                  {QStringLiteral("name"), keyboard.name},
                                  {QStringLiteral("accessible"), keyboard.accessible},
                                  {QStringLiteral("reason"), keyboard.reason}});
    }
    return result;
}

QString Controller::recordingKeyboardId() const { return m_recordingKeyboardId; }

QString Controller::recordingState() const
{
    switch (m_recordState) {
    case RecordState::Idle:
        return QStringLiteral("Idle");
    case RecordState::Preparing:
        return QStringLiteral("Preparing");
    case RecordState::Recording:
        return QStringLiteral("Recording");
    case RecordState::Stopping:
        return QStringLiteral("Stopping");
    case RecordState::Draft:
        return QStringLiteral("Draft");
    case RecordState::Saving:
        return QStringLiteral("Saving");
    case RecordState::Error:
        return QStringLiteral("Error");
    }
    return {};
}

QString Controller::recordingMessage() const { return m_recordMessage; }
QString Controller::draftChord() const { return m_draftChord; }
QString Controller::draftActionLabel() const { return m_draftActionLabel; }

bool Controller::recordingBusy() const
{
    return m_recordState == RecordState::Preparing || m_recordState == RecordState::Recording ||
           m_recordState == RecordState::Stopping;
}

bool Controller::canSaveDraft() const
{
    if (m_recordState != RecordState::Draft || !validRecordTarget() || m_draftCodes.isEmpty() ||
        ((!m_conflictsChecked || !m_conflicts.isEmpty()) && !m_conflictAcknowledged)) {
        return false;
    }
    auto profile = *m_selected;
    profile.layers[0].insert(m_recordTarget->control, {m_draftActionLabel, m_draftCodes});
    return static_cast<bool>(validateProfile(profile));
}

QStringList Controller::bindingConflicts() const { return m_conflicts; }
bool Controller::conflictsChecked() const { return m_conflictsChecked; }
bool Controller::conflictAcknowledged() const { return m_conflictAcknowledged; }
QString Controller::conflictCheckNote() const
{
    return m_recordState == RecordState::Draft || m_recordState == RecordState::Saving
               ? m_context->advisory()
               : QString{};
}

QVariantMap Controller::controlLabels() const
{
    const bool matchesActive = m_selected && m_snapshot.mode == Mode::Active &&
                               m_snapshot.activeId == m_selected->id &&
                               m_snapshot.activeRevision == m_selected->revision;
    if (!editing() && !matchesActive) {
        return {};
    }

    const int layer = editing() ? m_editLayer : m_snapshot.activeLayer;
    const auto &bindings = m_selected->layers.at(static_cast<std::size_t>(layer - 1));
    QVariantMap labels;
    for (auto entry = bindings.cbegin(); entry != bindings.cend(); ++entry) {
        const auto text =
            matchesActive && m_selected->thumbLayer && entry.key() == QStringLiteral("thumb")
                ? QStringLiteral("Layer 2")
                : savedChordLabel(entry.value().chord);
        labels.insert(entry.key(), text);
    }
    return labels;
}

void Controller::setDraftActionLabel(const QString &label)
{
    if (m_recordState == RecordState::Draft && label != m_draftActionLabel) {
        m_draftActionLabel = label;
        emit recordingChanged();
    }
}

int Controller::recordingRemainingSeconds() const
{
    if (m_recordState != RecordState::Recording) {
        return 0;
    }
    return std::max(0, static_cast<int>((10000 - m_captureClock.elapsed() + 999) / 1000));
}

void Controller::refreshRecordingKeyboards()
{
    if (!recordingAvailable() || recordingBusy() || m_recordState == RecordState::Saving) {
        return;
    }
    const auto *previous = selectedKeyboard();
    const auto approved = previous ? std::optional<KeyboardSelection>(*previous) : std::nullopt;
    auto inventory = m_capture->keyboards();
    if (inventory) {
        m_keyboards = std::move(inventory.value());
        m_inventoryError =
            m_keyboards.isEmpty() ? QStringLiteral("No supported USB keyboards found.") : QString{};
    } else {
        m_inventoryError = inventory.error().message;
        m_keyboards.clear();
    }
    const auto *keyboard = selectedKeyboard();
    const bool changed =
        approved && (!keyboard || !keyboard->accessible || keyboard->name != approved->name ||
                     keyboard->nodes != approved->nodes ||
                     keyboard->interfaceNames != approved->interfaceNames ||
                     keyboard->typingNames != approved->typingNames ||
                     keyboard->fingerprint != approved->fingerprint);
    if (!keyboard || !keyboard->accessible || changed) {
        m_recordingKeyboardId.clear();
        m_layoutKeyboardId.clear();
    }
    if (changed && inventory) {
        m_inventoryError = QStringLiteral("The selected keyboard changed. Choose it again.");
    }
    emit recordingChanged();
    emit resolutionChanged();
}

void Controller::setRecordingKeyboard(const QString &id)
{
    if (!recordingAvailable() || recordingBusy() || m_recordState == RecordState::Saving ||
        id == m_recordingKeyboardId) {
        return;
    }
    const auto item = std::ranges::find(m_keyboards, id, &KeyboardSelection::id);
    if (!id.isEmpty() && (item == m_keyboards.end() || !item->accessible)) {
        m_recordingKeyboardId.clear();
        m_layoutKeyboardId.clear();
        discardDraft(RecordState::Error, item != m_keyboards.end() && !item->reason.isEmpty()
                                             ? item->reason
                                             : QStringLiteral("Choose an accessible keyboard."));
        emit resolutionChanged();
        return;
    }
    m_recordingKeyboardId = id;
    m_layoutKeyboardId.clear();
    if (!id.isEmpty()) {
        m_inventoryError.clear();
    }
    setError({});
    discardDraft(RecordState::Idle);
    emit resolutionChanged();
}

void Controller::setWindowActive(bool active)
{
    m_windowActive = active;
    if (!active && (recordingBusy() || m_recordState == RecordState::Draft)) {
        cancelRecording();
    }
}

void Controller::beginEdit(const QString &controlId, int layer)
{
    if (!canChange() || !editing() || !knownControl(controlId)) {
        return;
    }
    if (recordingAvailable() && layer != 1) {
        discardDraft(RecordState::Error, QStringLiteral("Recording is available for Layer 1."));
        return;
    }
    if (layer != m_editLayer) {
        return;
    }

    m_editControl = controlId;
    setError({});
    emit profileChanged();
    emit resolutionChanged();
    if (thumbLayer() && controlId == QStringLiteral("thumb")) {
        discardDraft(
            RecordState::Idle,
            QStringLiteral("The thumb controls Layer 2. You can clear its saved shortcut."));
        return;
    }
    if (!recordingAvailable()) {
        discardDraft(RecordState::Idle, recordingUnavailableReason());
        return;
    }
    discardDraft(RecordState::Idle,
                 QStringLiteral("Choose your recording device, then click Start recording."));
}

void Controller::recordAgain()
{
    if (!canChange() || !editing() || m_editLayer != 1 || !knownControl(m_editControl) ||
        !recordingAvailable() || (thumbLayer() && m_editControl == QStringLiteral("thumb"))) {
        return;
    }
    if (!m_windowActive) {
        discardDraft(RecordState::Error, QStringLiteral("Focus the editor before recording."));
        return;
    }
    const auto *keyboard = selectedKeyboard();
    if (!keyboard || !keyboard->accessible) {
        discardDraft(RecordState::Error, QStringLiteral("Choose an accessible keyboard first."));
        refreshRecordingKeyboards();
        return;
    }

    refreshRecordingKeyboards();
    keyboard = selectedKeyboard();
    if (!keyboard || !keyboard->accessible) {
        discardDraft(RecordState::Error,
                     m_inventoryError.isEmpty()
                         ? QStringLiteral("The selected keyboard changed. Choose it again.")
                         : m_inventoryError);
        return;
    }

    discardDraft(RecordState::Preparing,
                 QStringLiteral("Preparing keyboard. Wait before pressing keys."));
    m_recordTarget = RecordTarget{m_selected->id, m_selected->revision, m_editControl};
    m_draftActionLabel = editBinding().label;
    m_layoutKeyboardId.clear();
    m_context->refresh(*keyboard);
}

void Controller::saveDraft()
{
    if (!canSaveDraft()) {
        return;
    }
    auto profile = *m_selected;
    profile.layers[0].insert(m_recordTarget->control, {m_draftActionLabel, m_draftCodes});
    setRecordState(RecordState::Saving, QStringLiteral("Saving shortcut."));
    saveProfile(profile);
}

void Controller::cancelDraft()
{
    if (m_recordState != RecordState::Saving) {
        cancelRecording();
    }
}

void Controller::setConflictAcknowledged(bool acknowledged)
{
    if (m_recordState == RecordState::Draft && m_conflictAcknowledged != acknowledged) {
        m_conflictAcknowledged = acknowledged;
        emit recordingChanged();
    }
}

bool Controller::canChange() const
{
    return connected() && !pending() && !recordingBusy() && m_recordState != RecordState::Saving;
}

bool Controller::validRecordTarget() const
{
    return connected() && !pending() && m_windowActive && editing() && m_recordTarget &&
           m_selected->id == m_recordTarget->profileId &&
           m_selected->revision == m_recordTarget->revision &&
           m_editControl == m_recordTarget->control && m_editLayer == 1;
}

const KeyboardSelection *Controller::selectedKeyboard() const
{
    const auto item = std::ranges::find(m_keyboards, m_recordingKeyboardId, &KeyboardSelection::id);
    return item == m_keyboards.cend() ? nullptr : &*item;
}

QString Controller::savedChordLabel(const QList<int> &codes) const
{
    if (!m_layoutKeyboardId.isEmpty() && m_layoutKeyboardId == m_recordingKeyboardId) {
        const auto label = m_context->label(codes);
        if (!label.isEmpty()) {
            return label;
        }
    }
    return chordLabel(codes);
}

void Controller::finishCapture(const QList<int> &codes)
{
    if (!m_captureStarted) {
        return;
    }
    m_captureStarted = false;
    m_recordingTick.stop();
    if (m_recordState == RecordState::Stopping) {
        discardDraft(RecordState::Idle, QStringLiteral("Recording cancelled."));
        return;
    }
    if (m_recordState != RecordState::Recording || !validRecordTarget()) {
        discardDraft(RecordState::Error,
                     QStringLiteral("The recording target is no longer available."));
        return;
    }

    auto profile = *m_selected;
    profile.layers[0].insert(m_recordTarget->control, {m_draftActionLabel, codes});
    const auto label = m_context->label(codes);
    if (codes.isEmpty() || label.isEmpty() || !validateProfile(profile)) {
        discardDraft(RecordState::Error,
                     QStringLiteral("The recorded keys could not be mapped to a shortcut."));
        return;
    }

    m_draftChord = label;
    m_draftCodes = codes;
    m_conflicts = m_context->conflicts(codes);
    m_conflictsChecked = m_context->bindingsAvailable();
    setRecordState(RecordState::Draft, QStringLiteral("Review the shortcut, then Save or Cancel."));
}

void Controller::captureEnded(const QString &reason, bool failed)
{
    if (!m_captureStarted) {
        return;
    }
    m_captureStarted = false;
    discardDraft(failed ? RecordState::Error : RecordState::Idle, reason);
    if (failed) {
        refreshRecordingKeyboards();
    }
}

void Controller::discardDraft(RecordState next, const QString &message)
{
    m_recordingTick.stop();
    m_recordTarget.reset();
    m_draftCodes.clear();
    m_draftChord.clear();
    m_draftActionLabel.clear();
    m_conflicts.clear();
    m_conflictsChecked = false;
    m_conflictAcknowledged = false;
    setRecordState(next, message);
}

void Controller::setRecordState(RecordState state, const QString &message)
{
    m_recordState = state;
    m_recordMessage = message;
    emit recordingChanged();
}

void Controller::cancelRecording()
{
    if (m_recordState == RecordState::Stopping || m_recordState == RecordState::Idle) {
        return;
    }
    if (m_captureStarted) {
        m_recordingTick.stop();
        setRecordState(RecordState::Stopping, QStringLiteral("Stopping. Release all keys."));
        m_capture->cancel();
    } else {
        discardDraft(RecordState::Idle, QStringLiteral("Recording cancelled."));
    }
}

void Controller::clearEditLease()
{
    m_editLease = false;
    cancelRecording();
}

QString Controller::statusText() const
{
    switch (m_state) {
    case State::Disconnected:
        return QStringLiteral("Disconnected");
    case State::Connecting:
        return QStringLiteral("Connecting");
    case State::Handshake:
        return QStringLiteral("Checking service");
    case State::Ready:
        return QStringLiteral("Ready");
    }
    return {};
}

QVariantList Controller::controls() const
{
    QVariantList result;
    for (const Control &control : demoControls()) {
        result.append(QVariantMap{{QStringLiteral("controlId"), control.id},
                                  {QStringLiteral("label"), control.label}});
    }
    return result;
}

void Controller::createProfile(const QString &name)
{
    if (canChange()) {
        discardDraft(RecordState::Idle);
        send(Operation::CreateProfile, {{QStringLiteral("name"), name}});
    }
}

void Controller::selectProfile(const QString &id)
{
    if (canChange()) {
        discardDraft(RecordState::Idle);
        send(Operation::LoadProfile, {{QStringLiteral("profileId"), id}});
    }
}

QJsonObject Controller::selectedVersion() const
{
    return {{QStringLiteral("profileId"), m_selected->id},
            {QStringLiteral("expected"), m_selected->revision}};
}

void Controller::renameSelected(const QString &name)
{
    if (canChange() && m_selected) {
        discardDraft(RecordState::Idle);
        auto args = selectedVersion();
        args.insert(QStringLiteral("name"), name);
        send(Operation::RenameProfile, args);
    }
}

void Controller::deleteSelected()
{
    if (canChange() && m_selected) {
        discardDraft(RecordState::Idle);
        send(Operation::DeleteProfile, selectedVersion());
    }
}

void Controller::editSelected()
{
    if (canChange() && m_selected) {
        discardDraft(RecordState::Idle);
        send(Operation::Edit, {{QStringLiteral("profileId"), m_selected->id}});
    }
}

void Controller::activateSelected()
{
    if (canChange() && m_selected) {
        discardDraft(RecordState::Idle);
        send(Operation::Activate, selectedVersion());
    }
}

void Controller::deactivate()
{
    if (canChange()) {
        discardDraft(RecordState::Idle);
        send(Operation::Deactivate);
    }
}

void Controller::setEditLayer(int layer)
{
    if (layer == 2 && recordingAvailable() && canChange()) {
        setError(QStringLiteral("Recording is available for Layer 1."));
        return;
    }
    if (canChange() && editing() && (layer == 1 || layer == 2) && layer != m_editLayer) {
        discardDraft(RecordState::Idle);
        m_editLayer = layer;
        emit profileChanged();
        emit resolutionChanged();
        emit recordingChanged();
    }
}

void Controller::setThumbLayer(bool enabled)
{
    if (canChange() && editing()) {
        discardDraft(RecordState::Idle);
        auto profile = *m_selected;
        profile.thumbLayer = enabled;
        saveProfile(profile);
    }
}

void Controller::assignSample(int preset, const QString &label)
{
    if (preset < 0 || preset >= static_cast<int>(sampleChords.size())) {
        setError(QStringLiteral("Choose a sample shortcut."));
        return;
    }
    updateBinding({label, sampleChords.at(static_cast<std::size_t>(preset))});
}

void Controller::clearBinding() { updateBinding({}); }

void Controller::updateBinding(const Binding &binding)
{
    if (!canChange() || !editing() || m_editControl.isEmpty()) {
        return;
    }
    if (!binding.chord.isEmpty() && thumbLayer() && m_editControl == QStringLiteral("thumb")) {
        setError(QStringLiteral("Disable the thumb layer before assigning a thumb shortcut."));
        return;
    }
    discardDraft(RecordState::Idle);
    auto profile = *m_selected;
    profile.layers.at(static_cast<std::size_t>(m_editLayer - 1)).insert(m_editControl, binding);
    saveProfile(profile);
}

Binding Controller::editBinding() const
{
    return m_selected->layers.at(static_cast<std::size_t>(m_editLayer - 1)).value(m_editControl);
}

void Controller::saveProfile(const Profile &profile)
{
    send(Operation::SaveProfile, {{QStringLiteral("profile"), profileToJson(profile)},
                                  {QStringLiteral("expected"), profile.revision}});
}

void Controller::simulate(const QString &controlId, bool hold)
{
    if (canChange()) {
        const auto action = hold ? QStringLiteral("toggle") : QStringLiteral("tap");
        send(Operation::Input,
             {{QStringLiteral("control"), controlId}, {QStringLiteral("action"), action}});
    }
}

void Controller::releaseAll()
{
    if (canChange()) {
        send(Operation::Reset);
    }
}

void Controller::selectControl(const QString &controlId)
{
    if (!canChange()) {
        return;
    }
    if (editing()) {
        beginEdit(controlId, m_editLayer);
    } else {
        simulate(controlId, false);
    }
}

void Controller::stopDemo()
{
    if (canChange()) {
        discardDraft(RecordState::Idle);
        send(Operation::Stop);
    }
}

void Controller::reconnectDemo()
{
    if (m_state == State::Disconnected) {
        const QString socketPath = m_socketOverride;
        const QString profilesDir = m_profilesOverride;
        startDemo(socketPath, profilesDir);
    }
}

void Controller::setState(State state)
{
    if (m_state != state) {
        m_state = state;
        emit connectionChanged();
    }
}

void Controller::setError(const QString &error)
{
    if (m_error != error) {
        m_error = error;
        emit errorChanged();
    }
}

void Controller::fail(const QString &error)
{
    m_retry.stop();
    setState(State::Disconnected);
    clearPending();
    setError(error);
    clearRuntime();
    m_socket.abort();
}

void Controller::tryConnect()
{
    if (m_startClock.elapsed() >= timeoutMs) {
        fail(QStringLiteral("The demo service did not start in time."));
        return;
    }

    if (m_socket.state() == QLocalSocket::UnconnectedState) {
        const QString error = checkSocketPath(m_endpoint);
        if (!error.isEmpty()) {
            fail(error);
            return;
        }
        m_socket.connectToServer(m_endpoint);
    }
}

void Controller::handleSocketError(QLocalSocket::LocalSocketError error)
{
    if (m_state == State::Disconnected) {
        return;
    }

    if (m_state == State::Connecting && (error == QLocalSocket::ServerNotFoundError ||
                                         error == QLocalSocket::ConnectionRefusedError)) {
        if (!m_launched) {
            m_launched = true;
            const QString service =
                QCoreApplication::applicationDirPath() + QStringLiteral("/xkeysd");
            if (!QProcess::startDetached(
                    service, {QStringLiteral("--demo"), QStringLiteral("--socket"), m_endpoint,
                              QStringLiteral("--profiles-dir"), m_profilesDir})) {
                fail(QStringLiteral("Could not launch the demo service."));
            }
        }
        return;
    }

    if (error != QLocalSocket::PeerClosedError) {
        fail(m_socket.errorString());
    }
}

void Controller::send(Operation operation, const QJsonObject &args)
{
    setError({});
    const bool alreadyPending = pending();
    m_pending = Request{QUuid::createUuid().toString(QUuid::WithoutBraces), operation, args};
    if (!alreadyPending) {
        emit pendingChanged();
        emit recordingChanged();
    }
    m_deadline.start(timeoutMs);

    if (!sendFrame(m_socket, makeRequest(*m_pending))) {
        fail(QStringLiteral("Could not send the demo request."));
    }
}

void Controller::readReplies()
{
    QJsonObject object;
    const Frame frame = readFrame(m_socket, object);
    if (frame == Frame::Incomplete) {
        return;
    }

    QString error;
    QJsonObject data;
    if (frame == Frame::Invalid || !m_pending || !parseReply(object, *m_pending, error, data) ||
        m_socket.bytesAvailable() != 0) {
        fail(QStringLiteral("Unexpected reply from the demo service."));
        return;
    }

    const auto request = *m_pending;
    const Operation operation = request.operation;
    if (!error.isEmpty()) {
        if (m_state == State::Handshake) {
            fail(error);
        } else {
            setError(error);
            if (operation == Operation::SaveProfile && m_recordState == RecordState::Saving) {
                if (m_windowActive) {
                    setRecordState(RecordState::Draft, error);
                } else {
                    discardDraft(RecordState::Idle);
                }
            }
            clearPending();
        }
        return;
    }
    if (operation == Operation::Hello) {
        const auto path = data.value(QStringLiteral("profilesDir")).toString();
        if (path.startsWith(u'/') && !path.contains(QChar::Null) && path != m_profilesDir) {
            fail(QStringLiteral("The demo service uses a different profiles directory. Stop it or "
                                "use a separate socket."));
            return;
        }
    }
    if (!applyReply(request, data)) {
        fail(QStringLiteral("Unexpected reply data from the demo service."));
        return;
    }
    if (operation == Operation::Hello || refreshProfilesAfter(operation)) {
        send(Operation::Profiles);
    } else {
        if (operation == Operation::Profiles && m_state == State::Handshake) {
            setState(State::Ready);
            refreshRecordingKeyboards();
        } else if (operation == Operation::Stop) {
            setState(State::Disconnected);
            clearRuntime();
            m_socket.disconnectFromServer();
        }
        clearPending();
    }
}

bool Controller::applyProfiles(const QJsonObject &data)
{
    const auto profiles = data.value(QStringLiteral("profiles"));
    if (data.size() != 1 || !profiles.isArray() || profiles.toArray().size() > 128) {
        return false;
    }
    QVariantList entries;
    QSet<QString> ids;
    const auto list = profiles.toArray();
    for (const auto item : list) {
        const auto metadata = item.toObject();
        const auto id = metadata.value(QStringLiteral("id")).toString();
        if (!validMetadata(metadata) || ids.contains(id)) {
            return false;
        }
        ids.insert(id);
        entries.append(metadata.toVariantMap());
    }
    m_profiles = std::move(entries);
    emit profilesChanged();
    return true;
}

bool Controller::applyReply(const Request &request, const QJsonObject &data)
{
    const auto operation = request.operation;
    if (operation == Operation::Profiles) {
        return applyProfiles(data);
    }
    if (operation == Operation::Stop) {
        return data.isEmpty();
    }

    const auto requestedId = request.args.value(QStringLiteral("profileId")).toString();
    const auto expected = request.args.value(QStringLiteral("expected")).toInteger();
    const bool hasProfileData = returnsProfile(operation);
    const bool hasStateData = !hasProfileData || operation == Operation::Edit;
    const bool pairedData = operation == Operation::Hello || operation == Operation::Edit ||
                            operation == Operation::Input;
    if (data.size() != (pairedData ? 2 : 1) ||
        (operation == Operation::Hello &&
         data.value(QStringLiteral("profilesDir")) != m_profilesDir)) {
        return false;
    }

    std::optional<Profile> profile;
    if (hasProfileData) {
        auto parsed = profileFromJson(data.value(QStringLiteral("profile")).toObject());
        if (!parsed || parsed.value().revision == 0) {
            return false;
        }
        profile = std::move(parsed.value());
        if (!requestedId.isEmpty() && profile->id != requestedId) {
            return false;
        }
        if (operation == Operation::CreateProfile) {
            auto blank = makeBlankProfile(profile->id,
                                          request.args.value(QStringLiteral("name")).toString());
            blank.revision = 1;
            if (*profile != blank) {
                return false;
            }
        } else if (operation == Operation::SaveProfile) {
            auto sent = request.args.value(QStringLiteral("profile")).toObject();
            sent.insert(QStringLiteral("revision"), expected + 1);
            if (profileToJson(*profile) != sent) {
                return false;
            }
        } else if (operation == Operation::RenameProfile &&
                   (profile->revision != expected + 1 ||
                    profile->name != request.args.value(QStringLiteral("name")).toString())) {
            return false;
        }
    }

    std::optional<Snapshot> snapshot;
    if (hasStateData) {
        snapshot = snapshotFromJson(data.value(QStringLiteral("state")).toObject());
        if (!snapshot) {
            return false;
        }
        switch (operation) {
        case Operation::Activate:
            if (snapshot->mode != Mode::Active || snapshot->activeId != requestedId ||
                snapshot->activeRevision != expected || snapshot->activeLayer != 1 ||
                !snapshot->heldControls.isEmpty() || !snapshot->output.isEmpty()) {
                return false;
            }
            break;
        case Operation::Edit:
            if (snapshot->mode != Mode::Edit || snapshot->editProfileId != profile->id) {
                return false;
            }
            break;
        case Operation::Deactivate:
            if (snapshot->mode != Mode::Inactive) {
                return false;
            }
            break;
        case Operation::Reset:
            if (!snapshot->heldControls.isEmpty() || snapshot->activeLayer != 1 ||
                std::ranges::any_of(snapshot->output, [](const auto &event) {
                    return event.transition != KeyTransition::Up;
                })) {
                return false;
            }
            break;
        case Operation::DeleteProfile:
            if (snapshot->activeId == requestedId || snapshot->editProfileId == requestedId) {
                return false;
            }
            break;
        default:
            break;
        }
    }

    if (operation == Operation::Input) {
        auto resolution = resolutionFromJson(data.value(QStringLiteral("resolution")).toObject());
        if (!resolution || snapshot->mode != Mode::Active ||
            resolution->control != request.args.value(QStringLiteral("control")).toString()) {
            return false;
        }
        m_resolution = std::move(*resolution);
    } else if (operation == Operation::LoadProfile || operation == Operation::CreateProfile ||
               operation == Operation::Edit || operation == Operation::DeleteProfile ||
               operation == Operation::Activate || operation == Operation::Deactivate) {
        m_editControl.clear();
        m_editLayer = 1;
        m_resolution = {};
    }

    const bool selectionChanged = profile.has_value() || operation == Operation::DeleteProfile;
    if (selectionChanged) {
        m_selected = std::move(profile);
    }
    if (snapshot) {
        m_snapshot = std::move(*snapshot);
    }
    if (operation == Operation::Edit) {
        m_editLease = true;
    } else if (m_snapshot.mode != Mode::Edit || selectedProfileId() != m_snapshot.editProfileId) {
        clearEditLease();
    }
    if (operation == Operation::SaveProfile && m_recordState == RecordState::Saving) {
        discardDraft(RecordState::Idle, QStringLiteral("Saved."));
    }
    if (selectionChanged || hasStateData) {
        emit simulationChanged();
        emit profileChanged();
        emit resolutionChanged();
        emit recordingChanged();
    }
    return true;
}

void Controller::clearRuntime()
{
    clearEditLease();
    m_editControl.clear();
    m_editLayer = 1;
    m_snapshot = {};
    m_resolution = {};
    emit simulationChanged();
    emit profileChanged();
    emit resolutionChanged();
    emit recordingChanged();
}

void Controller::clearPending()
{
    m_deadline.stop();
    if (m_pending) {
        m_pending.reset();
        emit pendingChanged();
        emit recordingChanged();
    }
}

} // namespace xkeys
