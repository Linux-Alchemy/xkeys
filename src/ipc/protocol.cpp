#include "protocol.h"

#include "core/profile_store.h"
#include "core/remapper.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <linux/input-event-codes.h>

namespace xkeys::ipc
{

namespace
{
constexpr int version = 2;
constexpr qsizetype maxIdLength = 64;
constexpr qsizetype maxOutputEvents = 512;
constexpr std::pair<Operation, QLatin1StringView> operations[] = {
    {Operation::Hello, QLatin1StringView("hello")},
    {Operation::Stop, QLatin1StringView("stop")},
    {Operation::Profiles, QLatin1StringView("profiles")},
    {Operation::CreateProfile, QLatin1StringView("create")},
    {Operation::LoadProfile, QLatin1StringView("load")},
    {Operation::SaveProfile, QLatin1StringView("save")},
    {Operation::RenameProfile, QLatin1StringView("rename")},
    {Operation::DeleteProfile, QLatin1StringView("delete")},
    {Operation::Activate, QLatin1StringView("activate")},
    {Operation::Deactivate, QLatin1StringView("deactivate")},
    {Operation::Edit, QLatin1StringView("edit")},
    {Operation::Input, QLatin1StringView("input")},
    {Operation::Reset, QLatin1StringView("reset")},
};

bool validId(const QJsonValue &value)
{
    const QString id = value.toString();
    return !id.isEmpty() && id.size() <= maxIdLength;
}

bool validRevision(const QJsonValue &value)
{
    const auto revision = value.toInteger(-1);
    return revision >= 0 && revision <= maxRevision;
}

bool validArgs(const Request &request)
{
    const auto &args = request.args;
    const auto id = args.value(QStringLiteral("profileId"));
    const auto name = args.value(QStringLiteral("name"));
    const auto expected = args.value(QStringLiteral("expected"));
    switch (request.operation) {
    case Operation::Hello:
    case Operation::Stop:
    case Operation::Profiles:
    case Operation::Deactivate:
    case Operation::Reset:
        return args.isEmpty();
    case Operation::CreateProfile:
        return args.size() == 1 && name.isString();
    case Operation::LoadProfile:
    case Operation::Edit:
        return args.size() == 1 && id.isString();
    case Operation::SaveProfile:
        return args.size() == 2 && args.value(QStringLiteral("profile")).isObject() &&
               validRevision(expected);
    case Operation::RenameProfile:
        return args.size() == 3 && id.isString() && name.isString() && validRevision(expected);
    case Operation::DeleteProfile:
    case Operation::Activate:
        return args.size() == 2 && id.isString() && validRevision(expected);
    case Operation::Input: {
        const auto action = args.value(QStringLiteral("action"));
        return args.size() == 2 && args.value(QStringLiteral("control")).isString() &&
               (action == QStringLiteral("press") || action == QStringLiteral("release") ||
                action == QStringLiteral("repeat") || action == QStringLiteral("tap") ||
                action == QStringLiteral("toggle"));
    }
    }
    return false;
}

int supportedKey(const QJsonValue &value)
{
    const int code = value.toInt(-1);
    return code > KEY_RESERVED && code < BTN_MISC && code != KEY_UNKNOWN ? code : -1;
}

QJsonArray keysToJson(const QList<int> &keys)
{
    QJsonArray result;
    for (const int key : keys) {
        result.append(key);
    }
    return result;
}

std::optional<QList<int>> keysFromJson(const QJsonValue &value)
{
    if (!value.isArray()) {
        return std::nullopt;
    }
    QSet<int> seen;
    QList<int> keys;
    const auto array = value.toArray();
    for (const auto item : array) {
        const int code = supportedKey(item);
        if (code == -1 || seen.contains(code)) {
            return std::nullopt;
        }
        seen.insert(code);
        keys.append(code);
    }
    return keys;
}

QString transitionName(KeyTransition transition)
{
    return transition == KeyTransition::Down ? QStringLiteral("down") : QStringLiteral("up");
}
} // namespace

Frame readFrame(QLocalSocket &socket, QJsonObject &object)
{
    if (!socket.canReadLine()) {
        return socket.bytesAvailable() > maxFrameBytes ? Frame::Invalid : Frame::Incomplete;
    }

    const QByteArray line = socket.readLine(maxFrameBytes + 2);
    if (!line.endsWith('\n') || line.size() > maxFrameBytes + 1) {
        return Frame::Invalid;
    }

    const QJsonDocument document = QJsonDocument::fromJson(line);
    if (!document.isObject()) {
        return Frame::Invalid;
    }

    object = document.object();
    return Frame::Object;
}

bool sendFrame(QLocalSocket &socket, const QJsonObject &object)
{
    QByteArray frame = QJsonDocument(object).toJson(QJsonDocument::Compact);
    if (frame.size() > maxFrameBytes ||
        socket.bytesToWrite() + frame.size() + 1 > maxFrameBytes + 1) {
        return false;
    }

    frame.append('\n');
    return socket.write(frame) == frame.size();
}

QString parseRequest(const QJsonObject &object, Request &request)
{
    request = {};
    const QJsonValue id = object.value(QStringLiteral("id"));
    if (!validId(id)) {
        return QStringLiteral("Invalid request ID.");
    }
    request.id = id.toString();
    if (object.value(QStringLiteral("version")) != version) {
        return QStringLiteral("Unsupported protocol version.");
    }

    const auto operation = object.value(QStringLiteral("op"));
    const auto item = std::ranges::find_if(
        operations, [&operation](const auto &entry) { return operation == entry.second; });
    if (item == std::end(operations)) {
        return QStringLiteral("Unknown operation.");
    }
    const auto args = object.value(QStringLiteral("args"));
    if (object.size() != 4 || !args.isObject()) {
        return QStringLiteral("Invalid request fields.");
    }
    request.operation = item->first;
    request.args = args.toObject();
    return validArgs(request) ? QString{} : QStringLiteral("Invalid operation arguments.");
}

QJsonObject makeRequest(const Request &request)
{
    const auto item = std::ranges::find(operations, request.operation,
                                        &std::pair<Operation, QLatin1StringView>::first);
    return {
        {QStringLiteral("version"), version},
        {QStringLiteral("id"), request.id},
        {QStringLiteral("op"), item == std::end(operations) ? QString{} : QString(item->second)},
        {QStringLiteral("args"), request.args}};
}

QJsonObject successReply(const QString &id, const QJsonObject &data)
{
    return {{QStringLiteral("version"), version},
            {QStringLiteral("id"), id},
            {QStringLiteral("ok"), true},
            {QStringLiteral("data"), data}};
}

QJsonObject errorReply(const QString &id, const QString &error)
{
    return {{QStringLiteral("version"), version},
            {QStringLiteral("id"), id},
            {QStringLiteral("ok"), false},
            {QStringLiteral("error"), error}};
}

bool parseReply(const QJsonObject &object, const Request &request, QString &error,
                QJsonObject &data)
{
    error.clear();
    data = {};
    const auto ok = object.value(QStringLiteral("ok"));
    if (object.size() != 4 || object.value(QStringLiteral("version")) != version ||
        object.value(QStringLiteral("id")) != request.id || !ok.isBool()) {
        return false;
    }

    if (!ok.toBool()) {
        error = object.value(QStringLiteral("error")).toString();
        return !error.trimmed().isEmpty();
    }

    const auto result = object.value(QStringLiteral("data"));
    data = result.toObject();
    return result.isObject();
}

bool knownControl(const ControlId &id)
{
    return std::ranges::find(demoControls(), id, &Control::id) != demoControls().end();
}

QString modeName(Mode mode)
{
    switch (mode) {
    case Mode::Inactive:
        return QStringLiteral("Inactive");
    case Mode::Edit:
        return QStringLiteral("Edit");
    case Mode::Active:
        return QStringLiteral("Active");
    }
    return {};
}

QJsonObject snapshotToJson(const Snapshot &snapshot)
{
    QJsonArray output;
    for (const auto &event : snapshot.output) {
        output.append(
            QJsonObject{{QStringLiteral("code"), event.code},
                        {QStringLiteral("transition"), transitionName(event.transition)}});
    }
    return {{QStringLiteral("mode"), modeName(snapshot.mode)},
            {QStringLiteral("activeId"), snapshot.activeId},
            {QStringLiteral("activeName"), snapshot.activeName},
            {QStringLiteral("activeRevision"), snapshot.activeRevision},
            {QStringLiteral("editProfileId"), snapshot.editProfileId},
            {QStringLiteral("activeLayer"), snapshot.activeLayer},
            {QStringLiteral("heldControls"), QJsonArray::fromStringList(snapshot.heldControls)},
            {QStringLiteral("ownedKeys"), keysToJson(snapshot.ownedKeys)},
            {QStringLiteral("output"), output}};
}

std::optional<Snapshot> snapshotFromJson(const QJsonObject &object)
{
    const auto mode = object.value(QStringLiteral("mode"));
    const auto id = object.value(QStringLiteral("activeId"));
    const auto name = object.value(QStringLiteral("activeName"));
    const auto revision = object.value(QStringLiteral("activeRevision"));
    const auto editId = object.value(QStringLiteral("editProfileId"));
    const auto layer = object.value(QStringLiteral("activeLayer"));
    const auto held = object.value(QStringLiteral("heldControls"));
    const auto keys = keysFromJson(object.value(QStringLiteral("ownedKeys")));
    const auto output = object.value(QStringLiteral("output"));
    if (object.size() != 9 || !id.isString() || !name.isString() || !validRevision(revision) ||
        !editId.isString() || (layer != 1 && layer != 2) || !held.isArray() || !keys ||
        !output.isArray() || output.toArray().size() > maxOutputEvents) {
        return std::nullopt;
    }

    Snapshot result;
    if (mode == modeName(Mode::Active)) {
        result.mode = Mode::Active;
        auto profile = makeBlankProfile(id.toString(), name.toString());
        profile.revision = revision.toInteger();
        if (profile.revision == 0 || !validateProfile(profile) || !editId.toString().isEmpty()) {
            return std::nullopt;
        }
    } else {
        if (!id.toString().isEmpty() || !name.toString().isEmpty() || revision != 0 || layer != 1 ||
            !held.toArray().isEmpty() || !keys->isEmpty()) {
            return std::nullopt;
        }
        if (mode == modeName(Mode::Edit)) {
            result.mode = Mode::Edit;
            if (QUuid::fromString(editId.toString()).toString(QUuid::WithoutBraces) != editId ||
                !output.toArray().isEmpty()) {
                return std::nullopt;
            }
        } else if (mode != modeName(Mode::Inactive) || !editId.toString().isEmpty()) {
            return std::nullopt;
        }
    }

    result.activeId = id.toString();
    result.activeName = name.toString();
    result.activeRevision = revision.toInteger();
    result.editProfileId = editId.toString();
    result.activeLayer = layer.toInt();
    const auto heldArray = held.toArray();
    for (const auto item : heldArray) {
        const auto control = item.toString();
        if (!knownControl(control) || result.heldControls.contains(control) ||
            control == QStringLiteral("wheelUp") || control == QStringLiteral("wheelDown")) {
            return std::nullopt;
        }
        result.heldControls.append(control);
    }
    if ((result.activeLayer == 2 && !result.heldControls.contains(QStringLiteral("thumb"))) ||
        (result.heldControls.isEmpty() && !keys->isEmpty())) {
        return std::nullopt;
    }
    result.ownedKeys = *keys;

    const auto outputArray = output.toArray();
    for (const auto item : outputArray) {
        const auto event = item.toObject();
        const int code = supportedKey(event.value(QStringLiteral("code")));
        const auto transition = event.value(QStringLiteral("transition"));
        const bool down = transition == transitionName(KeyTransition::Down);
        if (event.size() != 2 || code == -1 ||
            (!down && transition != transitionName(KeyTransition::Up)) ||
            (down && result.mode != Mode::Active)) {
            return std::nullopt;
        }
        result.output.append({code, down ? KeyTransition::Down : KeyTransition::Up});
    }
    // Rewind this batch from the reported final keyboard state. Each key must
    // alternate Down and Up, and its final transition must match ownership.
    QSet<int> owned(keys->begin(), keys->end());
    for (auto it = result.output.crbegin(); it != result.output.crend(); ++it) {
        if (it->transition == KeyTransition::Down) {
            if (!owned.remove(it->code)) {
                return std::nullopt;
            }
        } else {
            if (owned.contains(it->code)) {
                return std::nullopt;
            }
            owned.insert(it->code);
        }
    }
    return result;
}

QJsonObject resolutionToJson(const Resolution &resolution)
{
    return {{QStringLiteral("control"), resolution.control},
            {QStringLiteral("layer"), resolution.layer},
            {QStringLiteral("label"), resolution.label},
            {QStringLiteral("chord"), keysToJson(resolution.chord)},
            {QStringLiteral("blank"), resolution.blank},
            {QStringLiteral("layerControl"), resolution.layerControl}};
}

std::optional<Resolution> resolutionFromJson(const QJsonObject &object)
{
    const auto control = object.value(QStringLiteral("control")).toString();
    const auto layer = object.value(QStringLiteral("layer"));
    const auto label = object.value(QStringLiteral("label")).toString();
    const auto keys = keysFromJson(object.value(QStringLiteral("chord")));
    const auto blank = object.value(QStringLiteral("blank"));
    const auto layerControl = object.value(QStringLiteral("layerControl"));
    if (object.size() != 6 || !knownControl(control) || (layer != 1 && layer != 2) || !keys ||
        keys->size() > 16 || !blank.isBool() || !layerControl.isBool()) {
        return std::nullopt;
    }
    if (layerControl.toBool()) {
        if (control != QStringLiteral("thumb") || blank.toBool() || !keys->isEmpty() ||
            label != QStringLiteral("Layer %1").arg(layer.toInt())) {
            return std::nullopt;
        }
    } else if (blank.toBool() != keys->isEmpty() || label != chordLabel(*keys)) {
        return std::nullopt;
    }

    return Resolution{control, layer.toInt(),         label, *keys, blank.toBool(), true,
                      {},      layerControl.toBool(), {}};
}

QJsonObject metadataToJson(const Profile &profile)
{
    return {{QStringLiteral("id"), profile.id},
            {QStringLiteral("name"), profile.name},
            {QStringLiteral("revision"), profile.revision},
            {QStringLiteral("thumbLayer"), profile.thumbLayer}};
}

bool validMetadata(const QJsonObject &object)
{
    const auto revision = object.value(QStringLiteral("revision")).toInteger(-1);
    if (object.size() != 4 || revision <= 0 ||
        !object.value(QStringLiteral("thumbLayer")).isBool()) {
        return false;
    }
    auto profile = makeBlankProfile(object.value(QStringLiteral("id")).toString(),
                                    object.value(QStringLiteral("name")).toString());
    profile.revision = revision;
    return static_cast<bool>(validateProfile(profile));
}

} // namespace xkeys::ipc
