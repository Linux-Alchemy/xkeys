#include "session.h"

#include <QJsonArray>
#include <QUuid>

namespace xkeys
{

using namespace ipc;

namespace
{
QJsonObject profileData(const Profile &profile)
{
    return {{QStringLiteral("profile"), profileToJson(profile)}};
}
} // namespace

DemoSession::DemoSession(const QString &profilesDir)
    : m_profilesDir(profilesDir), m_store(profilesDir)
{
}

QJsonObject DemoSession::handle(const QLocalSocket &peer, const Request &request)
{
    switch (request.operation) {
    case Operation::Hello: {
        auto data = stateData();
        data.insert(QStringLiteral("profilesDir"), m_profilesDir);
        return successReply(request.id, data);
    }
    case Operation::Profiles:
        return profilesReply(request.id);
    case Operation::LoadProfile: {
        const auto profile =
            m_store.load(request.args.value(QStringLiteral("profileId")).toString());
        return profile ? successReply(request.id, profileData(profile.value()))
                       : errorReply(request.id, profile.error().message);
    }
    default:
        if (m_owner && m_owner != &peer) {
            return errorReply(request.id, QStringLiteral("Another editor is using the demo."));
        }
        const auto reply = mutate(request);
        if (reply.value(QStringLiteral("ok")).toBool()) {
            m_owner = &peer;
        }
        return reply;
    }
}

void DemoSession::disconnect(const QLocalSocket &peer)
{
    if (m_owner != &peer) {
        return;
    }
    m_owner = nullptr;
    m_remapper.reset();
    m_state.output.clear();
    if (m_state.mode == Mode::Edit) {
        m_state = {};
    }
}

QJsonObject DemoSession::stateData() const
{
    Snapshot snapshot = m_state;
    snapshot.activeLayer = m_remapper.activeLayer();
    snapshot.heldControls = m_remapper.heldControls();
    snapshot.ownedKeys = m_remapper.ownedKeys();
    return {{QStringLiteral("state"), snapshotToJson(snapshot)}};
}

QJsonObject DemoSession::profilesReply(const QString &id) const
{
    const auto profiles = m_store.list();
    if (!profiles) {
        return errorReply(id, profiles.error().message);
    }
    QJsonArray list;
    for (const auto &profile : profiles.value()) {
        list.append(metadataToJson(profile));
    }
    return successReply(id, {{QStringLiteral("profiles"), list}});
}

QJsonObject DemoSession::saveProfile(const QString &id, Profile profile, Revision expected)
{
    const auto saved = m_store.save(profile, expected);
    if (!saved) {
        return errorReply(id, saved.error().message);
    }
    profile.revision = saved.value();
    return successReply(id, profileData(profile));
}

QJsonObject DemoSession::mutate(const Request &request)
{
    const auto &id = request.id;
    const auto &args = request.args;
    const auto profileId = args.value(QStringLiteral("profileId")).toString();
    const auto expected = args.value(QStringLiteral("expected")).toInteger();
    const auto name = args.value(QStringLiteral("name")).toString();

    switch (request.operation) {
    case Operation::CreateProfile:
        return saveProfile(
            id, makeBlankProfile(QUuid::createUuid().toString(QUuid::WithoutBraces), name), 0);
    case Operation::SaveProfile: {
        auto profile = profileFromJson(args.value(QStringLiteral("profile")).toObject());
        if (!profile) {
            return errorReply(id, profile.error().message);
        }
        if (m_state.mode != Mode::Edit || m_state.editProfileId != profile.value().id) {
            return errorReply(id, QStringLiteral("Edit this profile before saving bindings."));
        }
        return saveProfile(id, std::move(profile.value()), expected);
    }
    case Operation::RenameProfile: {
        auto profile = m_store.load(profileId);
        if (!profile) {
            return errorReply(id, profile.error().message);
        }
        if (profile.value().revision != expected) {
            return errorReply(
                id, QStringLiteral("The saved profile changed. Reload it and try again."));
        }
        profile.value().name = name;
        return saveProfile(id, std::move(profile.value()), expected);
    }
    case Operation::DeleteProfile: {
        if (m_state.mode == Mode::Active && m_state.activeId == profileId) {
            return errorReply(id, QStringLiteral("Deactivate this profile before deleting it."));
        }
        const auto removed = m_store.remove(profileId, expected);
        if (!removed) {
            return errorReply(id, removed.error().message);
        }
        if (m_state.mode == Mode::Edit && m_state.editProfileId == profileId) {
            m_state = {};
        }
        m_state.output.clear();
        return successReply(id, stateData());
    }
    case Operation::Activate:
    case Operation::Edit:
        return useProfile(request);
    case Operation::Deactivate: {
        auto released = m_remapper.reset();
        m_remapper = {};
        m_state = {};
        m_state.output = std::move(released);
        return successReply(id, stateData());
    }
    case Operation::Reset:
        m_state.output = m_remapper.reset();
        return successReply(id, stateData());
    case Operation::Input:
        return input(request);
    case Operation::Hello:
    case Operation::Stop:
    case Operation::Profiles:
    case Operation::LoadProfile:
        return errorReply(id, QStringLiteral("Unexpected operation."));
    }
    return errorReply(id, QStringLiteral("Unknown operation."));
}

QJsonObject DemoSession::useProfile(const Request &request)
{
    if (!m_remapper.heldControls().isEmpty()) {
        return errorReply(request.id,
                          QStringLiteral("Release all controls before changing profiles."));
    }
    const auto profile = m_store.load(request.args.value(QStringLiteral("profileId")).toString());
    if (!profile) {
        return errorReply(request.id, profile.error().message);
    }
    const auto &saved = profile.value();
    if (request.operation == Operation::Edit) {
        m_remapper = {};
        m_state = {};
        m_state.mode = Mode::Edit;
        m_state.editProfileId = saved.id;
        auto data = stateData();
        data.insert(QStringLiteral("profile"), profileToJson(saved));
        return successReply(request.id, data);
    }
    if (saved.revision != request.args.value(QStringLiteral("expected")).toInteger()) {
        return errorReply(request.id,
                          QStringLiteral("The saved profile changed. Reload it and try again."));
    }
    const auto activated = m_remapper.activate(saved);
    if (!activated) {
        return errorReply(request.id, activated.error().message);
    }
    m_state = {};
    m_state.mode = Mode::Active;
    m_state.activeId = saved.id;
    m_state.activeName = saved.name;
    m_state.activeRevision = saved.revision;
    return successReply(request.id, stateData());
}

QJsonObject DemoSession::input(const Request &request)
{
    const auto control = request.args.value(QStringLiteral("control")).toString();
    if (!knownControl(control)) {
        return errorReply(request.id, QStringLiteral("Unknown control."));
    }
    if (m_state.mode != Mode::Active) {
        return errorReply(request.id,
                          QStringLiteral("Activate a demo profile before testing controls."));
    }

    const auto action = request.args.value(QStringLiteral("action")).toString();
    const bool tap = action == QStringLiteral("tap");
    const bool held = m_remapper.heldControls().contains(control);
    if (tap && held) {
        return errorReply(request.id, QStringLiteral("Release this control before tapping it."));
    }
    auto type = ControlEventType::Press;
    if (action == QStringLiteral("release") || (action == QStringLiteral("toggle") && held)) {
        type = ControlEventType::Release;
    } else if (action == QStringLiteral("repeat")) {
        type = ControlEventType::Repeat;
    }
    const auto resolution = m_remapper.consume({control, type});
    if (!resolution.valid) {
        return errorReply(request.id, resolution.error);
    }
    m_state.output = resolution.output;
    if (tap) {
        m_state.output.append(m_remapper.consume({control, ControlEventType::Release}).output);
    }
    auto data = stateData();
    data.insert(QStringLiteral("resolution"), resolutionToJson(resolution));
    return successReply(request.id, data);
}

} // namespace xkeys
