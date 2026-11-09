#include "core/remapper.h"
#include "ipc/endpoint.h"
#include "ipc/protocol.h"

#include <QCommandLineParser>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QPointer>
#include <QTextStream>
#include <QTimer>
#include <QUuid>

#include <cstdio>

namespace
{

using namespace xkeys;
using namespace xkeys::ipc;

class DemoServer : public QObject
{
  public:
    explicit DemoServer(const QString &endpoint)
    {
        m_server.setSocketOptions(QLocalServer::UserAccessOption);
        m_server.setMaxPendingConnections(maxPeers);
        connect(&m_server, &QLocalServer::newConnection, this, &DemoServer::acceptPeers);
        if (!m_server.listen(endpoint)) {
            m_error = m_server.errorString();
        }
    }

    QString error() const { return m_error; }

  private:
    static constexpr int maxPeers = 16;

    void acceptPeers()
    {
        while (m_server.hasPendingConnections()) {
            QLocalSocket *socket = m_server.nextPendingConnection();
            if (m_peers >= maxPeers) {
                socket->abort();
                socket->deleteLater();
                continue;
            }

            ++m_peers;
            socket->setReadBufferSize(maxFrameBytes + 1);
            connect(socket, &QLocalSocket::disconnected, this, [this, socket] {
                if (m_testOwner == socket) {
                    m_testOwner.clear();
                }
                --m_peers;
                socket->deleteLater();
            });

            auto *deadline = new QTimer(socket);
            deadline->setSingleShot(true);
            connect(deadline, &QTimer::timeout, socket, &QLocalSocket::abort);
            deadline->start(timeoutMs);

            connect(socket, &QLocalSocket::readyRead, this,
                    [this, socket, deadline, greeted = false]() mutable {
                        consume(*socket, *deadline, greeted);
                    });
        }
    }

    void consume(QLocalSocket &socket, QTimer &deadline, bool &greeted)
    {
        QJsonObject object;
        while (socket.state() == QLocalSocket::ConnectedState) {
            const Frame frame = readFrame(socket, object);
            if (frame == Frame::Invalid) {
                socket.abort();
                return;
            }
            if (frame == Frame::Incomplete) {
                if (socket.bytesAvailable() > 0 && !deadline.isActive()) {
                    deadline.start(timeoutMs);
                }
                return;
            }

            if (greeted) {
                deadline.stop();
            }
            Request request{};
            const QString error = parseRequest(object, request);
            if (request.id.isEmpty()) {
                socket.abort();
                return;
            }
            if (!error.isEmpty()) {
                if (!sendFrame(socket, errorReply(request.id, error))) {
                    socket.abort();
                }
                continue;
            }

            if (request.operation == Operation::Hello) {
                greeted = true;
                deadline.stop();
                if (!sendFrame(socket, successReply(request.id))) {
                    socket.abort();
                }
            } else if (!greeted) {
                if (!sendFrame(socket,
                               errorReply(request.id, QStringLiteral("Send hello first.")))) {
                    socket.abort();
                }
            } else if (request.operation == Operation::Resolve) {
                if (!sendFrame(socket, resolve(socket, request))) {
                    socket.abort();
                }
            } else {
                stop(socket, request.id);
                return;
            }
        }
    }

    QJsonObject resolve(QLocalSocket &socket, const Request &request)
    {
        if (m_testOwner && m_testOwner != &socket) {
            return errorReply(request.id, QStringLiteral("Another editor is using the demo."));
        }

        const Resolution resolution = resolveDemo(request.control);
        if (resolution.valid) {
            m_testOwner = &socket;
        }
        return resolutionReply(request.id, resolution);
    }

    void stop(QLocalSocket &socket, const QString &id)
    {
        m_server.close();
        if (!sendFrame(socket, successReply(id))) {
            QCoreApplication::quit();
            return;
        }

        connect(&socket, &QLocalSocket::disconnected, qApp, &QCoreApplication::quit);
        QTimer::singleShot(timeoutMs, qApp, &QCoreApplication::quit);
        socket.disconnectFromServer();
    }

    QString m_error;
    QPointer<QLocalSocket> m_testOwner;
    int m_peers = 0;
    QLocalServer m_server;
};

QString requestStop(const QString &path)
{
    QLocalSocket socket;
    socket.setReadBufferSize(maxFrameBytes + 1);
    socket.connectToServer(path);
    if (!socket.waitForConnected(timeoutMs)) {
        return QStringLiteral("Cannot connect to the demo service: %1").arg(socket.errorString());
    }

    for (const Operation operation : {Operation::Hello, Operation::Stop}) {
        const Request request{QUuid::createUuid().toString(QUuid::WithoutBraces), operation, {}};
        if (!sendFrame(socket, makeRequest(request))) {
            return QStringLiteral("Cannot send the demo request.");
        }
        socket.flush();

        QElapsedTimer clock;
        clock.start();
        QJsonObject object;
        while (true) {
            const Frame frame = readFrame(socket, object);
            if (frame == Frame::Invalid) {
                return QStringLiteral("Invalid reply from the demo service.");
            }
            if (frame == Frame::Object) {
                break;
            }

            const qint64 remaining = timeoutMs - clock.elapsed();
            if (remaining <= 0 || !socket.waitForReadyRead(static_cast<int>(remaining))) {
                return QStringLiteral("Demo service did not acknowledge the request.");
            }
        }

        QString error;
        Resolution resolution;
        if (!parseReply(object, request, error, resolution)) {
            return QStringLiteral("Invalid reply from the demo service.");
        }
        if (!error.isEmpty()) {
            return error;
        }
    }
    return {};
}

} // namespace

int main(int argc, char **argv)
{
    QCoreApplication app(argc, argv);
    QCoreApplication::setApplicationName(QStringLiteral("xkeysd"));
    QTextStream diagnostic(stderr);

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Synthetic xkeys demo service"));
    parser.addHelpOption();
    const QCommandLineOption demo(QStringLiteral("demo"),
                                  QStringLiteral("Run the synthetic demo."));
    const QCommandLineOption stop(QStringLiteral("stop"), QStringLiteral("Stop the demo service."));
    const QCommandLineOption socket(QStringLiteral("socket"),
                                    QStringLiteral("Use a private absolute socket path."),
                                    QStringLiteral("path"));
    parser.addOptions({demo, stop, socket});
    parser.process(app);

    if (!parser.isSet(demo) || !parser.positionalArguments().isEmpty()) {
        diagnostic << "xkeysd requires --demo. Use --help for usage." << Qt::endl;
        return 2;
    }

    if (parser.isSet(socket) && parser.value(socket).isEmpty()) {
        diagnostic << "--socket requires a private absolute path." << Qt::endl;
        return 2;
    }
    QString error;
    const QString endpoint = demoEndpoint(parser.value(socket), error);
    if (endpoint.isEmpty()) {
        diagnostic << error << Qt::endl;
        return 2;
    }

    if (parser.isSet(stop)) {
        error = requestStop(endpoint);
        if (!error.isEmpty()) {
            diagnostic << error << Qt::endl;
            return 1;
        }
        return 0;
    }

    const QString lockPath = endpoint + QStringLiteral(".lock");
    const QFileInfo lockInfo(lockPath);
    if (lockInfo.isSymLink() || (lockInfo.exists() && !lockInfo.isFile())) {
        diagnostic << "The demo lock path is occupied by an unsafe file." << Qt::endl;
        return 1;
    }
    QLockFile lock(lockPath);
    lock.setStaleLockTime(0);
    if (!lock.tryLock(0)) {
        diagnostic << "Cannot lock the demo socket. A service may already be running." << Qt::endl;
        return 1;
    }
    if (!prepareListener(endpoint, error)) {
        diagnostic << error << Qt::endl;
        return 1;
    }

    DemoServer server(endpoint);
    if (!server.error().isEmpty()) {
        diagnostic << server.error() << Qt::endl;
        return 1;
    }
    return QCoreApplication::exec();
}
