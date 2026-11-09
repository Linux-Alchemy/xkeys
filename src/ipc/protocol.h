#pragma once

#include "core/types.h"

#include <QJsonObject>

class QLocalSocket;

namespace xkeys::ipc
{

inline constexpr qint64 maxFrameBytes = 65536;
inline constexpr int timeoutMs = 3000;

enum class Frame { Incomplete, Object, Invalid };
enum class Operation { Hello, Resolve, Stop };

struct Request {
    QString id;
    Operation operation;
    ControlId control;
};

Frame readFrame(QLocalSocket &socket, QJsonObject &object);
bool sendFrame(QLocalSocket &socket, const QJsonObject &object);
QString parseRequest(const QJsonObject &object, Request &request);
QJsonObject makeRequest(const Request &request);
QJsonObject successReply(const QString &id);
QJsonObject errorReply(const QString &id, const QString &error);
QJsonObject resolutionReply(const QString &id, const Resolution &resolution);

// Returns false for an invalid envelope or a reply that does not match the request.
bool parseReply(const QJsonObject &object, const Request &request, QString &error,
                Resolution &resolution);

} // namespace xkeys::ipc
