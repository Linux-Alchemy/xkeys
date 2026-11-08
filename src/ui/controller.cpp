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

Controller::Controller(QObject *parent) : QObject(parent)
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
        return m_editControl.isEmpty() ? QString{} : chordLabel(editBinding().chord);
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
    return m_snapshot.mode == Mode::Edit && m_selected &&
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

QStringList Controller::sampleShortcuts() const
{
    QStringList shortcuts;
    for (const auto &chord : sampleChords) {
        shortcuts.append(chordLabel(chord));
    }
    return shortcuts;
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
    if (connected() && !pending()) {
        send(Operation::CreateProfile, {{QStringLiteral("name"), name}});
    }
}

void Controller::selectProfile(const QString &id)
{
    if (connected() && !pending()) {
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
    if (connected() && !pending() && m_selected) {
        auto args = selectedVersion();
        args.insert(QStringLiteral("name"), name);
        send(Operation::RenameProfile, args);
    }
}

void Controller::deleteSelected()
{
    if (connected() && !pending() && m_selected) {
        send(Operation::DeleteProfile, selectedVersion());
    }
}

void Controller::editSelected()
{
    if (connected() && !pending() && m_selected) {
        send(Operation::Edit, {{QStringLiteral("profileId"), m_selected->id}});
    }
}

void Controller::activateSelected()
{
    if (connected() && !pending() && m_selected) {
        send(Operation::Activate, selectedVersion());
    }
}

void Controller::deactivate()
{
    if (connected() && !pending()) {
        send(Operation::Deactivate);
    }
}

void Controller::setEditLayer(int layer)
{
    if (connected() && !pending() && editing() && (layer == 1 || layer == 2) &&
        layer != m_editLayer) {
        m_editLayer = layer;
        emit profileChanged();
        emit resolutionChanged();
    }
}

void Controller::setThumbLayer(bool enabled)
{
    if (connected() && !pending() && editing()) {
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
    if (!connected() || pending() || !editing() || m_editControl.isEmpty()) {
        return;
    }
    if (!binding.chord.isEmpty() && thumbLayer() && m_editControl == QStringLiteral("thumb")) {
        setError(QStringLiteral("Disable the thumb layer before assigning a thumb shortcut."));
        return;
    }
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
    if (connected() && !pending()) {
        const auto action = hold ? QStringLiteral("toggle") : QStringLiteral("tap");
        send(Operation::Input,
             {{QStringLiteral("control"), controlId}, {QStringLiteral("action"), action}});
    }
}

void Controller::releaseAll()
{
    if (connected() && !pending()) {
        send(Operation::Reset);
    }
}

void Controller::selectControl(const QString &controlId)
{
    if (!connected() || pending()) {
        return;
    }
    if (!editing()) {
        simulate(controlId, false);
    } else if (knownControl(controlId)) {
        m_editControl = controlId;
        setError({});
        emit profileChanged();
        emit resolutionChanged();
    }
}

void Controller::stopDemo()
{
    if (connected() && !pending()) {
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
    if (selectionChanged || hasStateData) {
        emit simulationChanged();
        emit profileChanged();
        emit resolutionChanged();
    }
    return true;
}

void Controller::clearRuntime()
{
    m_editControl.clear();
    m_editLayer = 1;
    m_snapshot = {};
    m_resolution = {};
    emit simulationChanged();
    emit profileChanged();
    emit resolutionChanged();
}

void Controller::clearPending()
{
    m_deadline.stop();
    if (m_pending) {
        m_pending.reset();
        emit pendingChanged();
    }
}

} // namespace xkeys
