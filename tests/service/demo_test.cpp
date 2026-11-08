#include "core/remapper.h"
#include "ipc/protocol.h"

#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QLocalSocket>
#include <QProcess>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

namespace
{

using namespace xkeys;
using namespace xkeys::ipc;

QString binary(const QString &name)
{
    return QCoreApplication::applicationDirPath() + QLatin1Char('/') + name;
}

QByteArray wire(const QJsonObject &object)
{
    return QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n';
}

bool write(QLocalSocket &socket, const QByteArray &data)
{
    return socket.write(data) == data.size() &&
           (socket.flush() || socket.bytesToWrite() == 0 || socket.waitForBytesWritten(timeoutMs));
}

QJsonObject receive(QLocalSocket &socket)
{
    QElapsedTimer clock;
    clock.start();
    QJsonObject object;
    while (true) {
        switch (readFrame(socket, object)) {
        case Frame::Object:
            return object;
        case Frame::Invalid:
            return {};
        case Frame::Incomplete:
            const qint64 remaining = timeoutMs - clock.elapsed();
            if (remaining <= 0 || !socket.waitForReadyRead(static_cast<int>(remaining))) {
                return {};
            }
        }
    }
}

QJsonObject exchange(QLocalSocket &socket, const Request &request)
{
    if (!write(socket, wire(makeRequest(request)))) {
        return {};
    }
    return receive(socket);
}

bool connectPeer(QLocalSocket &socket, const QString &path)
{
    socket.connectToServer(path);
    return socket.waitForConnected(timeoutMs);
}

bool greet(QLocalSocket &socket, const QString &path)
{
    const QString id = QStringLiteral("greeting");
    return connectPeer(socket, path) &&
           exchange(socket, {id, Operation::Hello, {}}) == successReply(id);
}

} // namespace

class ServiceDemoTest : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void cleanup();

    void resolverReturnsBoundBlankAndUnknown();
    void fragmentedAndCoalescedFrames();
    void rejectsMalformedRequests();
    void abortsInvalidFrames_data();
    void abortsInvalidFrames();
    void rejectsConcurrentTestAndReleasesOwnership();
    void protectsLiveEndpointAndStopsExplicitly();
    void requiresDemoAndOffersHelpWithoutDisplay();

  private:
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
    QProcess m_service;
};

void ServiceDemoTest::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->filePath(QStringLiteral("demo.sock"));
    m_service.start(binary(QStringLiteral("xkeysd")),
                    {QStringLiteral("--demo"), QStringLiteral("--socket"), m_path});
    QVERIFY2(m_service.waitForStarted(timeoutMs), qPrintable(m_service.errorString()));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(m_path), timeoutMs);
}

void ServiceDemoTest::cleanup()
{
    bool acknowledged = true;
    bool exited = true;
    if (m_service.state() != QProcess::NotRunning) {
        QLocalSocket socket;
        const Request stop{QStringLiteral("cleanup"), Operation::Stop, {}};
        acknowledged = greet(socket, m_path) && exchange(socket, stop) == successReply(stop.id);
        exited = m_service.state() == QProcess::NotRunning || m_service.waitForFinished(timeoutMs);
        if (!exited) {
            m_service.kill();
            m_service.waitForFinished(timeoutMs);
        }
    }
    m_dir.reset();
    const QByteArray stderrOutput = m_service.readAllStandardError();
    QVERIFY2(exited && m_service.exitStatus() == QProcess::NormalExit && m_service.exitCode() == 0,
             stderrOutput.constData());
    QVERIFY2(acknowledged, "The service did not acknowledge the cleanup stop request.");
}

void ServiceDemoTest::resolverReturnsBoundBlankAndUnknown()
{
    QCOMPARE(demoControls().size(), 28);
    const Resolution bound = resolveDemo(QStringLiteral("key01"));
    QVERIFY(bound.valid);
    QVERIFY(!bound.blank);
    QCOMPARE(bound.control, QStringLiteral("key01"));
    QCOMPARE(bound.label, QStringLiteral("Ctrl+Alt+E"));
    QCOMPARE(bound.layer, 1);
    QCOMPARE(bound.chord, (QList<int>{29, 56, 18}));
    QCOMPARE(resolveDemo(QStringLiteral("key02")).chord, (QList<int>{42, 33}));

    const Resolution blank = resolveDemo(QStringLiteral("wheelClick"));
    QVERIFY(blank.valid);
    QVERIFY(blank.blank);
    QCOMPARE(blank.label, QStringLiteral("Blank"));
    QVERIFY(blank.chord.isEmpty());

    const Resolution unknown = resolveDemo(QStringLiteral("key20"));
    QVERIFY(!unknown.valid);
    QVERIFY(!unknown.error.isEmpty());
}

void ServiceDemoTest::fragmentedAndCoalescedFrames()
{
    QLocalSocket socket;
    QVERIFY(connectPeer(socket, m_path));
    const Request hello{QStringLiteral("hello-partial"), Operation::Hello, {}};
    const QByteArray frame = wire(makeRequest(hello));
    QVERIFY(write(socket, frame.first(frame.size() / 2)));
    QVERIFY(!socket.waitForReadyRead(80));

    const Request first{QStringLiteral("first"), Operation::Resolve, QStringLiteral("key01")};
    const Request second{QStringLiteral("second"), Operation::Resolve, QStringLiteral("dpadUp")};
    QVERIFY(write(socket, frame.last(frame.size() - frame.size() / 2) + wire(makeRequest(first)) +
                              wire(makeRequest(second))));
    QCOMPARE(receive(socket), successReply(hello.id));

    QString error;
    Resolution resolution;
    const QJsonObject firstReply = receive(socket);
    QVERIFY(!parseReply(firstReply, second, error, resolution));
    QVERIFY(parseReply(firstReply, first, error, resolution));
    QCOMPARE(resolution.label, QStringLiteral("Ctrl+Alt+E"));
    QCOMPARE(resolution.chord, (QList<int>{29, 56, 18}));
    QVERIFY(error.isEmpty());
    QVERIFY(parseReply(receive(socket), second, error, resolution));
    QVERIFY(resolution.blank);
    QCOMPARE(resolution.control, second.control);
}

void ServiceDemoTest::rejectsMalformedRequests()
{
    QLocalSocket socket;
    QVERIFY(greet(socket, m_path));

    const QString id = QStringLiteral("invalid");
    const QJsonObject stop = makeRequest({id, Operation::Stop, {}});
    const auto checkRejected = [&](const QString &field, const QJsonValue &value) {
        QJsonObject invalid = stop;
        invalid.insert(field, value);
        QVERIFY(write(socket, wire(invalid)));
        const QJsonObject reply = receive(socket);
        QCOMPARE(reply.value(QStringLiteral("id")).toString(), id);
        QCOMPARE(reply.value(QStringLiteral("ok")), QJsonValue(false));
        QVERIFY(!reply.value(QStringLiteral("error")).toString().isEmpty());
    };
    checkRejected(QStringLiteral("version"), 2);
    checkRejected(QStringLiteral("version"), QStringLiteral("1"));
    checkRejected(QStringLiteral("op"), QStringLiteral("activate"));
    checkRejected(QStringLiteral("op"), false);
    checkRejected(QStringLiteral("control"), true);

    const QJsonObject unknown = exchange(socket, {id, Operation::Resolve, QStringLiteral("key20")});
    QCOMPARE(unknown, errorReply(id, QStringLiteral("Unknown control.")));
    QCOMPARE(m_service.state(), QProcess::Running);
}

void ServiceDemoTest::abortsInvalidFrames_data()
{
    QTest::addColumn<QByteArray>("input");
    QTest::newRow("invalid JSON") << QByteArray("{\n");
    QTest::newRow("invalid ID") << QByteArray("{\"id\":3,\"version\":1,\"op\":\"stop\"}\n");
    QTest::newRow("oversize without newline") << QByteArray(maxFrameBytes + 1, ' ');
    QTest::newRow("idle partial frame") << QByteArray("{");
}

void ServiceDemoTest::abortsInvalidFrames()
{
    QFETCH(QByteArray, input);
    QLocalSocket socket;
    QVERIFY(greet(socket, m_path));
    QVERIFY(write(socket, input));
    QVERIFY(socket.state() == QLocalSocket::UnconnectedState ||
            socket.waitForDisconnected(timeoutMs + 1000));
    QCOMPARE(m_service.state(), QProcess::Running);
}

void ServiceDemoTest::rejectsConcurrentTestAndReleasesOwnership()
{
    QLocalSocket first;
    QLocalSocket second;
    QVERIFY(greet(first, m_path));
    QVERIFY(greet(second, m_path));

    const Request unknown{QStringLiteral("unknown"), Operation::Resolve, QStringLiteral("key20")};
    QCOMPARE(exchange(first, unknown), errorReply(unknown.id, QStringLiteral("Unknown control.")));

    const Request resolveSecond{QStringLiteral("second"), Operation::Resolve,
                                QStringLiteral("key02")};
    QCOMPARE(exchange(second, resolveSecond),
             resolutionReply(resolveSecond.id, resolveDemo(resolveSecond.control)));

    const Request resolveFirst{QStringLiteral("first"), Operation::Resolve,
                               QStringLiteral("key01")};
    QCOMPARE(exchange(first, resolveFirst),
             errorReply(resolveFirst.id, QStringLiteral("Another editor is using the demo.")));

    second.abort();
    QTRY_COMPARE_WITH_TIMEOUT(exchange(first, resolveFirst),
                              resolutionReply(resolveFirst.id, resolveDemo(resolveFirst.control)),
                              timeoutMs);
}

void ServiceDemoTest::protectsLiveEndpointAndStopsExplicitly()
{
    QProcess duplicate;
    duplicate.start(binary(QStringLiteral("xkeysd")),
                    {QStringLiteral("--demo"), QStringLiteral("--socket"), m_path});
    QVERIFY(duplicate.waitForFinished(timeoutMs));
    QCOMPARE(duplicate.exitStatus(), QProcess::NormalExit);
    QVERIFY(duplicate.exitCode() != 0);
    QVERIFY(duplicate.readAllStandardError().contains("Cannot lock the demo socket"));

    QLocalSocket first;
    QVERIFY(greet(first, m_path));
    first.disconnectFromServer();
    QCOMPARE(m_service.state(), QProcess::Running);

    QProcess stop;
    stop.start(
        binary(QStringLiteral("xkeysd")),
        {QStringLiteral("--demo"), QStringLiteral("--stop"), QStringLiteral("--socket"), m_path});
    QVERIFY(stop.waitForFinished(timeoutMs));
    QCOMPARE(stop.exitStatus(), QProcess::NormalExit);
    QCOMPARE(stop.exitCode(), 0);
    if (m_service.state() != QProcess::NotRunning) {
        QVERIFY(m_service.waitForFinished(timeoutMs));
    }
    QCOMPARE(m_service.exitStatus(), QProcess::NormalExit);
    QCOMPARE(m_service.exitCode(), 0);
    QVERIFY(!QFileInfo::exists(m_path));
}

void ServiceDemoTest::requiresDemoAndOffersHelpWithoutDisplay()
{
    for (const auto &name : {QStringLiteral("xkeysd"), QStringLiteral("xkeys")}) {
        QProcess process;
        auto environment = QProcessEnvironment::systemEnvironment();
        environment.remove(QStringLiteral("WAYLAND_DISPLAY"));
        environment.remove(QStringLiteral("DISPLAY"));
        environment.remove(QStringLiteral("QT_FORCE_STDERR_LOGGING"));
        process.setProcessEnvironment(environment);
        process.start(binary(name), QStringList{});
        QVERIFY(process.waitForFinished(timeoutMs));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 2);
        QVERIFY(process.readAllStandardError().contains("requires --demo"));

        process.start(binary(name), {QStringLiteral("--help")});
        QVERIFY(process.waitForFinished(timeoutMs));
        QCOMPARE(process.exitStatus(), QProcess::NormalExit);
        QCOMPARE(process.exitCode(), 0);
        QVERIFY(process.readAllStandardOutput().contains("--demo"));
    }
}

QTEST_GUILESS_MAIN(ServiceDemoTest)
#include "demo_test.moc"
