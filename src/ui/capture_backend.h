#pragma once

#include "service/shortcut_recorder.h"

#include <QObject>

#include <memory>

namespace xkeys
{

class CaptureBackend : public QObject
{
    Q_OBJECT

  public:
    using QObject::QObject;
    virtual bool available() const = 0;
    virtual QString unavailableReason() const = 0;
    virtual Result<QList<KeyboardSelection>> keyboards() = 0;
    virtual void start(const KeyboardSelection &keyboard) = 0;
    virtual void cancel() = 0;

  signals:
    void ready();
    void progress(const QList<int> &codes);
    void completed(const QList<int> &codes);
    void cancelled(const QString &reason);
    void failed(const QString &reason);
};

std::unique_ptr<CaptureBackend> makeCaptureBackend(bool enabled);
std::unique_ptr<CaptureBackend> makeProcessCaptureBackend(const QString &helperPath,
                                                          int watchdogMs = 15000);

} // namespace xkeys
