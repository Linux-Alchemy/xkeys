#include "protocol.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocalSocket>
#include <linux/input-event-codes.h>

namespace xkeys::ipc
{

namespace
{
constexpr int version = 1;
constexpr qsizetype maxIdLength = 64;

bool validId(const QJsonValue &value)
{
    const QString id = value.toString();
    return !id.isEmpty() && id.size() <= maxIdLength;
}

QString operationName(Operation operation)
{
    switch (operation) {
    case Operation::Hello:
        return QStringLiteral("hello");
    case Operation::Resolve:
        return QStringLiteral("resolve");
    case Operation::Stop:
        return QStringLiteral("stop");
    }

    return {};
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
    const QJsonValue id = object.value(QStringLiteral("id"));
    if (!validId(id)) {
        return QStringLiteral("Invalid request ID.");
    }

    request.id = id.toString();
    if (object.value(QStringLiteral("version")) != version) {
        return QStringLiteral("Unsupported protocol version.");
    }

    const QJsonValue operation = object.value(QStringLiteral("op"));
    if (operation == operationName(Operation::Hello)) {
        request.operation = Operation::Hello;
    } else if (operation == operationName(Operation::Resolve)) {
        request.operation = Operation::Resolve;
    } else if (operation == operationName(Operation::Stop)) {
        request.operation = Operation::Stop;
    } else {
        return QStringLiteral("Unknown operation.");
    }

    if (request.operation == Operation::Resolve) {
        const QJsonValue control = object.value(QStringLiteral("control"));
        if (object.size() != 4 || !validId(control)) {
            return QStringLiteral("Invalid resolve request.");
        }
        request.control = control.toString();
    } else if (object.size() != 3) {
        return QStringLiteral("Unexpected request fields.");
    }

    return {};
}

QJsonObject makeRequest(const Request &request)
{
    QJsonObject object{{QStringLiteral("version"), version},
                       {QStringLiteral("id"), request.id},
                       {QStringLiteral("op"), operationName(request.operation)}};
    if (request.operation == Operation::Resolve) {
        object.insert(QStringLiteral("control"), request.control);
    }
    return object;
}

QJsonObject successReply(const QString &id)
{
    return {{QStringLiteral("version"), version},
            {QStringLiteral("id"), id},
            {QStringLiteral("ok"), true}};
}

QJsonObject errorReply(const QString &id, const QString &error)
{
    QJsonObject reply = successReply(id);
    reply.insert(QStringLiteral("ok"), false);
    reply.insert(QStringLiteral("error"), error);
    return reply;
}

QJsonObject resolutionReply(const QString &id, const Resolution &resolution)
{
    if (!resolution.valid) {
        return errorReply(id, resolution.error);
    }

    QJsonArray chord;
    for (const int code : resolution.chord) {
        chord.append(code);
    }

    QJsonObject reply = successReply(id);
    reply.insert(QStringLiteral("control"), resolution.control);
    reply.insert(QStringLiteral("layer"), resolution.layer);
    reply.insert(QStringLiteral("label"), resolution.label);
    reply.insert(QStringLiteral("chord"), chord);
    reply.insert(QStringLiteral("blank"), resolution.blank);
    return reply;
}

bool parseReply(const QJsonObject &object, const Request &request, QString &error,
                Resolution &resolution)
{
    const QJsonValue ok = object.value(QStringLiteral("ok"));
    if (object.value(QStringLiteral("version")) != version ||
        object.value(QStringLiteral("id")) != request.id || !ok.isBool()) {
        return false;
    }

    if (!ok.toBool()) {
        error = object.value(QStringLiteral("error")).toString();
        return object.size() == 4 && !error.isEmpty();
    }

    if (request.operation != Operation::Resolve) {
        return object.size() == 3;
    }

    const QJsonValue blank = object.value(QStringLiteral("blank"));
    const QJsonValue chord = object.value(QStringLiteral("chord"));
    const QString label = object.value(QStringLiteral("label")).toString();
    if (object.size() != 8 || object.value(QStringLiteral("control")) != request.control ||
        object.value(QStringLiteral("layer")) != 1 || !blank.isBool() || !chord.isArray() ||
        label.isEmpty()) {
        return false;
    }

    const QJsonArray codes = chord.toArray();
    if (codes.size() > 16 || codes.isEmpty() != blank.toBool()) {
        return false;
    }

    QList<int> keys;
    for (const QJsonValue code : codes) {
        const int key = code.toInt();
        if (key <= 0 || key > KEY_MAX) {
            return false;
        }
        keys.append(key);
    }

    resolution = {request.control, 1, label, keys, blank.toBool(), true, {}};
    return true;
}

} // namespace xkeys::ipc
