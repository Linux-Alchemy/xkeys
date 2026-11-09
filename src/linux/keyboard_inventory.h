#pragma once

#include "service/shortcut_recorder.h"

namespace xkeys
{

Result<QList<KeyboardSelection>> discoverKeyboards();

namespace keyboard
{

struct Interface {
    QString parentId;
    QString product;
    QString path;
    QString name;
    QString syspath;
    QString serial;
    QString capabilities;
    quint64 deviceNumber = 0;
    quint64 filesystemDevice = 0;
    quint64 inode = 0;
    qint64 ctimeSeconds = 0;
    qint64 ctimeNanoseconds = 0;
    int vendor = 0;
    int productId = 0;
    bool complete = false;
    bool keyboard = false;
    bool typing = false;
    bool readable = false;

    bool sameIdentity(const Interface &other) const;
};

struct Group {
    KeyboardSelection selection;
    QList<Interface> interfaces;
};

Result<QList<Group>> groupInterfaces(const QList<Interface> &interfaces);
Result<QList<Group>> discoverGroups();

} // namespace keyboard
} // namespace xkeys
