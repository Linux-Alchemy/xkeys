#pragma once

#include "service/shortcut_recorder.h"

#include <QObject>

#include <memory>

namespace xkeys
{

class ShortcutContext : public QObject
{
    Q_OBJECT

  public:
    using QObject::QObject;
    virtual void refresh(const KeyboardSelection &keyboard) = 0;
    virtual QString label(const QList<int> &codes) const = 0;
    virtual QStringList conflicts(const QList<int> &codes) const = 0;
    virtual bool bindingsAvailable() const = 0;
    virtual QString advisory() const = 0;

  signals:
    void prepared(bool ok, const QString &reason);
};

std::unique_ptr<ShortcutContext> makeShortcutContext();
std::unique_ptr<ShortcutContext> makeShortcutContext(const QString &hyprctlPath);

} // namespace xkeys
