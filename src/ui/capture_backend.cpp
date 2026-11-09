#include "capture_backend.h"

#include "linux/keyboard_inventory.h"

#include <QCoreApplication>
#include <QFileInfo>
#include <QGuiApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSet>
#include <QTimer>

#include <linux/input-event-codes.h>
#include <unistd.h>

namespace xkeys
{

namespace
{
constexpr int heartbeatMs = 250;
constexpr int cancelWatchdogMs = 2250;
constexpr qsizetype maxFrameBytes = 4096;
constexpr int maxFrames = 18;

std::optional<QList<int>> parseCodes(const QJsonValue &value)
{
    const auto array = value.toArray();
    if (array.isEmpty() || array.size() > 16) {
        return std::nullopt;
    }
    QSet<int> seen;
    QList<int> codes;
    for (const auto item : array) {
        const int code = item.toInt(-1);
        if (code <= KEY_RESERVED || code >= BTN_MISC || code == KEY_UNKNOWN ||
            seen.contains(code)) {
            return std::nullopt;
        }
        seen.insert(code);
        codes.append(code);
    }
    return codes;
}

class ProcessCaptureBackend final : public CaptureBackend
{
  public:
    ProcessCaptureBackend(QString helper, QString reason, int watchdogMs)
        : m_helper(std::move(helper)), m_unavailable(std::move(reason))
    {
        m_process.setProcessChannelMode(QProcess::ForwardedErrorChannel);
        m_ping.setInterval(heartbeatMs);
        m_ping.setTimerType(Qt::PreciseTimer);
        m_deadline.setSingleShot(true);
        m_deadline.setTimerType(Qt::PreciseTimer);
        m_deadline.setInterval(watchdogMs);
        m_cancelDeadline.setSingleShot(true);
        m_cancelDeadline.setTimerType(Qt::PreciseTimer);
        m_cancelDeadline.setInterval(cancelWatchdogMs);

        connect(&m_process, &QProcess::started, this, [this] {
            if (m_cancelRequested) {
                m_process.write("cancel\n");
                return;
            }
            heartbeat();
            m_ping.start();
        });
        connect(&m_process, &QProcess::readyReadStandardOutput, this, [this] { readFrames(); });
        connect(&m_process, &QProcess::finished, this, &ProcessCaptureBackend::finished);
        connect(&m_process, &QProcess::errorOccurred, this, [this](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                reset();
                emit failed(QStringLiteral("Could not start the recording helper."));
            }
        });
        connect(&m_ping, &QTimer::timeout, this, &ProcessCaptureBackend::heartbeat);
        connect(&m_deadline, &QTimer::timeout, this,
                [this] { kill(QStringLiteral("The recording helper exceeded its time limit.")); });
        connect(&m_cancelDeadline, &QTimer::timeout, this, [this] {
            kill(QStringLiteral("The recording helper did not finish cancelling in time."));
        });
    }

    ~ProcessCaptureBackend() override
    {
        m_process.disconnect(this);
        if (m_process.state() != QProcess::NotRunning) {
            m_process.write("cancel\n");
            m_process.closeWriteChannel();
            if (!m_process.waitForFinished(cancelWatchdogMs)) {
                m_process.kill();
                m_process.waitForFinished(1000);
            }
        }
    }

    bool available() const override { return unavailableReason().isEmpty(); }

    QString unavailableReason() const override
    {
        if (!m_unavailable.isEmpty()) {
            return m_unavailable;
        }
        const QFileInfo helper(m_helper);
        return helper.isFile() && helper.isExecutable()
                   ? QString{}
                   : QStringLiteral("The recording helper is missing or cannot run.");
    }

    Result<QList<KeyboardSelection>> keyboards() override
    {
        if (!available()) {
            return Result<QList<KeyboardSelection>>::failure(ErrorCode::Invalid,
                                                             unavailableReason());
        }
        return discoverKeyboards();
    }

    void start(const KeyboardSelection &keyboard) override
    {
        if (m_active) {
            return;
        }
        if (!available() || !keyboard.accessible || keyboard.id.isEmpty() ||
            keyboard.fingerprint.isEmpty()) {
            const auto reason = unavailableReason();
            emit failed(reason.isEmpty() ? QStringLiteral("Choose an accessible keyboard.")
                                         : reason);
            return;
        }

        m_active = true;
        m_deadline.start();
        m_process.start(m_helper,
                        {QStringLiteral("--capture"), QStringLiteral("--keyboard"), keyboard.id,
                         QStringLiteral("--fingerprint"), keyboard.fingerprint});
    }

    void cancel() override
    {
        if (!m_active || m_cancelRequested) {
            return;
        }
        m_cancelRequested = true;
        m_ping.stop();
        if (m_process.state() == QProcess::Running && m_terminal == Terminal::None) {
            m_process.write("cancel\n");
        }
        m_cancelDeadline.start();
    }

  private:
    enum class Terminal { None, Complete, Cancelled, Error };

    void heartbeat()
    {
        if (m_process.bytesToWrite() < maxFrameBytes) {
            m_process.write("ping\n");
        }
    }

    void kill(const QString &reason)
    {
        if (m_failure.isEmpty()) {
            m_failure = reason;
        }
        m_ping.stop();
        m_process.kill();
    }

    void reset()
    {
        m_active = false;
        m_ready = false;
        m_cancelRequested = false;
        m_ping.stop();
        m_deadline.stop();
        m_cancelDeadline.stop();
        m_buffer.clear();
        m_frameCount = 0;
        m_terminal = Terminal::None;
        m_failure.clear();
        m_reason.clear();
        m_codes.clear();
    }

    void readFrames()
    {
        while (m_failure.isEmpty() && m_process.bytesAvailable() > 0) {
            m_buffer += m_process.read(maxFrameBytes + 1 - m_buffer.size());
            for (qsizetype end = m_buffer.indexOf('\n'); end != -1; end = m_buffer.indexOf('\n')) {
                if (end >= maxFrameBytes) {
                    kill(QStringLiteral("The recording helper sent an oversized reply."));
                    return;
                }
                const auto line = m_buffer.first(end);
                m_buffer.remove(0, end + 1);
                if (!acceptFrame(QJsonDocument::fromJson(line).object())) {
                    kill(QStringLiteral("Invalid reply from the recording helper."));
                    return;
                }
            }
            if (m_buffer.size() >= maxFrameBytes) {
                kill(QStringLiteral("The recording helper sent an oversized reply."));
                return;
            }
        }
    }

    bool acceptFrame(const QJsonObject &frame)
    {
        if (m_terminal != Terminal::None || ++m_frameCount > maxFrames) {
            return false;
        }
        const auto type = frame.value(QStringLiteral("type"));
        if (type == QStringLiteral("ready")) {
            if (frame.size() != 1 || m_ready) {
                return false;
            }
            m_ready = true;
            if (!m_cancelRequested) {
                emit ready();
            }
            return true;
        }
        if (frame.size() != 2) {
            return false;
        }
        if (type == QStringLiteral("complete") || type == QStringLiteral("progress")) {
            const auto codes = parseCodes(frame.value(QStringLiteral("codes")));
            if (!m_ready || !codes) {
                return false;
            }
            if (type == QStringLiteral("complete")) {
                m_terminal = Terminal::Complete;
                m_codes = *codes;
            } else if (!m_cancelRequested) {
                emit progress(*codes);
            }
        } else if (type == QStringLiteral("cancelled") || type == QStringLiteral("error")) {
            m_reason = frame.value(QStringLiteral("reason")).toString();
            if (m_reason.trimmed().isEmpty()) {
                return false;
            }
            m_terminal =
                type == QStringLiteral("cancelled") ? Terminal::Cancelled : Terminal::Error;
        } else {
            return false;
        }
        if (m_terminal != Terminal::None) {
            m_ping.stop();
        }
        return true;
    }

    void finished(int code, QProcess::ExitStatus status)
    {
        readFrames();
        const auto terminal = m_terminal;
        const auto reason = std::move(m_reason);
        const auto codes = std::move(m_codes);
        const auto failure = std::move(m_failure);
        const bool cancelledByEditor = m_cancelRequested;
        const bool partial = !m_buffer.isEmpty();
        reset();

        if (!failure.isEmpty()) {
            emit failed(failure);
        } else if (partial || status != QProcess::NormalExit || terminal == Terminal::None ||
                   (terminal != Terminal::Error && code != 0) ||
                   (terminal == Terminal::Error && code == 0)) {
            emit failed(QStringLiteral("The recording helper closed unexpectedly."));
        } else if (terminal == Terminal::Error) {
            emit failed(reason);
        } else if (terminal == Terminal::Cancelled || cancelledByEditor) {
            emit cancelled(reason.isEmpty() ? QStringLiteral("Recording cancelled.") : reason);
        } else {
            emit completed(codes);
        }
    }

    QString m_helper;
    QString m_unavailable;
    QProcess m_process;
    QTimer m_ping;
    QTimer m_deadline;
    QTimer m_cancelDeadline;
    QByteArray m_buffer;
    QList<int> m_codes;
    QString m_failure;
    QString m_reason;
    Terminal m_terminal = Terminal::None;
    int m_frameCount = 0;
    bool m_active = false;
    bool m_ready = false;
    bool m_cancelRequested = false;
};
} // namespace

std::unique_ptr<CaptureBackend> makeCaptureBackend(bool enabled)
{
    QString reason;
    if (!enabled) {
        reason = QStringLiteral("Launch with --demo --recording to enable keyboard recording.");
    } else if (!QGuiApplication::platformName().startsWith(QStringLiteral("wayland")) ||
               qEnvironmentVariableIsEmpty("HYPRLAND_INSTANCE_SIGNATURE")) {
        reason = QStringLiteral("Keyboard recording requires a Hyprland Wayland session.");
    } else if (geteuid() == 0) {
        reason = QStringLiteral("Run keyboard recording as your signed-in desktop user.");
    }

    const auto helper = QCoreApplication::applicationDirPath() + QStringLiteral("/xkeys-record");
    return std::make_unique<ProcessCaptureBackend>(helper, reason, 15000);
}

std::unique_ptr<CaptureBackend> makeProcessCaptureBackend(const QString &helperPath, int watchdogMs)
{
    return std::make_unique<ProcessCaptureBackend>(helperPath, QString{}, watchdogMs);
}

} // namespace xkeys
