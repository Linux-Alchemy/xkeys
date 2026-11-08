#include "ui/capture_backend.h"

#include <QSignalSpy>
#include <QThread>
#include <QtTest>

#include <iostream>
#include <string>

namespace
{

int fakeHelper(const std::string &scenario)
{
    std::string command;
    if (!std::getline(std::cin, command) || command != "ping") {
        return 3;
    }

    const auto write = [](const char *frame) { std::cout << frame << std::endl; };
    if (scenario == "before-ready") {
        write(R"({"type":"progress","codes":[29]})");
        return 0;
    }
    if (scenario == "failed-arm") {
        write(R"({"type":"error","reason":"Synthetic grab failed."})");
        return 1;
    }

    write(R"({"type":"ready"})");
    if (scenario == "cancel") {
        while (std::getline(std::cin, command)) {
            if (command == "cancel") {
                write(R"({"type":"cancelled","reason":"Synthetic cancellation."})");
                return 0;
            }
        }
        return 3;
    }
    if (scenario == "oversize") {
        std::cout << std::string(4097, ' ') << std::endl;
        return 0;
    }
    if (scenario == "invalid-codes") {
        write(R"({"type":"progress","codes":[29,29]})");
        return 0;
    }

    if (scenario == "normal" && (!std::getline(std::cin, command) || command != "ping")) {
        return 3;
    }
    write(R"({"type":"progress","codes":[29,33]})");
    write(R"({"type":"complete","codes":[29,33]})");

    if (scenario == "extra-frame") {
        write(R"({"type":"ready"})");
    } else if (scenario == "hang") {
        QThread::msleep(5000);
    } else if (scenario == "bad-exit") {
        return 1;
    }
    return 0;
}

xkeys::KeyboardSelection selection(const QString &id)
{
    return {id, QStringLiteral("Synthetic keyboard"), {}, {}, {}, true, {}, QString(64, u'a')};
}

} // namespace

class CaptureBackendTest : public QObject
{
    Q_OBJECT

  private slots:
    void startsHeartbeatBeforeReadyAndContinues();
    void sendsCancelAndWaitsForExit();
    void rejectsMalformedOrFailedHelper_data();
    void rejectsMalformedOrFailedHelper();
};

void CaptureBackendTest::startsHeartbeatBeforeReadyAndContinues()
{
    const auto backend = xkeys::makeProcessCaptureBackend(QCoreApplication::applicationFilePath());
    QSignalSpy ready(backend.get(), &xkeys::CaptureBackend::ready);
    QSignalSpy progress(backend.get(), &xkeys::CaptureBackend::progress);
    QSignalSpy completed(backend.get(), &xkeys::CaptureBackend::completed);
    QSignalSpy failed(backend.get(), &xkeys::CaptureBackend::failed);

    backend->start(selection(QStringLiteral("normal")));
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
    QTRY_COMPARE_WITH_TIMEOUT(completed.count(), 1, 3000);
    QCOMPARE(progress.count(), 1);
    QCOMPARE(completed.first().first().value<QList<int>>(), (QList<int>{29, 33}));
    QCOMPARE(failed.count(), 0);
}

void CaptureBackendTest::sendsCancelAndWaitsForExit()
{
    const auto backend = xkeys::makeProcessCaptureBackend(QCoreApplication::applicationFilePath());
    QSignalSpy ready(backend.get(), &xkeys::CaptureBackend::ready);
    QSignalSpy cancelled(backend.get(), &xkeys::CaptureBackend::cancelled);
    QSignalSpy completed(backend.get(), &xkeys::CaptureBackend::completed);
    QSignalSpy failed(backend.get(), &xkeys::CaptureBackend::failed);

    backend->start(selection(QStringLiteral("cancel")));
    QTRY_COMPARE_WITH_TIMEOUT(ready.count(), 1, 3000);
    backend->cancel();
    QTRY_COMPARE_WITH_TIMEOUT(cancelled.count(), 1, 3000);
    QCOMPARE(cancelled.first().first().toString(), QStringLiteral("Synthetic cancellation."));
    QCOMPARE(completed.count(), 0);
    QCOMPARE(failed.count(), 0);
}

void CaptureBackendTest::rejectsMalformedOrFailedHelper_data()
{
    QTest::addColumn<QString>("scenario");
    QTest::addColumn<QString>("reason");
    QTest::newRow("progress before ready")
        << QStringLiteral("before-ready") << QStringLiteral("Invalid reply");
    QTest::newRow("oversized frame")
        << QStringLiteral("oversize") << QStringLiteral("oversized reply");
    QTest::newRow("duplicate code")
        << QStringLiteral("invalid-codes") << QStringLiteral("Invalid reply");
    QTest::newRow("extra frame after complete")
        << QStringLiteral("extra-frame") << QStringLiteral("Invalid reply");
    QTest::newRow("complete then hang") << QStringLiteral("hang") << QStringLiteral("time limit");
    QTest::newRow("complete then bad exit")
        << QStringLiteral("bad-exit") << QStringLiteral("closed unexpectedly");
    QTest::newRow("arm failure") << QStringLiteral("failed-arm")
                                 << QStringLiteral("Synthetic grab failed.");
}

void CaptureBackendTest::rejectsMalformedOrFailedHelper()
{
    QFETCH(QString, scenario);
    QFETCH(QString, reason);
    const auto backend =
        xkeys::makeProcessCaptureBackend(QCoreApplication::applicationFilePath(), 2000);
    QSignalSpy progress(backend.get(), &xkeys::CaptureBackend::progress);
    QSignalSpy completed(backend.get(), &xkeys::CaptureBackend::completed);
    QSignalSpy failed(backend.get(), &xkeys::CaptureBackend::failed);
    backend->start(selection(scenario));
    if (scenario == QStringLiteral("hang")) {
        QTRY_COMPARE_WITH_TIMEOUT(progress.count(), 1, 1500);
        QCOMPARE(completed.count(), 0);
    }
    QTRY_COMPARE_WITH_TIMEOUT(failed.count(), 1, 3500);
    QCOMPARE(completed.count(), 0);
    QVERIFY2(failed.first().first().toString().contains(reason),
             qPrintable(failed.first().first().toString()));
}

int main(int argc, char **argv)
{
    if (argc == 6 && std::string(argv[1]) == "--capture" && std::string(argv[2]) == "--keyboard" &&
        std::string(argv[4]) == "--fingerprint") {
        return std::string(argv[5]) == std::string(64, 'a') ? fakeHelper(argv[3]) : 3;
    }
    QCoreApplication app(argc, argv);
    CaptureBackendTest test;
    return QTest::qExec(&test, argc, argv);
}

#include "capture_backend_test.moc"
