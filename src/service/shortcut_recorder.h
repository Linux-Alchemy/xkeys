#pragma once

#include "core/types.h"

#include <QSet>
#include <QStringList>

namespace xkeys
{

struct KeyboardSelection {
    QString id;
    QString name;
    QStringList nodes;
    QStringList interfaceNames;
    QStringList typingNames;
    bool accessible = false;
    QString reason;
    QString fingerprint{};
};

enum class CapturePhase { Idle, Recording, Complete, Cancelled, Rejected };

struct RawEvent {
    int source;
    int code;
    int value;
};

struct CaptureUpdate {
    CapturePhase phase = CapturePhase::Idle;
    QList<int> chord;
    QString message;
};

class Recorder
{
  public:
    Result<void> begin(const KeyboardSelection &keyboard);
    CaptureUpdate consume(const RawEvent &event);
    void cancel();
    CaptureUpdate snapshot() const;

  private:
    CaptureUpdate reject(const QString &message);

    QList<QSet<int>> m_held;
    CaptureUpdate m_update;
    bool m_releasing = false;
};

} // namespace xkeys
