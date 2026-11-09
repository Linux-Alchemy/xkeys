#include "core/profile_store.h"
#include "ipc/protocol.h"

#include <QElapsedTimer>
#include <QFileInfo>
#include <QJsonArray>
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
    return write(socket, wire(makeRequest(request))) ? receive(socket) : QJsonObject{};
}

QJsonObject request(QLocalSocket &socket, Operation operation, const QJsonObject &args = {})
{
    return exchange(socket, {QStringLiteral("request"), operation, args});
}

QJsonObject data(const QJsonObject &reply)
{
    return reply.value(QStringLiteral("data")).toObject();
}

QString error(const QJsonObject &reply) { return reply.value(QStringLiteral("error")).toString(); }

std::optional<Snapshot> state(const QJsonObject &reply)
{
    return snapshotFromJson(data(reply).value(QStringLiteral("state")).toObject());
}

Result<Profile> profile(const QJsonObject &reply)
{
    return profileFromJson(data(reply).value(QStringLiteral("profile")).toObject());
}

bool connectPeer(QLocalSocket &socket, const QString &path)
{
    socket.connectToServer(path);
    return socket.waitForConnected(timeoutMs);
}

bool greet(QLocalSocket &socket, const QString &path)
{
    return connectPeer(socket, path) && state(request(socket, Operation::Hello)).has_value();
}

QJsonObject profileId(const Profile &profile)
{
    return {{QStringLiteral("profileId"), profile.id}};
}

QJsonObject versionedProfile(const Profile &profile)
{
    auto args = profileId(profile);
    args.insert(QStringLiteral("expected"), profile.revision);
    return args;
}

QJsonObject saveArgs(const Profile &profile)
{
    return {{QStringLiteral("profile"), profileToJson(profile)},
            {QStringLiteral("expected"), profile.revision}};
}

QJsonObject inputArgs(const QString &control, const QString &action)
{
    return {{QStringLiteral("control"), control}, {QStringLiteral("action"), action}};
}

QJsonObject create(QLocalSocket &socket, const QString &name)
{
    return request(socket, Operation::CreateProfile, {{QStringLiteral("name"), name}});
}

} // namespace

class ServiceDemoTest : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void cleanup();

    void profileLifecycleAndRevisionChecks();
    void simulatesBoundBlankAndUnknown();
    void fragmentedAndCoalescedFrames();
    void rejectsMalformedRequests();
    void rejectsMalformedReplies();
    void abortsInvalidFrames_data();
    void abortsInvalidFrames();
    void releasesLeaseAndHeldOutputsOnDisconnect();
    void preservesProfilesAcrossServiceRestart();
    void protectsLiveEndpointAndStopsExplicitly();
    void requiresDemoAndOffersHelpWithoutDisplay();

  private:
    void startService(const QStringList &extra = {});
    void stopService();
    std::unique_ptr<QTemporaryDir> m_dir;
    QString m_path;
    QProcess m_service;
};

void ServiceDemoTest::init()
{
    m_dir = std::make_unique<QTemporaryDir>();
    QVERIFY(m_dir->isValid());
    m_path = m_dir->filePath(QStringLiteral("demo.sock"));
    startService();
}

void ServiceDemoTest::startService(const QStringList &extra)
{
    m_service.start(binary(QStringLiteral("xkeysd")),
                    QStringList{QStringLiteral("--demo"), QStringLiteral("--socket"), m_path} +
                        extra);
    QVERIFY2(m_service.waitForStarted(timeoutMs), qPrintable(m_service.errorString()));
    QTRY_VERIFY_WITH_TIMEOUT(QFileInfo::exists(m_path), timeoutMs);
}

void ServiceDemoTest::stopService()
{
    bool acknowledged = true;
    bool exited = true;
    if (m_service.state() != QProcess::NotRunning) {
        QLocalSocket socket;
        acknowledged = greet(socket, m_path) &&
                       request(socket, Operation::Stop) == successReply(QStringLiteral("request"));
        exited = m_service.state() == QProcess::NotRunning || m_service.waitForFinished(timeoutMs);
        if (!exited) {
            m_service.kill();
            m_service.waitForFinished(timeoutMs);
        }
    }
    const QByteArray stderrOutput = m_service.readAllStandardError();
    QVERIFY2(exited && m_service.exitStatus() == QProcess::NormalExit && m_service.exitCode() == 0,
             stderrOutput.constData());
    QVERIFY2(acknowledged, "The service did not acknowledge the cleanup stop request.");
}

void ServiceDemoTest::cleanup()
{
    stopService();
    m_dir.reset();
}

void ServiceDemoTest::profileLifecycleAndRevisionChecks()
{
    QLocalSocket socket;
    QVERIFY(greet(socket, m_path));
    QVERIFY(!QFileInfo::exists(m_dir->filePath(QStringLiteral("profiles"))));
    QCOMPARE(data(request(socket, Operation::Profiles)).value(QStringLiteral("profiles")),
             QJsonValue(QJsonArray{}));

    auto created = profile(create(socket, QStringLiteral("Alpha")));
    QVERIFY(created);
    auto alpha = created.value();
    auto expected = makeBlankProfile(alpha.id, QStringLiteral("Alpha"));
    expected.revision = 1;
    QCOMPARE(alpha, expected);
    auto current = state(request(socket, Operation::Hello));
    QVERIFY(current);
    QCOMPARE(current->mode, Mode::Inactive);
    QVERIFY(!error(request(socket, Operation::SaveProfile, saveArgs(alpha))).isEmpty());

    const auto loaded = profile(request(socket, Operation::LoadProfile, profileId(alpha)));
    QVERIFY(loaded);
    QCOMPARE(loaded.value(), alpha);
    current = state(request(socket, Operation::Hello));
    QVERIFY(current);
    QCOMPARE(current->mode, Mode::Inactive);
    const auto edit = request(socket, Operation::Edit, profileId(alpha));
    QVERIFY(state(edit));
    QCOMPARE(state(edit)->mode, Mode::Edit);
    QCOMPARE(state(edit)->editProfileId, alpha.id);

    alpha.layers[0][QStringLiteral("key01")] = {QStringLiteral("Break even"), {29, 56, 18}};
    const auto edited = profile(request(socket, Operation::SaveProfile, saveArgs(alpha)));
    QVERIFY(edited);
    QCOMPARE(edited.value().revision, 2);
    current = state(request(socket, Operation::Hello));
    QVERIFY(current);
    QCOMPARE(current->mode, Mode::Edit);
    QVERIFY(current->activeId.isEmpty());

    alpha.layers[0][QStringLiteral("key01")] = {};
    QVERIFY(error(request(socket, Operation::SaveProfile, saveArgs(alpha))).contains("changed"));
    const auto afterStaleSave = profile(request(socket, Operation::LoadProfile, profileId(alpha)));
    QVERIFY(afterStaleSave);
    QCOMPARE(afterStaleSave.value(), edited.value());
    QVERIFY(
        error(request(socket, Operation::Activate, versionedProfile(alpha))).contains("changed"));

    const auto active =
        state(request(socket, Operation::Activate, versionedProfile(edited.value())));
    QVERIFY(active);
    QCOMPARE(active->mode, Mode::Active);
    QCOMPARE(active->activeId, alpha.id);
    QCOMPARE(active->activeRevision, 2);
    QVERIFY(error(request(socket, Operation::DeleteProfile, versionedProfile(edited.value())))
                .contains("Deactivate"));

    auto rename = versionedProfile(edited.value());
    rename.insert(QStringLiteral("name"), QStringLiteral("Renamed"));
    const auto renamed = profile(request(socket, Operation::RenameProfile, rename));
    QVERIFY(renamed);
    QCOMPARE(renamed.value().revision, 3);
    QCOMPARE(renamed.value().name, QStringLiteral("Renamed"));
    const auto originalActive = state(request(socket, Operation::Hello));
    QVERIFY(originalActive);
    QCOMPARE(originalActive->activeRevision, 2);
    QCOMPARE(originalActive->activeName, QStringLiteral("Alpha"));

    const auto beta = profile(create(socket, QStringLiteral("Beta")));
    QVERIFY(beta);
    QVERIFY(profile(request(socket, Operation::LoadProfile, profileId(beta.value()))));
    const auto afterSelection = state(request(socket, Operation::Hello));
    QVERIFY(afterSelection);
    QCOMPARE(afterSelection->activeId, alpha.id);
    const auto inactive = state(request(socket, Operation::Deactivate));
    QVERIFY(inactive);
    QCOMPARE(inactive->mode, Mode::Inactive);

    const auto editRenamed = state(request(socket, Operation::Edit, profileId(renamed.value())));
    QVERIFY(editRenamed);
    QCOMPARE(editRenamed->mode, Mode::Edit);
    const auto deleted =
        state(request(socket, Operation::DeleteProfile, versionedProfile(renamed.value())));
    QVERIFY(deleted);
    QCOMPARE(deleted->mode, Mode::Inactive);
    const auto list = data(request(socket, Operation::Profiles)).value(QStringLiteral("profiles"));
    QCOMPARE(list, QJsonValue(QJsonArray{metadataToJson(beta.value())}));
}

void ServiceDemoTest::simulatesBoundBlankAndUnknown()
{
    QLocalSocket socket;
    QVERIFY(greet(socket, m_path));
    const auto created = profile(create(socket, QStringLiteral("Alpha")));
    QVERIFY(created);
    auto alpha = created.value();
    QVERIFY(state(request(socket, Operation::Edit, profileId(alpha))));
    alpha.layers[0][QStringLiteral("key01")] = {QStringLiteral("Break even"), {29, 56, 18}};
    const auto saved = profile(request(socket, Operation::SaveProfile, saveArgs(alpha)));
    QVERIFY(saved);
    QVERIFY(state(request(socket, Operation::Activate, versionedProfile(saved.value()))));

    const auto press = request(socket, Operation::Input,
                               inputArgs(QStringLiteral("key01"), QStringLiteral("press")));
    const auto pressed = state(press);
    QVERIFY(pressed);
    QCOMPARE(pressed->heldControls, QStringList{QStringLiteral("key01")});
    QCOMPARE(pressed->ownedKeys, (QList<int>{29, 56, 18}));
    QCOMPARE(pressed->output,
             (OutputBatch{
                 {29, KeyTransition::Down}, {56, KeyTransition::Down}, {18, KeyTransition::Down}}));
    const auto bound =
        resolutionFromJson(data(press).value(QStringLiteral("resolution")).toObject());
    QVERIFY(bound);
    QCOMPARE(bound->label, QStringLiteral("Ctrl+Alt+E"));
    QVERIFY(!bound->blank);

    QVERIFY(error(request(socket, Operation::Input,
                          inputArgs(QStringLiteral("key01"), QStringLiteral("tap"))))
                .contains("Release"));
    QVERIFY(error(request(socket, Operation::Activate, versionedProfile(saved.value())))
                .contains("Release"));
    const auto repeat = state(request(
        socket, Operation::Input, inputArgs(QStringLiteral("key01"), QStringLiteral("repeat"))));
    QVERIFY(repeat);
    QCOMPARE(repeat->ownedKeys, pressed->ownedKeys);
    QVERIFY(repeat->output.isEmpty());
    const auto release = state(request(
        socket, Operation::Input, inputArgs(QStringLiteral("key01"), QStringLiteral("release"))));
    QVERIFY(release);
    QVERIFY(release->heldControls.isEmpty());
    QVERIFY(release->ownedKeys.isEmpty());
    QCOMPARE(
        release->output,
        (OutputBatch{{18, KeyTransition::Up}, {56, KeyTransition::Up}, {29, KeyTransition::Up}}));

    const auto tap = request(socket, Operation::Input,
                             inputArgs(QStringLiteral("wheelClick"), QStringLiteral("tap")));
    const auto blank = resolutionFromJson(data(tap).value(QStringLiteral("resolution")).toObject());
    QVERIFY(blank);
    QVERIFY(blank->blank);
    QCOMPARE(blank->label, QStringLiteral("Blank"));
    const auto tapped = state(tap);
    QVERIFY(tapped);
    QVERIFY(tapped->heldControls.isEmpty());
    QVERIFY(tapped->output.isEmpty());
    QCOMPARE(error(request(socket, Operation::Input,
                           inputArgs(QStringLiteral("key20"), QStringLiteral("press")))),
             QStringLiteral("Unknown control."));
}

void ServiceDemoTest::fragmentedAndCoalescedFrames()
{
    QLocalSocket socket;
    QVERIFY(connectPeer(socket, m_path));
    const Request hello{QStringLiteral("hello-partial"), Operation::Hello, {}};
    const QByteArray frame = wire(makeRequest(hello));
    QVERIFY(write(socket, frame.first(frame.size() / 2)));
    QVERIFY(!socket.waitForReadyRead(80));

    const Request first{QStringLiteral("first"), Operation::Profiles, {}};
    const Request second{QStringLiteral("second"), Operation::Hello, {}};
    QVERIFY(write(socket, frame.last(frame.size() - frame.size() / 2) + wire(makeRequest(first)) +
                              wire(makeRequest(second))));
    QString message;
    QJsonObject payload;
    const auto helloReply = receive(socket);
    QVERIFY(parseReply(helloReply, hello, message, payload));
    QVERIFY(state(helloReply));
    QCOMPARE(payload.value(QStringLiteral("profilesDir")).toString(),
             m_dir->filePath(QStringLiteral("profiles")));
    const auto firstReply = receive(socket);
    QVERIFY(!parseReply(firstReply, second, message, payload));
    QVERIFY(parseReply(firstReply, first, message, payload));
    QVERIFY(message.isEmpty());
    QCOMPARE(payload.value(QStringLiteral("profiles")), QJsonValue(QJsonArray{}));
    const auto secondReply = receive(socket);
    QVERIFY(parseReply(secondReply, second, message, payload));
    QVERIFY(state(secondReply));
}

void ServiceDemoTest::rejectsMalformedRequests()
{
    QLocalSocket socket;
    QVERIFY(connectPeer(socket, m_path));
    QCOMPARE(error(request(socket, Operation::Profiles)), QStringLiteral("Send hello first."));
    QVERIFY(state(request(socket, Operation::Hello)));

    const QString id = QStringLiteral("invalid");
    const QJsonObject stop = makeRequest({id, Operation::Stop, {}});
    const auto checkRejected = [&](const QString &field, const QJsonValue &value) {
        QJsonObject invalid = stop;
        invalid.insert(field, value);
        QVERIFY(write(socket, wire(invalid)));
        const QJsonObject reply = receive(socket);
        QCOMPARE(reply.value(QStringLiteral("id")), QJsonValue(id));
        QCOMPARE(reply.value(QStringLiteral("ok")), QJsonValue(false));
        QVERIFY(!error(reply).isEmpty());
    };
    checkRejected(QStringLiteral("version"), 1);
    checkRejected(QStringLiteral("version"), QStringLiteral("2"));
    checkRejected(QStringLiteral("op"), QStringLiteral("resolve"));
    checkRejected(QStringLiteral("op"), false);
    checkRejected(QStringLiteral("control"), true);
    checkRejected(QStringLiteral("args"), QJsonArray{});
    checkRejected(QStringLiteral("args"), QJsonObject{{QStringLiteral("extra"), false}});

    QVERIFY(!error(request(socket, Operation::Activate,
                           {{QStringLiteral("profileId"), QStringLiteral("unused")},
                            {QStringLiteral("expected"), 1.25}}))
                 .isEmpty());
    QVERIFY(!error(request(socket, Operation::CreateProfile, {{QStringLiteral("name"), 42}}))
                 .isEmpty());
    QCOMPARE(m_service.state(), QProcess::Running);
    QVERIFY(!QFileInfo::exists(m_dir->filePath(QStringLiteral("profiles"))));
}

void ServiceDemoTest::rejectsMalformedReplies()
{
    const Request reset{QStringLiteral("reset"), Operation::Reset, {}};
    QString message;
    QJsonObject payload;
    auto extra = successReply(reset.id);
    extra.insert(QStringLiteral("error"), QStringLiteral("Unexpected."));
    QVERIFY(!parseReply(extra, reset, message, payload));
    auto wrongVersion = successReply(reset.id);
    wrongVersion.insert(QStringLiteral("version"), 1);
    QVERIFY(!parseReply(wrongVersion, reset, message, payload));
    auto wrongData = successReply(reset.id);
    wrongData.insert(QStringLiteral("data"), QJsonArray{});
    QVERIFY(!parseReply(wrongData, reset, message, payload));

    Snapshot active;
    active.mode = Mode::Active;
    active.activeId = QStringLiteral("12345678-abcd-4567-abcd-123456789abc");
    active.activeName = QStringLiteral("Valid");
    active.activeRevision = 1;
    active.output = {{29, KeyTransition::Down}};
    QVERIFY(!snapshotFromJson(snapshotToJson(active)));
    active.output.append({29, KeyTransition::Up});
    QVERIFY(snapshotFromJson(snapshotToJson(active)));
    active.output.prepend({29, KeyTransition::Down});
    QVERIFY(!snapshotFromJson(snapshotToJson(active)));

    active.output = {};
    active.ownedKeys = {29, 29};
    active.heldControls = {QStringLiteral("key01")};
    QVERIFY(!snapshotFromJson(snapshotToJson(active)));
    active.ownedKeys = {29};
    QVERIFY(snapshotFromJson(snapshotToJson(active)));
    active.heldControls.append(QStringLiteral("key01"));
    QVERIFY(!snapshotFromJson(snapshotToJson(active)));
}

void ServiceDemoTest::abortsInvalidFrames_data()
{
    QTest::addColumn<QByteArray>("input");
    QTest::newRow("invalid JSON") << QByteArray("{\n");
    QTest::newRow("invalid ID") << QByteArray(
        "{\"id\":3,\"version\":2,\"op\":\"stop\",\"args\":{}}\n");
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

void ServiceDemoTest::releasesLeaseAndHeldOutputsOnDisconnect()
{
    QLocalSocket first;
    QLocalSocket second;
    QVERIFY(greet(first, m_path));
    QVERIFY(greet(second, m_path));
    QCOMPARE(error(request(second, Operation::Input,
                           inputArgs(QStringLiteral("key20"), QStringLiteral("press")))),
             QStringLiteral("Unknown control."));
    const auto created = profile(create(first, QStringLiteral("Alpha")));
    QVERIFY(created);
    auto alpha = created.value();
    QVERIFY(state(request(first, Operation::Edit, profileId(alpha))));
    QVERIFY(profile(request(second, Operation::LoadProfile, profileId(alpha))));
    QCOMPARE(error(request(second, Operation::Edit, profileId(alpha))),
             QStringLiteral("Another editor is using the demo."));

    alpha.layers[0][QStringLiteral("key01")] = {QStringLiteral("Break even"), {29, 56, 18}};
    const auto saved = profile(request(first, Operation::SaveProfile, saveArgs(alpha)));
    QVERIFY(saved);
    QVERIFY(state(request(first, Operation::Activate, versionedProfile(saved.value()))));
    const auto held = state(request(first, Operation::Input,
                                    inputArgs(QStringLiteral("key01"), QStringLiteral("press"))));
    QVERIFY(held);
    QVERIFY(!held->ownedKeys.isEmpty());
    QCOMPARE(error(request(second, Operation::Reset)),
             QStringLiteral("Another editor is using the demo."));
    const auto observed = state(request(second, Operation::Hello));
    QVERIFY(observed);
    QCOMPARE(observed->heldControls, held->heldControls);
    QCOMPARE(error(request(second, Operation::Input,
                           inputArgs(QStringLiteral("key01"), QStringLiteral("toggle")))),
             QStringLiteral("Another editor is using the demo."));

    first.abort();
    QLocalSocket observer;
    QVERIFY(greet(observer, m_path));
    std::optional<Snapshot> reopened;
    QTRY_VERIFY_WITH_TIMEOUT((reopened = state(request(observer, Operation::Hello))) &&
                                 reopened->heldControls.isEmpty(),
                             timeoutMs);
    QCOMPARE(reopened->mode, Mode::Active);
    QCOMPARE(reopened->activeId, alpha.id);
    QVERIFY(reopened->ownedKeys.isEmpty());
    QVERIFY(reopened->output.isEmpty());
    const auto toggle = inputArgs(QStringLiteral("key01"), QStringLiteral("toggle"));
    const auto pressed = state(request(second, Operation::Input, toggle));
    QVERIFY(pressed);
    QCOMPARE(pressed->heldControls, held->heldControls);
    QCOMPARE(pressed->output, held->output);
    const auto released = state(request(second, Operation::Input, toggle));
    QVERIFY(released);
    QVERIFY(released->heldControls.isEmpty());
    QVERIFY(released->ownedKeys.isEmpty());
    QCOMPARE(
        released->output,
        (OutputBatch{{18, KeyTransition::Up}, {56, KeyTransition::Up}, {29, KeyTransition::Up}}));
    QVERIFY(state(request(second, Operation::Edit, profileId(alpha))));
    second.abort();

    QLocalSocket third;
    QVERIFY(greet(third, m_path));
    std::optional<Snapshot> noOwner;
    QTRY_VERIFY_WITH_TIMEOUT((noOwner = state(request(third, Operation::Hello))) &&
                                 noOwner->mode == Mode::Inactive,
                             timeoutMs);
}

void ServiceDemoTest::preservesProfilesAcrossServiceRestart()
{
    stopService();
    const auto root = m_dir->filePath(QStringLiteral("custom-profiles"));
    const QStringList args{QStringLiteral("--profiles-dir"), root};
    startService(args);
    QLocalSocket socket;
    QVERIFY(greet(socket, m_path));
    QVERIFY(!QFileInfo::exists(root));
    QCOMPARE(
        data(request(socket, Operation::Hello)).value(QStringLiteral("profilesDir")).toString(),
        root);
    const auto created = profile(create(socket, QStringLiteral("Persistent")));
    QVERIFY(created);
    QVERIFY(state(request(socket, Operation::Activate, versionedProfile(created.value()))));
    QVERIFY(QFileInfo::exists(root));
    QVERIFY(!QFileInfo::exists(m_dir->filePath(QStringLiteral("profiles"))));
    stopService();
    socket.abort();
    startService(args);
    QVERIFY(greet(socket, m_path));

    const auto restarted = state(request(socket, Operation::Hello));
    QVERIFY(restarted);
    QCOMPARE(restarted->mode, Mode::Inactive);
    QVERIFY(restarted->activeId.isEmpty());
    const auto retained =
        profile(request(socket, Operation::LoadProfile, profileId(created.value())));
    QVERIFY(retained);
    QCOMPARE(retained.value(), created.value());
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
    QVERIFY(profile(create(first, QStringLiteral("Owner"))));
    QCOMPARE(m_service.state(), QProcess::Running);

    QProcess stop;
    stop.start(
        binary(QStringLiteral("xkeysd")),
        {QStringLiteral("--demo"), QStringLiteral("--stop"), QStringLiteral("--socket"), m_path});
    QVERIFY(stop.waitForFinished(timeoutMs));
    const auto stopError = stop.readAllStandardError();
    QVERIFY2(stop.exitStatus() == QProcess::NormalExit && stop.exitCode() == 0,
             stopError.constData());
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
        const auto usage = process.readAllStandardOutput();
        QVERIFY(usage.contains("--demo"));
        QVERIFY(usage.contains("--profiles-dir"));
    }
}

QTEST_GUILESS_MAIN(ServiceDemoTest)
#include "demo_test.moc"
