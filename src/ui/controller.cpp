#include "controller.h"

#include "core/remapper.h"
#include "ipc/endpoint.h"

#include <QCoreApplication>
#include <QProcess>
#include <QUuid>
#include <QVariantMap>

#include <utility>

namespace xkeys
{

using namespace ipc;

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

void Controller::startDemo(const QString &socketPath)
{
    setState(State::Disconnected);
    clearPending();
    m_retry.stop();
    m_socket.abort();
    m_socketOverride = socketPath;
    m_launched = false;
    QString error;
    m_endpoint = demoEndpoint(socketPath, error);
    setError(error);
    if (m_endpoint.isEmpty()) {
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
QString Controller::selectedControl() const { return m_resolution.control; }
QString Controller::bindingLabel() const { return m_resolution.label; }
int Controller::selectedLayer() const { return m_resolution.layer; }
bool Controller::blank() const { return m_resolution.blank; }

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

void Controller::selectControl(const QString &controlId)
{
    if (connected() && !pending()) {
        send(Operation::Resolve, controlId);
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
        startDemo(socketPath);
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
                    service, {QStringLiteral("--demo"), QStringLiteral("--socket"), m_endpoint})) {
                fail(QStringLiteral("Could not launch the demo service."));
            }
        }
        return;
    }

    if (error != QLocalSocket::PeerClosedError) {
        fail(m_socket.errorString());
    }
}

void Controller::send(Operation operation, const QString &control)
{
    setError({});
    m_pending = Request{QUuid::createUuid().toString(QUuid::WithoutBraces), operation, control};
    emit pendingChanged();
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
    Resolution resolution;
    if (frame == Frame::Invalid || !m_pending ||
        !parseReply(object, *m_pending, error, resolution) || m_socket.bytesAvailable() != 0) {
        fail(QStringLiteral("Unexpected reply from the demo service."));
        return;
    }

    const Operation operation = m_pending->operation;
    clearPending();
    if (!error.isEmpty()) {
        if (operation == Operation::Hello) {
            fail(error);
        } else {
            setError(error);
        }
    } else if (operation == Operation::Hello) {
        setState(State::Ready);
    } else if (operation == Operation::Stop) {
        setState(State::Disconnected);
        m_socket.disconnectFromServer();
    } else {
        m_resolution = std::move(resolution);
        emit resolutionChanged();
    }
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
