#include "src/ui/controller.h"

#include <QFileInfo>
#include <QGuiApplication>
#include <QImage>
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
    void actualClicksResolveAndRemainUsableWhenResized();
    void editorClosureDoesNotStopService();
    void serviceLossClearsReadyAndAllowsReconnect();
    void waitsForAcknowledgementAndRejectsWrongReply();

  private:
    void openEditor();
    void clickControl(const QString &id);
    void assertInsideWindow(QQuickItem *item);
    QQuickItem *findItem(const QString &name) const;

    std::unique_ptr<QTemporaryDir> directory_;
    QString socketPath_;
    QProcess service_;
    std::unique_ptr<xkeys::Controller> controller_;
    std::unique_ptr<QQmlApplicationEngine> engine_;
    QQuickWindow *window_ = nullptr;
    QStringList warnings_;
};

void UiDemoTest::init()
{
    warnings_.clear();
    directory_ = std::make_unique<QTemporaryDir>();
    QVERIFY(directory_->isValid());
    socketPath_ = directory_->filePath(QStringLiteral("demo.sock"));
    service_.start(QCoreApplication::applicationDirPath() + QStringLiteral("/xkeysd"),
                   {QStringLiteral("--demo"), QStringLiteral("--socket"), socketPath_});
    QVERIFY2(service_.waitForStarted(), qPrintable(service_.errorString()));
    QTRY_VERIFY_WITH_TIMEOUT(
        QFileInfo::exists(socketPath_) || service_.state() == QProcess::NotRunning, 5000);
    QVERIFY2(QFileInfo::exists(socketPath_), service_.readAllStandardError().constData());
    openEditor();
    QVERIFY(window_ != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(controller_->connected(), 5000);
}

void UiDemoTest::cleanup()
{
    if (controller_ && controller_->connected()) {
        controller_->stopDemo();
        QTest::qWait(100);
    }
    engine_.reset();
    window_ = nullptr;
    controller_.reset();
    if (service_.state() != QProcess::NotRunning && !service_.waitForFinished(2000)) {
        service_.terminate();
        if (!service_.waitForFinished(2000)) {
            service_.kill();
            service_.waitForFinished();
        }
    }
    directory_.reset();
    QCOMPARE(service_.exitStatus(), QProcess::NormalExit);
    QVERIFY2(service_.exitCode() == 0, service_.readAllStandardError().constData());
    QVERIFY2(warnings_.isEmpty(), qPrintable(warnings_.join(QLatin1Char('\n'))));
}

void UiDemoTest::openEditor()
{
    controller_ = std::make_unique<xkeys::Controller>();
    engine_ = std::make_unique<QQmlApplicationEngine>();
    connect(engine_.get(), &QQmlEngine::warnings, this, [this](const QList<QQmlError> &errors) {
        for (const auto &error : errors) {
            warnings_.append(error.toString());
        }
    });
    engine_->setInitialProperties(
        {{QStringLiteral("controller"), QVariant::fromValue(controller_.get())}});
    engine_->loadFromModule("XKeys", "Main");
    if (!engine_->rootObjects().isEmpty()) {
        window_ = qobject_cast<QQuickWindow *>(engine_->rootObjects().first());
    }
    controller_->startDemo(socketPath_);
}

QQuickItem *UiDemoTest::findItem(const QString &name) const
{
    QList<QQuickItem *> pending{window_->contentItem()};
    while (!pending.isEmpty()) {
        QQuickItem *item = pending.takeLast();
        if (item->objectName() == name) {
            return item;
        }
        pending.append(item->childItems());
    }
    return nullptr;
}

void UiDemoTest::clickControl(const QString &id)
{
    auto *button = findItem(QStringLiteral("pad-") + id);
    QVERIFY2(button != nullptr, qPrintable(id));
    QVERIFY(button->isEnabled());
    const QPoint position =
        button->mapToScene(QPointF(button->width() / 2, button->height() / 2)).toPoint();
    QTest::mouseClick(window_, Qt::LeftButton, Qt::NoModifier, position);
}

void UiDemoTest::assertInsideWindow(QQuickItem *item)
{
    QVERIFY(item != nullptr);
    QVERIFY(item->width() > 0);
    QVERIFY(item->height() > 0);
    const QPointF topLeft = item->mapToScene(QPointF(0, 0));
    const QPointF bottomRight = item->mapToScene(QPointF(item->width(), item->height()));
    QVERIFY(topLeft.x() >= 0);
    QVERIFY(topLeft.y() >= 0);
    QVERIFY(bottomRight.x() <= window_->width());
    QVERIFY(bottomRight.y() <= window_->height());
}

void UiDemoTest::actualClicksResolveAndRemainUsableWhenResized()
{
    QVERIFY(QTest::qWaitForWindowExposed(window_));
    clickControl(QStringLiteral("key01"));
    QTRY_COMPARE(controller_->selectedControl(), QStringLiteral("key01"));
    QCOMPARE(controller_->bindingLabel(), QStringLiteral("Ctrl+Alt+E"));
    QVERIFY(!controller_->blank());
    QCOMPARE(controller_->selectedLayer(), 1);
    auto *result = findItem(QStringLiteral("resolvedBinding"));
    QVERIFY(result != nullptr);
    QCOMPARE(result->property("text").toString(), controller_->bindingLabel());

    const QString screenshotPath = qEnvironmentVariable("XKEYS_SCREENSHOT_DIR");
    if (!screenshotPath.isEmpty()) {
        QTest::qWait(100);
        const QImage image = window_->grabWindow();
        QVERIFY(!image.isNull());
        QVERIFY(image.save(screenshotPath + QStringLiteral("/xkeys-demo-wide.png")));
    }

    clickControl(QStringLiteral("key03"));
    QTRY_COMPARE(controller_->selectedControl(), QStringLiteral("key03"));
    QCOMPARE(controller_->bindingLabel(), QStringLiteral("Blank"));
    QVERIFY(controller_->blank());
    QCOMPARE(result->property("text").toString(), QStringLiteral("Blank"));

    window_->resize(800, 550);
    QTest::qWait(100);
    for (const auto &control : controller_->controls()) {
        const QString id = control.toMap().value(QStringLiteral("controlId")).toString();
        auto *button = findItem(QStringLiteral("pad-") + id);
        assertInsideWindow(button);
    }
    assertInsideWindow(findItem(QStringLiteral("stopButton")));
    assertInsideWindow(findItem(QStringLiteral("testPopup")));
    clickControl(QStringLiteral("paddle"));
    QTRY_COMPARE(controller_->selectedControl(), QStringLiteral("paddle"));
    QVERIFY(controller_->blank());
    if (!screenshotPath.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(
            window_->grabWindow().save(screenshotPath + QStringLiteral("/xkeys-demo-small.png")));
    }

    window_->resize(854, 1410);
    QTest::qWait(100);
    auto *keypad = findItem(QStringLiteral("keypad"));
    QVERIFY(keypad != nullptr);
    QVERIFY(keypad->height() <= 420);
    assertInsideWindow(keypad);
    clickControl(QStringLiteral("wheelUp"));
    QTRY_COMPARE(controller_->selectedControl(), QStringLiteral("wheelUp"));
    if (!screenshotPath.isEmpty()) {
        QTest::qWait(100);
        QVERIFY(
            window_->grabWindow().save(screenshotPath + QStringLiteral("/xkeys-demo-tall.png")));
    }
}

void UiDemoTest::editorClosureDoesNotStopService()
{
    const qint64 servicePid = service_.processId();
    engine_.reset();
    window_ = nullptr;
    controller_.reset();
    QTest::qWait(100);
    QCOMPARE(service_.state(), QProcess::Running);

    openEditor();
    QVERIFY(window_ != nullptr);
    QTRY_VERIFY_WITH_TIMEOUT(controller_->connected(), 5000);
    QCOMPARE(service_.processId(), servicePid);
    clickControl(QStringLiteral("key02"));
    QTRY_COMPARE(controller_->selectedControl(), QStringLiteral("key02"));
    QCOMPARE(controller_->bindingLabel(), QStringLiteral("Shift+F"));
}

void UiDemoTest::serviceLossClearsReadyAndAllowsReconnect()
{
    service_.terminate();
    QVERIFY(service_.waitForFinished(3000));
    QTRY_VERIFY(!controller_->connected());
    QVERIFY(!controller_->pending());
    auto *button = findItem(QStringLiteral("pad-key01"));
    QVERIFY(button != nullptr);
    QVERIFY(!button->isEnabled());

    service_.start(QCoreApplication::applicationDirPath() + QStringLiteral("/xkeysd"),
                   {QStringLiteral("--demo"), QStringLiteral("--socket"), socketPath_});
    QVERIFY(service_.waitForStarted());
    QTest::qWait(100);
    controller_->reconnectDemo();
    QTRY_VERIFY_WITH_TIMEOUT(controller_->connected(), 5000);
    controller_->stopDemo();
    QTRY_VERIFY(!controller_->connected());
    QTRY_COMPARE(service_.state(), QProcess::NotRunning);
}

void UiDemoTest::waitsForAcknowledgementAndRejectsWrongReply()
{
    QLocalServer fakeService;
    fakeService.setSocketOptions(QLocalServer::UserAccessOption);
    const QString fakePath = directory_->filePath(QStringLiteral("fake.sock"));
    QVERIFY(fakeService.listen(fakePath));
    xkeys::Controller client;
    client.startDemo(fakePath);
    QTRY_VERIFY(fakeService.hasPendingConnections());
    std::unique_ptr<QLocalSocket> peer(fakeService.nextPendingConnection());
    QTRY_VERIFY(peer->canReadLine());
    const QJsonObject hello = QJsonDocument::fromJson(peer->readLine()).object();
    QVERIFY(!client.connected());
    QVERIFY(client.pending());
    const QJsonObject helloReply{{QStringLiteral("version"), 1},
                                 {QStringLiteral("id"), hello.value(QStringLiteral("id"))},
                                 {QStringLiteral("ok"), true}};
    peer->write(QJsonDocument(helloReply).toJson(QJsonDocument::Compact) + '\n');
    peer->flush();
    QTRY_VERIFY(client.connected());

    client.selectControl(QStringLiteral("key01"));
    QTRY_VERIFY(peer->canReadLine());
    const QJsonObject resolve = QJsonDocument::fromJson(peer->readLine()).object();
    QCOMPARE(resolve.value(QStringLiteral("control")).toString(), QStringLiteral("key01"));
    QVERIFY(client.pending());
    QVERIFY(client.selectedControl().isEmpty());
    QVERIFY(client.bindingLabel().isEmpty());

    const QJsonObject wrongReply{{QStringLiteral("version"), 1},
                                 {QStringLiteral("id"), QStringLiteral("not-the-request")},
                                 {QStringLiteral("ok"), false},
                                 {QStringLiteral("error"), QStringLiteral("Unrelated reply")}};
    peer->write(QJsonDocument(wrongReply).toJson(QJsonDocument::Compact) + '\n');
    peer->flush();
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
