#include "src/ui/controller.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLocalServer>
#include <QLocalSocket>
#include <QProcess>
#include <QQmlApplicationEngine>
#include <QQmlError>
#include <QQuickItem>
#include <QQuickWindow>
#include <QTemporaryDir>
#include <QtTest>

#include <memory>

class UiDemoTest : public QObject
{
    Q_OBJECT

  private slots:
    void init();
    void cleanup();
    void createsEditsAndActivatesExplicitly();
    void simulatesLayersAndOverlappingHolds();
    void editorClosurePreservesSessionAndRestartRequiresActivation();
    void waitsForAcknowledgementAndRejectsWrongReply_data();
    void waitsForAcknowledgementAndRejectsWrongReply();

  private:
    void openEditor();
    void startService();
    void makeAssignedProfile();
    void click(const QString &name);
    void clickControl(const QString &id);
    QQuickItem *findItem(const QString &name) const;
    void assertInsideWindow(QQuickItem *item);
    void saveImage(const QString &name);

    std::unique_ptr<QTemporaryDir> directory_;
    QString socketPath_;
    QProcess service_;
    std::unique_ptr<xkeys::Controller> controller_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    QQuickWindow *window_ = nullptr;
    QStringList warnings_;
};

void UiDemoTest::startService()
{
    service_.start(QCoreApplication::applicationDirPath() + QStringLiteral("/xkeysd"),
                   {QStringLiteral("--demo"), QStringLiteral("--socket"), socketPath_});
    QVERIFY2(service_.waitForStarted(), qPrintable(service_.errorString()));
    const auto listening = [this] {
        QLocalSocket probe;
        probe.connectToServer(socketPath_);
        return probe.waitForConnected(100);
    };
    QTRY_VERIFY_WITH_TIMEOUT(listening() || service_.state() == QProcess::NotRunning, 5000);
    QVERIFY2(service_.state() == QProcess::Running, service_.readAllStandardError().constData());
}

void UiDemoTest::init()
{
    warnings_.clear();
    directory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(directory_->isValid());
    socketPath_ = directory_->filePath(QStringLiteral("demo.sock"));
    startService();
    openEditor();
    QVERIFY(window_ != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(controller_->connected() && !controller_->pending(), 5000);
    QCOMPARE(controller_->mode(), QStringLiteral("Inactive"));
    QVERIFY(controller_->profiles().isEmpty());
}

void UiDemoTest::cleanup()
{
    bool editorSettled = true;
    if (controller_ && controller_->connected()) {
        editorSettled = QTest::qWaitFor([this] { return !controller_->pending(); }, 1000);
        if (editorSettled && controller_->connected()) {
            controller_->stopDemo();
            editorSettled = QTest::qWaitFor([this] { return !controller_->connected(); }, 1000);
        }
    }
    engine_.reset();
    window_ = nullptr;
    controller_.reset();
    QByteArray stopError;
    if (service_.state() != QProcess::NotRunning && !service_.waitForFinished(100)) {
        QProcess stop;
        stop.start(service_.program(), {QStringLiteral("--demo"), QStringLiteral("--stop"),
                                        QStringLiteral("--socket"), socketPath_});
        if (!stop.waitForFinished(1500) && stop.state() != QProcess::NotRunning) {
            stop.kill();
            stop.waitForFinished(1000);
        }
        stopError = stop.readAllStandardError();
    }
    if (service_.state() != QProcess::NotRunning && !service_.waitForFinished(1000)) {
        service_.terminate();
        if (!service_.waitForFinished(1000)) {
            service_.kill();
            service_.waitForFinished(1000);
        }
    }
    const auto serviceError = service_.readAllStandardError() + stopError;
    directory_.reset();
    QVERIFY2(service_.state() == QProcess::NotRunning &&
                 service_.exitStatus() == QProcess::NormalExit && service_.exitCode() == 0,
             serviceError.constData());
    QVERIFY2(editorSettled, "The editor did not settle during cleanup.");
    QVERIFY2(warnings_.isEmpty(), qPrintable(warnings_.join(QLatin1Char('\n'))));
}

void UiDemoTest::openEditor()
{
    controller_ = std::make_unique<xkeys::Controller>();
    engine_ = std::make_unique<QQmlApplicationEngine>();
    connect(engine_.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
        for (const auto &error : errors)
            warnings_.append(error.toString());
    });
    engine_->setInitialProperties(
        {{QStringLiteral("controller"), QVariant::fromValue(controller_.get())}});
    engine_->loadFromModule("XKeys", "Main");
    if (!engine_->rootObjects().isEmpty())
        window_ = qobject_cast<QQuickWindow *>(engine_->rootObjects().first());
    controller_->startDemo(socketPath_);
}

QQuickItem *UiDemoTest::findItem(const QString &name) const
{
    QList<QQuickItem *> pending{window_->contentItem()};
    while (!pending.isEmpty()) {
        QQuickItem *item = pending.takeLast();
        if (item->objectName() == name)
            return item;
        pending.append(item->childItems());
    }
    return nullptr;
}

void UiDemoTest::click(const QString &name)
{
    auto *item = findItem(name);
    QVERIFY2(item != nullptr, qPrintable(name));
    QVERIFY(item->isVisible());
    QVERIFY(item->isEnabled());
    const auto position =
        item->mapToScene(QPointF(item->width() / 2, item->height() / 2)).toPoint();
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, position);
}

void UiDemoTest::clickControl(const QString &id) { click(QStringLiteral("pad-") + id); }

void UiDemoTest::assertInsideWindow(QQuickItem *item)
{
    QVERIFY(item != nullptr);
    QVERIFY(item->width() > 0);
    QVERIFY(item->height() > 0);
    const auto topLeft = item->mapToScene(QPointF(0, 0));
    const auto bottomRight = item->mapToScene(QPointF(item->width(), item->height()));
    QVERIFY(topLeft.x() >= 0 && topLeft.y() >= 0);
    QVERIFY(bottomRight.x() <= window_->width() && bottomRight.y() <= window_->height());
}

void UiDemoTest::saveImage(const QString &name)
{
    const QString root = qEnvironmentVariable("XKEYS_SCREENSHOT_DIR");
    if (!root.isEmpty()) {
        QTest::qWait(100);
        const auto image = window_->grabWindow();
        QVERIFY(!image.isNull());
        QVERIFY(image.save(root + QLatin1Char('/') + name));
    }
}

void UiDemoTest::makeAssignedProfile()
{
    controller_->createProfile(QStringLiteral("Alpha"));
    QTRY_VERIFY(controller_->hasProfile() && !controller_->pending());
    controller_->editSelected();
    QTRY_VERIFY(controller_->editing() && !controller_->pending());
    controller_->selectControl(QStringLiteral("key01"));
    controller_->assignSample(0, QStringLiteral("First chord"));
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->bindingLabel(), QStringLiteral("Ctrl+Alt+E"));
    controller_->selectControl(QStringLiteral("key02"));
    controller_->assignSample(1, {});
    QTRY_VERIFY(!controller_->pending());
    controller_->selectControl(QStringLiteral("key03"));
    controller_->assignSample(0, {});
    QTRY_VERIFY(!controller_->pending());
    controller_->selectControl(QStringLiteral("thumb"));
    controller_->assignSample(3, {});
    QTRY_VERIFY(!controller_->pending());
    controller_->setThumbLayer(true);
    QTRY_VERIFY(!controller_->pending());
    QVERIFY(controller_->thumbLayer());
    controller_->setEditLayer(2);
    controller_->selectControl(QStringLiteral("key01"));
    controller_->assignSample(2, {});
    QTRY_VERIFY(!controller_->pending());
    controller_->setEditLayer(1);
    controller_->activateSelected();
    QTRY_VERIFY(controller_->mode() == QStringLiteral("Active") && !controller_->pending());
}

void UiDemoTest::createsEditsAndActivatesExplicitly()
{
    QVERIFY(QTest::qWaitForWindowExposed(window_));
    click(QStringLiteral("newProfileButton"));
    QTRY_VERIFY(findItem(QStringLiteral("newProfileName")) != nullptr);
    auto *name = findItem(QStringLiteral("newProfileName"));
    name->forceActiveFocus();
    for (const char letter : QByteArray("Alpha"))
        QTest::keyClick(window_, letter);
    QTest::keyClick(window_, Qt::Key_Return);
    QTRY_VERIFY(controller_->hasProfile() && !controller_->pending());
    QCOMPARE(controller_->selectedProfileName(), QStringLiteral("Alpha"));
    QCOMPARE(controller_->mode(), QStringLiteral("Inactive"));
    QVERIFY(controller_->activeProfileId().isEmpty());
    const QString alphaId = controller_->selectedProfileId();

    click(QStringLiteral("editProfileButton"));
    QTRY_VERIFY(controller_->editing() && !controller_->pending());
    clickControl(QStringLiteral("key01"));
    QCOMPARE(controller_->bindingLabel(), QStringLiteral("Blank"));
    controller_->assignSample(0, {});
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->bindingLabel(), QStringLiteral("Ctrl+Alt+E"));
    QVERIFY(controller_->activeProfileId().isEmpty());
    saveImage(QStringLiteral("phase2-edit.png"));
    click(QStringLiteral("activateButton"));
    QTRY_VERIFY(controller_->mode() == QStringLiteral("Active") && !controller_->pending());
    clickControl(QStringLiteral("key01"));
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->bindingLabel(), QStringLiteral("Ctrl+Alt+E"));
    QCOMPARE(controller_->heldOutputLabel(), QStringLiteral("None"));
    clickControl(QStringLiteral("key03"));
    QTRY_VERIFY(!controller_->pending());
    QVERIFY(controller_->blank());
    saveImage(QStringLiteral("phase2-active.png"));

    controller_->createProfile(QStringLiteral("Beta"));
    QTRY_VERIFY(controller_->profiles().size() == 2 && !controller_->pending());
    QCOMPARE(controller_->activeProfileId(), alphaId);
    QVERIFY(controller_->selectedProfileId() != alphaId);
    click(QStringLiteral("activateButton"));
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->activeProfileName(), QStringLiteral("Beta"));
    clickControl(QStringLiteral("key01"));
    QTRY_VERIFY(!controller_->pending());
    QVERIFY(controller_->blank());

    click(QStringLiteral("deleteProfileButton"));
    QObject *dialog =
        engine_->rootObjects().first()->findChild<QObject *>(QStringLiteral("deleteProfileDialog"));
    QVERIFY(dialog != nullptr);
    QVERIFY(dialog->property("visible").toBool());
    QVERIFY(QMetaObject::invokeMethod(dialog, "reject"));
    QCOMPARE(controller_->profiles().size(), 2);
    controller_->deleteSelected();
    QTRY_VERIFY(!controller_->pending());
    QVERIFY(!controller_->errorText().isEmpty());
    QCOMPARE(controller_->profiles().size(), 2);
    QCOMPARE(controller_->activeProfileName(), QStringLiteral("Beta"));
    click(QStringLiteral("deactivateButton"));
    QTRY_VERIFY(!controller_->pending());
    click(QStringLiteral("deleteProfileButton"));
    QVERIFY(QMetaObject::invokeMethod(dialog, "accept"));
    QTRY_VERIFY(controller_->profiles().size() == 1 && !controller_->pending());
    QVERIFY(!controller_->hasProfile());

    window_->resize(1000, 780);
    QTest::qWait(100);
    for (const auto &control : controller_->controls()) {
        const auto id = control.toMap().value(QStringLiteral("controlId")).toString();
        assertInsideWindow(findItem(QStringLiteral("pad-") + id));
    }
    assertInsideWindow(findItem(QStringLiteral("stopButton")));
    assertInsideWindow(findItem(QStringLiteral("profileSelector")));
    saveImage(QStringLiteral("phase2-small.png"));
    window_->resize(1100, 1410);
    QTest::qWait(100);
    QVERIFY(findItem(QStringLiteral("keypad"))->height() <= 420);
    saveImage(QStringLiteral("phase2-tall.png"));
}

void UiDemoTest::simulatesLayersAndOverlappingHolds()
{
    makeAssignedProfile();
    click(QStringLiteral("holdMode"));
    clickControl(QStringLiteral("thumb"));
    QTRY_VERIFY(controller_->activeLayer() == 2 && !controller_->pending());
    clickControl(QStringLiteral("key01"));
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->heldOutputLabel(), QStringLiteral("W"));
    clickControl(QStringLiteral("thumb"));
    QTRY_VERIFY(controller_->activeLayer() == 1 && !controller_->pending());
    QCOMPARE(controller_->heldOutputLabel(), QStringLiteral("W"));
    clickControl(QStringLiteral("key01"));
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->heldOutputLabel(), QStringLiteral("None"));
    QVERIFY(controller_->heldControls().isEmpty());

    clickControl(QStringLiteral("thumb"));
    QTRY_VERIFY(!controller_->pending());
    clickControl(QStringLiteral("key01"));
    QTRY_VERIFY(!controller_->pending());
    clickControl(QStringLiteral("key01"));
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->activeLayer(), 2);
    QCOMPARE(controller_->heldOutputLabel(), QStringLiteral("None"));
    clickControl(QStringLiteral("thumb"));
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->activeLayer(), 1);

    clickControl(QStringLiteral("key01"));
    QTRY_VERIFY(!controller_->pending());
    clickControl(QStringLiteral("key03"));
    QTRY_VERIFY(!controller_->pending());
    clickControl(QStringLiteral("key01"));
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->heldOutputLabel(), QStringLiteral("Ctrl+Alt+E"));
    saveImage(QStringLiteral("phase2-held.png"));
    click(QStringLiteral("releaseAllButton"));
    QTRY_VERIFY(!controller_->pending());
    QVERIFY(controller_->heldControls().isEmpty());
    QCOMPARE(controller_->heldOutputLabel(), QStringLiteral("None"));

    click(QStringLiteral("editProfileButton"));
    QTRY_VERIFY(controller_->editing() && !controller_->pending());
    clickControl(QStringLiteral("key03"));
    click(QStringLiteral("clearBindingButton"));
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->bindingLabel(), QStringLiteral("Blank"));
    QVERIFY(controller_->activeProfileId().isEmpty());
    clickControl(QStringLiteral("thumb"));
    QCOMPARE(controller_->bindingLabel(), QStringLiteral("Space"));
    click(QStringLiteral("clearBindingButton"));
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->bindingLabel(), QStringLiteral("Blank"));
    QVERIFY(controller_->thumbLayer());
}

void UiDemoTest::editorClosurePreservesSessionAndRestartRequiresActivation()
{
    makeAssignedProfile();
    const auto profileId = controller_->selectedProfileId();
    const auto pid = service_.processId();
    controller_->simulate(QStringLiteral("key01"), true);
    QTRY_VERIFY(!controller_->pending());
    QVERIFY(!controller_->heldControls().isEmpty());
    engine_.reset();
    window_ = nullptr;
    controller_.reset();
    QTest::qWait(100);
    QCOMPARE(service_.state(), QProcess::Running);
    openEditor();
    QVERIFY(window_ != nullptr);
    QTRY_VERIFY(controller_->connected() && !controller_->pending());
    QCOMPARE(service_.processId(), pid);
    QCOMPARE(controller_->activeProfileId(), profileId);
    QVERIFY(controller_->heldControls().isEmpty());
    QCOMPARE(controller_->heldOutputLabel(), QStringLiteral("None"));
    clickControl(QStringLiteral("key01"));
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->bindingLabel(), QStringLiteral("Ctrl+Alt+E"));

    controller_->simulate(QStringLiteral("key01"), true);
    QTRY_VERIFY(!controller_->pending());
    QVERIFY(!controller_->heldControls().isEmpty());
    service_.terminate();
    QVERIFY(service_.waitForFinished(3000));
    QTRY_VERIFY(!controller_->connected());
    QVERIFY(controller_->heldControls().isEmpty());
    QCOMPARE(controller_->heldOutputLabel(), QStringLiteral("None"));
    QVERIFY(!findItem(QStringLiteral("pad-key01"))->isEnabled());
    startService();
    controller_->reconnectDemo();
    QTRY_VERIFY(controller_->connected() && !controller_->pending());
    QCOMPARE(controller_->mode(), QStringLiteral("Inactive"));
    QVERIFY(controller_->activeProfileId().isEmpty());
    QCOMPARE(controller_->profiles().size(), 1);
    controller_->selectProfile(profileId);
    QTRY_VERIFY(!controller_->pending());
    controller_->activateSelected();
    QTRY_VERIFY(!controller_->pending());
    clickControl(QStringLiteral("key01"));
    QTRY_VERIFY(!controller_->pending());
    QCOMPARE(controller_->bindingLabel(), QStringLiteral("Ctrl+Alt+E"));
    click(QStringLiteral("stopButton"));
    QTRY_VERIFY(!controller_->connected());
    QTRY_COMPARE(service_.state(), QProcess::NotRunning);
}

void UiDemoTest::waitsForAcknowledgementAndRejectsWrongReply_data()
{
    QTest::addColumn<QString>("fault");
    QTest::newRow("mismatched ID") << QStringLiteral("id");
    QTest::newRow("reset contains down and up") << QStringLiteral("reset");
    QTest::newRow("transition contradicts final ownership") << QStringLiteral("ownership");
    QTest::newRow("different profile directory") << QStringLiteral("root");
}

void UiDemoTest::waitsForAcknowledgementAndRejectsWrongReply()
{
    QFETCH(QString, fault);
    QLocalServer fakeService;
    fakeService.setSocketOptions(QLocalServer::UserAccessOption);
    const auto fakePath = directory_->filePath(QStringLiteral("fake.sock"));
    QVERIFY(fakeService.listen(fakePath));
    xkeys::Controller client;
    client.startDemo(fakePath);
    QTRY_VERIFY(fakeService.hasPendingConnections());
    std::unique_ptr<QLocalSocket> peer(fakeService.nextPendingConnection());
    QTRY_VERIFY(peer->canReadLine());
    auto request = QJsonDocument::fromJson(peer->readLine()).object();
    const auto reply = [&](const QJsonObject &data) {
        const QJsonObject object{{QStringLiteral("version"), 2},
                                 {QStringLiteral("id"), request.value(QStringLiteral("id"))},
                                 {QStringLiteral("ok"), true},
                                 {QStringLiteral("data"), data}};
        peer->write(QJsonDocument(object).toJson(QJsonDocument::Compact) + '\n');
        peer->flush();
    };
    const QString id = QStringLiteral("11111111-1111-4111-8111-111111111111");
    const QJsonObject state{{QStringLiteral("mode"), QStringLiteral("Active")},
                            {QStringLiteral("activeId"), id},
                            {QStringLiteral("activeName"), QStringLiteral("Alpha")},
                            {QStringLiteral("activeRevision"), 1},
                            {QStringLiteral("editProfileId"), QString{}},
                            {QStringLiteral("activeLayer"), 1},
                            {QStringLiteral("heldControls"), QJsonArray{}},
                            {QStringLiteral("ownedKeys"), QJsonArray{}},
                            {QStringLiteral("output"), QJsonArray{}}};
    reply({{QStringLiteral("state"), state},
           {QStringLiteral("profilesDir"),
            directory_->filePath(fault == QStringLiteral("root") ? QStringLiteral("other-profiles")
                                                                 : QStringLiteral("profiles"))}});
    if (fault == QStringLiteral("root")) {
        QTRY_VERIFY(!client.errorText().isEmpty());
        QVERIFY(!client.connected());
        QVERIFY(!client.pending());
        return;
    }
    QTRY_VERIFY(peer->canReadLine());
    request = QJsonDocument::fromJson(peer->readLine()).object();
    QCOMPARE(request.value(QStringLiteral("op")).toString(), QStringLiteral("profiles"));
    reply({{QStringLiteral("profiles"),
            QJsonArray{QJsonObject{{QStringLiteral("id"), id},
                                   {QStringLiteral("name"), QStringLiteral("Alpha")},
                                   {QStringLiteral("revision"), 1},
                                   {QStringLiteral("thumbLayer"), false}}}}});
    QTRY_VERIFY(client.connected() && !client.pending());
    if (fault == QStringLiteral("id"))
        client.simulate(QStringLiteral("key01"), false);
    else
        client.releaseAll();
    QTRY_VERIFY(peer->canReadLine());
    request = QJsonDocument::fromJson(peer->readLine()).object();
    QCOMPARE(request.value(QStringLiteral("op")).toString(),
             fault == QStringLiteral("id") ? QStringLiteral("input") : QStringLiteral("reset"));
    QVERIFY(client.pending());
    QVERIFY(client.selectedControl().isEmpty());
    QVERIFY(client.bindingLabel().isEmpty());
    const QJsonObject wrong{{QStringLiteral("version"), 2},
                            {QStringLiteral("id"), QStringLiteral("wrong-request")},
                            {QStringLiteral("ok"), false},
                            {QStringLiteral("error"), QStringLiteral("Unrelated reply")}};
    if (fault == QStringLiteral("id")) {
        peer->write(QJsonDocument(wrong).toJson(QJsonDocument::Compact) + '\n');
        peer->flush();
    } else {
        auto malformedState = state;
        QJsonArray output{QJsonObject{{QStringLiteral("code"), 29},
                                      {QStringLiteral("transition"), QStringLiteral("down")}}};
        if (fault == QStringLiteral("reset")) {
            output.append(QJsonObject{{QStringLiteral("code"), 29},
                                      {QStringLiteral("transition"), QStringLiteral("up")}});
        }
        malformedState.insert(QStringLiteral("output"), output);
        reply({{QStringLiteral("state"), malformedState}});
    }
    QTRY_VERIFY(!client.connected());
    QVERIFY(!client.pending());
    QVERIFY(client.selectedControl().isEmpty());
    QVERIFY(client.bindingLabel().isEmpty());
    QVERIFY(!client.errorText().isEmpty());
}

int main(int argc, char **argv)
{
    QGuiApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    UiDemoTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "demo_test.moc"
