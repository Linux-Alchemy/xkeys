#include "keyboard_capture.h"

#include "keyboard_inventory.h"

#include <QElapsedTimer>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

#include <algorithm>
#include <array>
#include <cerrno>
#include <climits>
#include <csignal>
#include <fcntl.h>
#include <libevdev/libevdev.h>
#include <linux/input.h>
#include <memory>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace xkeys::keyboard
{
namespace
{
constexpr qint64 heartbeatLimit = 1000;
constexpr qint64 recordingLimit = 10000;
constexpr qint64 drainLimit = 2000;
constexpr qint64 lifetimeLimit = 15000;
constexpr qint64 completionQuietMs = 40;
constexpr qint64 topologyCheckMs = 250;
constexpr int maxDevices = 16;
constexpr int batchLimit = 128;
constexpr int pipeFrameLimit = 4096;
constexpr int pollInterval = 20;

struct TimedKey {
    RawEvent event;
    qint64 timestampUs;
};

class CaptureRun
{
  public:
    CaptureRun(const KeyboardSelection &selection, CaptureIo &io) : m_selection(selection), m_io(io)
    {
    }

    int run()
    {
        if (!m_selection.accessible) {
            return finish(QStringLiteral("error"),
                          m_selection.reason.isEmpty()
                              ? QStringLiteral("The selected keyboard is unavailable.")
                              : m_selection.reason);
        }
        const auto started = m_recorder.begin(m_selection);
        if (!started) {
            return finish(QStringLiteral("error"), started.error().message);
        }
        m_count = static_cast<int>(m_selection.nodes.size());

        for (int source = 0; source < m_count; ++source) {
            if (const auto reason = interruption(); !reason.isEmpty()) {
                return finish(QStringLiteral("cancelled"), reason);
            }
            const auto opened = m_io.openDevice(source);
            if (!opened) {
                return finish(QStringLiteral("error"), opened.error().message);
            }
            m_open.at(source) = true;
        }
        const auto verified = m_io.validateGroup();
        if (!verified) {
            return finish(QStringLiteral("error"), verified.error().message);
        }
        if (const auto error = checkNeutral(); !error.isEmpty()) {
            return finish(QStringLiteral("error"), error);
        }

        for (int source = 0; source < m_count; ++source) {
            if (const auto reason = interruption(); !reason.isEmpty()) {
                return finish(QStringLiteral("cancelled"), reason);
            }
            if (const auto error = checkNeutral(source); !error.isEmpty()) {
                return finish(QStringLiteral("error"), error);
            }
            const auto grabbed = m_io.grabDevice(source);
            if (!grabbed) {
                return finish(QStringLiteral("error"), grabbed.error().message);
            }
            m_grabbed.at(source) = true;
        }
        if (const auto error = checkNeutral(); !error.isEmpty()) {
            return finish(QStringLiteral("error"), error);
        }

        const auto armedGroup = m_io.validateGroup();
        if (!armedGroup) {
            return finish(QStringLiteral("error"), armedGroup.error().message);
        }
        const qint64 armingCutoff = m_io.eventTimeUs();
        if (armingCutoff < 0) {
            return finish(QStringLiteral("error"), captureClockError());
        }
        if (const auto error = checkNeutral(); !error.isEmpty()) {
            return finish(QStringLiteral("error"), error);
        }
        QList<TimedKey> stale;
        if (const auto reason = collect(stale); !reason.isEmpty()) {
            return finish(QStringLiteral("error"), reason);
        }
        if (!stale.isEmpty()) {
            return finish(QStringLiteral("error"), armingInputError());
        }
        if (const auto reason = interruption(); !reason.isEmpty()) {
            return finish(QStringLiteral("cancelled"), reason);
        }

        const qint64 readyAt = m_io.eventTimeUs();
        if (readyAt < armingCutoff) {
            return finish(QStringLiteral("error"), captureClockError());
        }
        const qint64 deadline = m_io.elapsedMs() + recordingLimit;
        if (!m_io.publish(QStringLiteral("ready"), {}, {})) {
            return finish(QStringLiteral("cancelled"), editorOutputError());
        }
        return record(deadline, readyAt);
    }

  private:
    static QString captureClockError() { return QStringLiteral("Cannot read the capture clock."); }
    static QString armingInputError()
    {
        return QStringLiteral("Keys changed while preparing capture. Release them and try again.");
    }
    static QString editorOutputError()
    {
        return QStringLiteral("The editor stopped accepting capture updates.");
    }

    QString interruption(qint64 deadline = lifetimeLimit)
    {
        const auto input = m_io.readParent();
        if (!input.cancelReason.isEmpty()) {
            return input.cancelReason;
        }
        const qint64 now = m_io.elapsedMs();
        if (now >= std::min(deadline, lifetimeLimit)) {
            return QStringLiteral("Keyboard recording timed out.");
        }
        if (now - m_lastHeartbeat >= heartbeatLimit) {
            return QStringLiteral("The editor did not renew the capture lease.");
        }
        if (input.heartbeat) {
            m_lastHeartbeat = now;
        }
        return {};
    }

    QString checkNeutral(int source)
    {
        const auto state = m_io.neutral(source);
        if (!state) {
            return state.error().message;
        }
        if (!state.value()) {
            return QStringLiteral("Release all keys on the selected keyboard and try again.");
        }
        return {};
    }

    QString checkNeutral()
    {
        for (int source = 0; source < m_count; ++source) {
            if (!m_open.at(source)) {
                continue;
            }
            if (const auto reason = checkNeutral(source); !reason.isEmpty()) {
                return reason;
            }
        }
        return {};
    }

    QString collect(QList<TimedKey> &keys)
    {
        for (int source = 0; source < m_count; ++source) {
            if (!m_open.at(source)) {
                continue;
            }
            bool empty = false;
            for (int count = 0; count < batchLimit && !empty; ++count) {
                const auto read = m_io.readDevice(source);
                if (!read) {
                    return read.error().message;
                }
                switch (read.value().kind) {
                case ReadKind::Empty:
                    empty = true;
                    break;
                case ReadKind::Ignored:
                    break;
                case ReadKind::Key:
                    if (keys.size() >= maxDevices * batchLimit) {
                        return QStringLiteral(
                            "The keyboard sent too much input at once. Try again.");
                    }
                    keys.append({{source, read.value().code, read.value().value},
                                 read.value().timestampUs});
                    break;
                case ReadKind::SyncDropped:
                    return QStringLiteral(
                        "The keyboard lost input events. Record the shortcut again.");
                case ReadKind::Disconnected:
                    close(source);
                    return QStringLiteral("A selected keyboard interface disconnected.");
                }
            }
            if (!empty) {
                return QStringLiteral("The keyboard sent too much input at once. Try again.");
            }
        }
        std::stable_sort(keys.begin(), keys.end(),
                         [](const TimedKey &first, const TimedKey &second) {
                             return first.timestampUs < second.timestampUs;
                         });
        return {};
    }

    QString validateTime(const TimedKey &key, qint64 readyAt)
    {
        if (key.timestampUs < readyAt) {
            return armingInputError();
        }
        if (key.timestampUs < m_lastKeyTime) {
            return QStringLiteral("Keyboard events arrived out of order. Try recording again.");
        }
        if (key.timestampUs > m_lastKeyTime) {
            m_lastKeyTime = key.timestampUs;
            m_pressSources = 0;
            m_releaseSources = 0;
        }

        const unsigned source = 1U << key.event.source;
        if ((key.event.value == 0 && (m_pressSources & ~source) != 0) ||
            (key.event.value == 1 && (m_releaseSources & ~source) != 0)) {
            return QStringLiteral("Keyboard interfaces reported an ambiguous press and release.");
        }
        if (key.event.value == 0) {
            m_releaseSources |= source;
        } else if (key.event.value == 1) {
            m_pressSources |= source;
        }
        return {};
    }

    int record(qint64 deadline, qint64 readyAt)
    {
        QList<int> lastProgress;
        QList<TimedKey> keys;
        std::optional<qint64> quietUntil;
        qint64 nextTopologyCheck = m_io.elapsedMs() + topologyCheckMs;
        while (true) {
            if (const auto reason = interruption(deadline); !reason.isEmpty()) {
                return cancel(reason);
            }
            if (m_io.elapsedMs() >= nextTopologyCheck) {
                const auto verified = m_io.validateGroup();
                if (!verified) {
                    return cancel(verified.error().message);
                }
                nextTopologyCheck = m_io.elapsedMs() + topologyCheckMs;
                if (const auto reason = interruption(deadline); !reason.isEmpty()) {
                    return cancel(reason);
                }
            }
            // Events arriving during this sweep wait for the next one, so a
            // source visited early can contribute before they are processed.
            const qint64 watermark = m_io.eventTimeUs();
            if (watermark < readyAt) {
                return cancel(captureClockError());
            }
            if (const auto reason = collect(keys); !reason.isEmpty()) {
                return cancel(reason);
            }

            qsizetype processed = 0;
            for (const auto &key : std::as_const(keys)) {
                if (key.timestampUs > watermark) {
                    break;
                }
                ++processed;
                if (const auto reason = interruption(deadline); !reason.isEmpty()) {
                    return cancel(reason);
                }
                if (const auto reason = validateTime(key, readyAt); !reason.isEmpty()) {
                    return cancel(reason);
                }
                if (quietUntil) {
                    if (key.event.value != 2) {
                        return cancel(
                            QStringLiteral("Press and release only one shortcut at a time."));
                    }
                    continue;
                }
                const auto update = m_recorder.consume(key.event);
                if (update.phase == CapturePhase::Rejected) {
                    return cancel(update.message);
                }
                if (update.phase == CapturePhase::Complete) {
                    quietUntil = m_io.elapsedMs() + completionQuietMs;
                }
                if (update.chord != lastProgress) {
                    lastProgress = update.chord;
                    if (!m_io.publish(QStringLiteral("progress"), lastProgress, {})) {
                        return cancel(editorOutputError());
                    }
                }
            }
            keys.remove(0, processed);

            if (quietUntil && m_io.elapsedMs() >= *quietUntil && keys.isEmpty()) {
                const auto verified = m_io.validateGroup();
                if (!verified) {
                    return cancel(verified.error().message);
                }
                if (const auto reason = interruption(deadline); !reason.isEmpty()) {
                    return cancel(reason);
                }
                if (const auto reason = checkNeutral(); !reason.isEmpty()) {
                    return cancel(reason);
                }
                if (const auto reason = interruption(deadline); !reason.isEmpty()) {
                    return cancel(reason);
                }
                // One more sweep catches activity queued during the neutral ioctls.
                if (const auto reason = collect(keys); !reason.isEmpty()) {
                    return cancel(reason);
                }
                if (const auto reason = interruption(deadline); !reason.isEmpty()) {
                    return cancel(reason);
                }
                if (keys.isEmpty()) {
                    return finish(QStringLiteral("complete"), {}, m_recorder.snapshot().chord);
                }
                continue;
            }

            const auto nextDeadline = std::min(
                {deadline, lifetimeLimit, m_lastHeartbeat + heartbeatLimit, nextTopologyCheck});
            const auto remaining = nextDeadline - m_io.elapsedMs();
            if (remaining > 0) {
                m_io.wait(static_cast<int>(std::min<qint64>(pollInterval, remaining)));
            }
        }
    }

    int cancel(QString reason)
    {
        m_recorder.cancel();
        const qint64 deadline = std::min(m_io.elapsedMs() + drainLimit, lifetimeLimit);
        bool neutral = false;
        while (m_io.elapsedMs() < deadline) {
            neutral = true;
            for (int source = 0; source < m_count; ++source) {
                if (!m_grabbed.at(source)) {
                    continue;
                }
                drainQueue(source, deadline);
                if (m_io.elapsedMs() >= deadline) {
                    neutral = false;
                    break;
                }
                if (!m_grabbed.at(source)) {
                    continue;
                }
                const auto state = m_io.neutral(source);
                if (!state && state.error().code == ErrorCode::NotFound) {
                    close(source);
                    continue;
                }
                if (!state || !state.value()) {
                    neutral = false;
                }
            }
            if (neutral) {
                break;
            }

            const auto remaining = deadline - m_io.elapsedMs();
            if (remaining > 0) {
                m_io.waitForDrain(static_cast<int>(std::min<qint64>(pollInterval, remaining)));
            }
        }
        if (!neutral) {
            reason +=
                QStringLiteral(" The grab ended at its safety deadline. Release any held keys.");
        }
        return finish(QStringLiteral("cancelled"), reason);
    }

    void drainQueue(int source, qint64 deadline)
    {
        for (int count = 0; count < batchLimit && m_io.elapsedMs() < deadline; ++count) {
            const auto read = m_io.readDevice(source);
            if (!read || read.value().kind == ReadKind::Empty) {
                return;
            }
            if (read.value().kind == ReadKind::Disconnected) {
                close(source);
                return;
            }
        }
    }

    void close(int source)
    {
        if (m_open.at(source)) {
            m_io.closeDevice(source);
            m_open.at(source) = false;
            m_grabbed.at(source) = false;
        }
    }

    int finish(const QString &type, const QString &reason, const QList<int> &codes = {})
    {
        for (int source = 0; source < m_count; ++source) {
            close(source);
        }
        const bool sent = m_io.publish(type, codes, reason);
        return sent && type != QStringLiteral("error") ? 0 : 1;
    }

    const KeyboardSelection &m_selection;
    CaptureIo &m_io;
    Recorder m_recorder;
    std::array<bool, maxDevices> m_open{};
    std::array<bool, maxDevices> m_grabbed{};
    int m_count = 0;
    qint64 m_lastHeartbeat = 0;
    qint64 m_lastKeyTime = -1;
    unsigned m_pressSources = 0;
    unsigned m_releaseSources = 0;
};

bool writeFrame(const QString &type, const QList<int> &codes = {}, const QString &reason = {})
{
    QJsonObject object{{QStringLiteral("type"), type}};
    if (!codes.isEmpty()) {
        QJsonArray keys;
        for (const int code : codes) {
            keys.append(code);
        }
        object.insert(QStringLiteral("codes"), keys);
    }
    if (!reason.isEmpty()) {
        object.insert(QStringLiteral("reason"), reason);
    }

    QByteArray frame = QJsonDocument(object).toJson(QJsonDocument::Compact);
    frame.append('\n');
    return frame.size() <= pipeFrameLimit &&
           ::write(STDOUT_FILENO, frame.constData(), static_cast<std::size_t>(frame.size())) ==
               frame.size();
}

volatile std::sig_atomic_t interrupted = 0;

void requestCancel(int) { interrupted = 1; }

bool configureCaptureProcess()
{
    struct stat input{};
    struct stat output{};
    if (fstat(STDIN_FILENO, &input) != 0 || fstat(STDOUT_FILENO, &output) != 0 ||
        !S_ISFIFO(input.st_mode) || !S_ISFIFO(output.st_mode)) {
        return false;
    }
    for (const int fd : {STDIN_FILENO, STDOUT_FILENO}) {
        const int flags = fcntl(fd, F_GETFL);
        if (flags < 0 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
            return false;
        }
    }

    struct sigaction cancelAction{};
    cancelAction.sa_handler = requestCancel;
    struct sigaction ignoreAction{};
    ignoreAction.sa_handler = SIG_IGN;
    return sigemptyset(&cancelAction.sa_mask) == 0 && sigemptyset(&ignoreAction.sa_mask) == 0 &&
           sigaction(SIGTERM, &cancelAction, nullptr) == 0 &&
           sigaction(SIGINT, &cancelAction, nullptr) == 0 &&
           sigaction(SIGPIPE, &ignoreAction, nullptr) == 0;
}

class DeviceHandle
{
  public:
    ~DeviceHandle() { close(); }

    DeviceHandle() = default;
    DeviceHandle(const DeviceHandle &) = delete;
    DeviceHandle &operator=(const DeviceHandle &) = delete;

    void close()
    {
        device.reset();
        if (fd >= 0) {
            ::close(fd);
            fd = -1;
        }
    }

    int fd = -1;
    std::unique_ptr<libevdev, decltype(&libevdev_free)> device{nullptr, libevdev_free};
    bool syncing = false;
};

bool matchesDescriptor(int fd, const Interface &expected)
{
    struct stat metadata{};
    input_id identity{};
    return fstat(fd, &metadata) == 0 && S_ISCHR(metadata.st_mode) &&
           static_cast<quint64>(metadata.st_rdev) == expected.deviceNumber &&
           static_cast<quint64>(metadata.st_dev) == expected.filesystemDevice &&
           static_cast<quint64>(metadata.st_ino) == expected.inode &&
           metadata.st_ctim.tv_sec == expected.ctimeSeconds &&
           metadata.st_ctim.tv_nsec == expected.ctimeNanoseconds &&
           ioctl(fd, EVIOCGID, &identity) == 0 && identity.bustype == BUS_USB &&
           identity.vendor == expected.vendor && identity.product == expected.productId;
}

class LinuxCaptureIo final : public CaptureIo
{
  public:
    LinuxCaptureIo(const Group &group, qint64 processStart)
        : m_group(group), m_processStart(processStart)
    {
        m_clock.start();
    }

    Result<void> openDevice(int source) override
    {
        const auto &expected = m_group.interfaces.at(source);
        auto &handle = m_devices.at(source);
        const auto path = QFile::encodeName(expected.path);
        handle.fd = ::open(path.constData(), O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (handle.fd < 0) {
            return Result<void>::failure(
                ErrorCode::Io, QStringLiteral("Cannot open the selected keyboard interface %1.")
                                   .arg(expected.name));
        }
        if (!matchesDescriptor(handle.fd, expected)) {
            handle.close();
            return Result<void>::failure(
                ErrorCode::Invalid,
                QStringLiteral("The selected keyboard interface changed. Choose it again."));
        }

        libevdev *device = nullptr;
        if (libevdev_new_from_fd(handle.fd, &device) < 0) {
            handle.close();
            return Result<void>::failure(
                ErrorCode::Io,
                QStringLiteral("Cannot initialize the selected keyboard interface."));
        }
        handle.device.reset(device);
        const QString name = QString::fromUtf8(libevdev_get_name(device)).trimmed();
        if (name != expected.name || libevdev_set_clock_id(device, CLOCK_MONOTONIC) < 0) {
            handle.close();
            return Result<void>::failure(
                ErrorCode::Invalid,
                QStringLiteral("The selected keyboard interface could not be verified."));
        }
        return Result<void>::success();
    }

    Result<void> grabDevice(int source) override
    {
        auto &handle = m_devices.at(source);
        const int result = libevdev_grab(handle.device.get(), LIBEVDEV_GRAB);
        if (result == -EBUSY) {
            return Result<void>::failure(
                ErrorCode::Conflict,
                QStringLiteral(
                    "A selected keyboard interface is already grabbed by another program."));
        }
        if (result < 0) {
            return Result<void>::failure(
                ErrorCode::Io,
                QStringLiteral(
                    "Cannot exclusively grab every interface of the selected keyboard."));
        }
        return Result<void>::success();
    }

    Result<void> validateGroup() override
    {
        const auto current = discoverGroups();
        if (!current) {
            return Result<void>::failure(current.error().code, current.error().message);
        }
        const auto &groups = current.value();
        const auto found = std::ranges::find_if(groups, [this](const Group &group) {
            return group.selection.id == m_group.selection.id;
        });
        if (found == groups.end() || !found->selection.accessible ||
            found->selection.fingerprint != m_group.selection.fingerprint ||
            found->interfaces.size() != m_group.interfaces.size()) {
            return changedGroup();
        }
        for (qsizetype index = 0; index < found->interfaces.size(); ++index) {
            const auto &interface = found->interfaces.at(index);
            if (!interface.sameIdentity(m_group.interfaces.at(index)) ||
                !matchesDescriptor(m_devices.at(index).fd, interface)) {
                return changedGroup();
            }
        }
        return Result<void>::success();
    }

    Result<bool> neutral(int source) override
    {
        std::array<unsigned char, (KEY_CNT + CHAR_BIT - 1) / CHAR_BIT> bits{};
        if (ioctl(m_devices.at(source).fd, EVIOCGKEY(bits.size()), bits.data()) < 0) {
            if (errno == ENODEV) {
                return Result<bool>::failure(
                    ErrorCode::NotFound,
                    QStringLiteral("A selected keyboard interface disconnected."));
            }
            return Result<bool>::failure(
                ErrorCode::Io, QStringLiteral("Cannot check the selected keyboard's held keys."));
        }
        return Result<bool>::success(
            std::ranges::none_of(bits, [](unsigned char bitsByte) { return bitsByte != 0; }));
    }

    Result<DeviceRead> readDevice(int source) override
    {
        using Read = Result<DeviceRead>;
        auto &handle = m_devices.at(source);
        input_event event{};
        const auto mode = handle.syncing ? LIBEVDEV_READ_FLAG_SYNC : LIBEVDEV_READ_FLAG_NORMAL;
        const int result = libevdev_next_event(handle.device.get(), mode, &event);
        if (result == -EAGAIN) {
            handle.syncing = false;
            return Read::success({});
        }
        if (result == -EINTR) {
            return Read::success({ReadKind::Ignored});
        }
        if (result == -ENODEV) {
            return Read::success({ReadKind::Disconnected});
        }
        if (result < 0) {
            return Read::failure(ErrorCode::Io,
                                 QStringLiteral("Cannot read the selected keyboard interface."));
        }
        if (handle.syncing) {
            return Read::success({ReadKind::Ignored});
        }
        if (result == LIBEVDEV_READ_STATUS_SYNC) {
            handle.syncing = true;
            return Read::success({ReadKind::SyncDropped});
        }
        if (event.type != EV_KEY) {
            return Read::success({ReadKind::Ignored});
        }
        const qint64 time =
            static_cast<qint64>(event.input_event_sec) * 1000000 + event.input_event_usec;
        return Read::success({ReadKind::Key, event.code, event.value, time});
    }

    void closeDevice(int source) override { m_devices.at(source).close(); }

    qint64 elapsedMs() const override
    {
        return m_clock.msecsSinceReference() - m_processStart + m_clock.elapsed();
    }

    qint64 eventTimeUs() const override
    {
        timespec time{};
        if (clock_gettime(CLOCK_MONOTONIC, &time) != 0) {
            return -1;
        }
        return static_cast<qint64>(time.tv_sec) * 1000000 + time.tv_nsec / 1000;
    }

    ParentInput readParent() override
    {
        if (interrupted) {
            return {false, QStringLiteral("Keyboard recording was interrupted.")};
        }
        if (m_inputClosed) {
            return {false, QStringLiteral("The editor disconnected.")};
        }

        std::array<char, pipeFrameLimit> bytes{};
        const auto count = ::read(STDIN_FILENO, bytes.data(), bytes.size());
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) {
            return {};
        }
        if (count <= 0) {
            m_inputClosed = true;
            return {false, QStringLiteral("The editor disconnected.")};
        }

        m_parentBuffer.append(bytes.data(), count);
        bool heartbeat = false;
        qsizetype start = 0;
        while (true) {
            const auto newline = m_parentBuffer.indexOf('\n', start);
            if (newline < 0) {
                break;
            }
            const auto line = QByteArrayView(m_parentBuffer).sliced(start, newline - start);
            if (line == "cancel") {
                return {false, QStringLiteral("Keyboard recording was cancelled.")};
            }
            if (line != "ping") {
                return {false, QStringLiteral("The editor sent an invalid capture command.")};
            }
            heartbeat = true;
            start = newline + 1;
        }
        m_parentBuffer.remove(0, start);
        if (m_parentBuffer.size() > pipeFrameLimit) {
            return {false, QStringLiteral("The editor sent an oversized capture command.")};
        }
        return {heartbeat, {}};
    }

    bool publish(const QString &type, const QList<int> &codes, const QString &reason) override
    {
        return writeFrame(type, codes, reason);
    }

    void wait(int milliseconds) override
    {
        std::array<pollfd, maxDevices + 1> fds{};
        nfds_t count = 0;
        if (!m_inputClosed) {
            fds.at(count++) = {STDIN_FILENO, POLLIN, 0};
        }
        for (const auto &device : m_devices) {
            if (device.fd >= 0) {
                fds.at(count++) = {device.fd, POLLIN, 0};
            }
        }
        poll(fds.data(), count, milliseconds);
    }

    void waitForDrain(int milliseconds) override
    {
        // A disconnected or faulty descriptor may always signal POLLERR.
        // Polling only the clock here keeps the drain bounded without spinning.
        poll(nullptr, 0, milliseconds);
    }

  private:
    static Result<void> changedGroup()
    {
        return Result<void>::failure(
            ErrorCode::Invalid,
            QStringLiteral("The selected keyboard group changed. Choose it again."));
    }

    const Group &m_group;
    qint64 m_processStart;
    QElapsedTimer m_clock;
    std::array<DeviceHandle, maxDevices> m_devices{};
    QByteArray m_parentBuffer;
    bool m_inputClosed = false;
};
} // namespace

int runCapture(const KeyboardSelection &selection, CaptureIo &io)
{
    return CaptureRun(selection, io).run();
}

int captureKeyboard(const QString &id, const QString &fingerprint, qint64 processStart)
{
    if (!configureCaptureProcess()) {
        return 2;
    }
    if (getuid() == 0 || geteuid() == 0) {
        writeFrame(QStringLiteral("error"), {},
                   QStringLiteral("Run the recorder as the desktop user."));
        return 2;
    }

    const auto inventory = discoverGroups();
    if (!inventory) {
        writeFrame(QStringLiteral("error"), {}, inventory.error().message);
        return 1;
    }
    const auto &groups = inventory.value();
    const auto group = std::ranges::find_if(
        groups, [&id](const Group &candidate) { return candidate.selection.id == id; });
    if (group == groups.end()) {
        writeFrame(QStringLiteral("error"), {},
                   QStringLiteral("Choose a currently connected USB keyboard."));
        return 1;
    }
    if (!group->selection.accessible || group->selection.fingerprint != fingerprint) {
        writeFrame(QStringLiteral("error"), {},
                   group->selection.reason.isEmpty()
                       ? QStringLiteral("The selected keyboard group changed. Choose it again.")
                       : group->selection.reason);
        return 1;
    }

    LinuxCaptureIo io(*group, processStart);
    return runCapture(group->selection, io);
}

} // namespace xkeys::keyboard
