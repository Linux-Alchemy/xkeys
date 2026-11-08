#pragma once

#include "service/shortcut_recorder.h"

namespace xkeys::keyboard
{

enum class ReadKind { Empty, Ignored, Key, SyncDropped, Disconnected };

struct DeviceRead {
    ReadKind kind = ReadKind::Empty;
    int code = 0;
    int value = 0;
    qint64 timestampUs = 0;
};

struct ParentInput {
    bool heartbeat = false;
    QString cancelReason;
};

// The helper supplies evdev and pipe operations. Tests supply a fake event stream and clock.
class CaptureIo
{
  public:
    virtual ~CaptureIo() = default;
    virtual Result<void> openDevice(int source) = 0;
    virtual Result<void> grabDevice(int source) = 0;
    virtual Result<void> validateGroup() = 0;
    virtual Result<bool> neutral(int source) = 0;
    virtual Result<DeviceRead> readDevice(int source) = 0;
    virtual void closeDevice(int source) = 0;
    virtual qint64 elapsedMs() const = 0;
    virtual qint64 eventTimeUs() const = 0;
    virtual ParentInput readParent() = 0;
    virtual bool publish(const QString &type, const QList<int> &codes, const QString &reason) = 0;
    virtual void wait(int milliseconds) = 0;
    virtual void waitForDrain(int milliseconds) = 0;
};

int runCapture(const KeyboardSelection &selection, CaptureIo &io);
int captureKeyboard(const QString &id, const QString &fingerprint, qint64 processStart);

} // namespace xkeys::keyboard
