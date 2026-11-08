#pragma once

#include "core/types.h"

#include <QJsonObject>
#include <QStringList>

class QLocalSocket;

namespace xkeys::ipc
{

inline constexpr qint64 maxFrameBytes = 65536;
inline constexpr int timeoutMs = 3000;

enum class Frame { Incomplete, Object, Invalid };
enum class Operation {
    Hello,
    Stop,
    Profiles,
    CreateProfile,
    LoadProfile,
    SaveProfile,
    RenameProfile,
    DeleteProfile,
    Activate,
    Deactivate,
    Edit,
    Input,
    Reset
};

struct Request {
    QString id;
    Operation operation;
    QJsonObject args;
};

enum class Mode { Inactive, Edit, Active };

struct Snapshot {
    Mode mode = Mode::Inactive;
    ProfileId activeId;
    QString activeName;
    Revision activeRevision = 0;
    ProfileId editProfileId;
    int activeLayer = 1;
    QStringList heldControls;
    QList<int> ownedKeys;
    OutputBatch output;
};

Frame readFrame(QLocalSocket &socket, QJsonObject &object);
bool sendFrame(QLocalSocket &socket, const QJsonObject &object);
QString parseRequest(const QJsonObject &object, Request &request);
QJsonObject makeRequest(const Request &request);
QJsonObject successReply(const QString &id, const QJsonObject &data = {});
QJsonObject errorReply(const QString &id, const QString &error);
bool parseReply(const QJsonObject &object, const Request &request, QString &error,
                QJsonObject &data);

bool knownControl(const ControlId &id);
QString modeName(Mode mode);
QJsonObject snapshotToJson(const Snapshot &snapshot);
std::optional<Snapshot> snapshotFromJson(const QJsonObject &object);
QJsonObject resolutionToJson(const Resolution &resolution);
std::optional<Resolution> resolutionFromJson(const QJsonObject &object);
QJsonObject metadataToJson(const Profile &profile);
bool validMetadata(const QJsonObject &object);

} // namespace xkeys::ipc
