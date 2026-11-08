#include "keyboard_inventory.h"

#include <QCryptographicHash>
#include <QDataStream>
#include <QFile>
#include <QMap>
#include <QSet>

#include <algorithm>
#include <climits>
#include <libevdev/libevdev.h>
#include <libudev.h>
#include <linux/input-event-codes.h>
#include <memory>
#include <sys/stat.h>
#include <unistd.h>

namespace xkeys::keyboard
{
namespace
{
constexpr qsizetype maxNodes = 128;
constexpr qsizetype maxGroupNodes = 16;
constexpr int wordBits = sizeof(unsigned long) * CHAR_BIT;
using Udev = std::unique_ptr<udev, decltype(&udev_unref)>;
using Enumeration = std::unique_ptr<udev_enumerate, decltype(&udev_enumerate_unref)>;
using Device = std::unique_ptr<udev_device, decltype(&udev_device_unref)>;

QString text(const char *value) { return QString::fromUtf8(value); }

bool eventName(QStringView name)
{
    return name.startsWith(u"event") && name.size() > 5 &&
           std::ranges::all_of(name.sliced(5), [](QChar character) {
               return character >= u'0' && character <= u'9';
           });
}

bool eventPath(const QString &path)
{
    return path.startsWith(QStringLiteral("/dev/input/")) &&
           eventName(QStringView(path).sliced(11));
}

std::optional<QList<unsigned long>> keyBitmap(const QString &raw)
{
    const auto words = raw.split(u' ', Qt::SkipEmptyParts);
    if (words.isEmpty() || words.size() > (KEY_CNT + wordBits - 1) / wordBits) {
        return std::nullopt;
    }
    QList<unsigned long> bits;
    for (auto word = words.crbegin(); word != words.crend(); ++word) {
        bool valid = false;
        const auto value = word->toULong(&valid, 16);
        if (!valid) {
            return std::nullopt;
        }
        bits.append(value);
    }
    return bits;
}

bool keyPresent(const QList<unsigned long> &bits, int code)
{
    return (bits.value(code / wordBits) & (1UL << (code % wordBits))) != 0;
}

bool keyboardBearing(const QList<unsigned long> &bits)
{
    for (int code = KEY_RESERVED + 1; code < KEY_CNT; ++code) {
        if (!keyPresent(bits, code)) {
            continue;
        }
        const char *name = libevdev_event_code_get_name(EV_KEY, code);
        if (code < BTN_MISC || (name && QByteArrayView(name).startsWith("KEY_"))) {
            return true;
        }
    }
    return false;
}

bool usbIdentity(const QString &id)
{
    return id.startsWith(QStringLiteral("/sys/devices/")) &&
           !id.startsWith(QStringLiteral("/sys/devices/virtual/"));
}

Interface inspect(udev_device *node, udev_device *usb)
{
    Interface result;
    result.parentId = text(udev_device_get_syspath(usb));
    result.product = text(udev_device_get_sysattr_value(usb, "product")).trimmed();
    result.serial = text(udev_device_get_sysattr_value(usb, "serial"));
    result.path = text(udev_device_get_devnode(node));
    result.syspath = text(udev_device_get_syspath(node));
    result.deviceNumber = static_cast<quint64>(udev_device_get_devnum(node));

    bool vendorValid = false;
    bool productValid = false;
    result.vendor = text(udev_device_get_sysattr_value(usb, "idVendor")).toUShort(&vendorValid, 16);
    result.productId =
        text(udev_device_get_sysattr_value(usb, "idProduct")).toUShort(&productValid, 16);
    auto *input = udev_device_get_parent_with_subsystem_devtype(node, "input", nullptr);
    result.capabilities =
        input ? text(udev_device_get_sysattr_value(input, "capabilities/key")) : QString{};
    const auto bits = keyBitmap(result.capabilities);
    if (bits) {
        result.keyboard = keyboardBearing(*bits);
        result.typing =
            keyPresent(*bits, KEY_A) && keyPresent(*bits, KEY_Z) && keyPresent(*bits, KEY_ENTER);
    }
    if (input) {
        result.name = text(udev_device_get_sysattr_value(input, "name")).trimmed();
    }

    const QByteArray path = QFile::encodeName(result.path);
    struct stat info{};
    const bool characterDevice = eventPath(result.path) && lstat(path.constData(), &info) == 0 &&
                                 S_ISCHR(info.st_mode) &&
                                 static_cast<quint64>(info.st_rdev) == result.deviceNumber;
    if (characterDevice) {
        result.filesystemDevice = static_cast<quint64>(info.st_dev);
        result.inode = static_cast<quint64>(info.st_ino);
        result.ctimeSeconds = info.st_ctim.tv_sec;
        result.ctimeNanoseconds = info.st_ctim.tv_nsec;
    }
    result.complete = usbIdentity(result.parentId) && vendorValid && productValid && input &&
                      bits && characterDevice && (!result.keyboard || !result.name.isEmpty());
    result.readable = characterDevice && access(path.constData(), R_OK) == 0;
    return result;
}

QString fingerprint(const QString &groupId, QList<Interface> interfaces)
{
    std::ranges::sort(interfaces, {}, &Interface::syspath);
    QByteArray bytes;
    QDataStream stream(&bytes, QIODevice::WriteOnly);
    stream.setVersion(QDataStream::Qt_6_0);
    stream << groupId << static_cast<qint32>(interfaces.size());
    for (const auto &item : std::as_const(interfaces)) {
        stream << item.parentId << item.vendor << item.productId << item.serial << item.syspath
               << item.path << item.deviceNumber << item.filesystemDevice << item.inode
               << item.ctimeSeconds << item.ctimeNanoseconds << item.name << item.capabilities
               << item.keyboard << item.typing;
    }
    return QString::fromLatin1(QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

Group makeGroup(const QString &id, const QString &name, const QList<Interface> &nodes,
                const QString &unavailable = {})
{
    Group group;
    auto &selection = group.selection;
    selection.id = id;
    selection.name = name.isEmpty() ? QStringLiteral("USB keyboard") : name;

    bool complete = nodes.size() <= maxGroupNodes;
    bool readable = true;
    QSet<QString> paths;
    QSet<quint64> deviceNumbers;
    QMap<QString, std::pair<int, int>> parentDevices;
    for (const auto &interface : nodes) {
        const auto identity = std::pair{interface.vendor, interface.productId};
        if (!interface.complete || !eventPath(interface.path) ||
            !interface.syspath.startsWith(interface.parentId + u'/') ||
            paths.contains(interface.path) || deviceNumbers.contains(interface.deviceNumber) ||
            parentDevices.value(interface.parentId, identity) != identity) {
            complete = false;
        }
        paths.insert(interface.path);
        deviceNumbers.insert(interface.deviceNumber);
        parentDevices.insert(interface.parentId, identity);
        if (interface.keyboard) {
            group.interfaces.append(interface);
            readable = readable && interface.readable;
        }
    }

    std::ranges::sort(group.interfaces, {}, &Interface::path);
    for (const auto &interface : std::as_const(group.interfaces)) {
        selection.nodes.append(interface.path);
        selection.interfaceNames.append(interface.name);
        if (interface.typing) {
            selection.typingNames.append(interface.name);
        }
    }

    selection.accessible =
        unavailable.isEmpty() && complete && readable && !selection.typingNames.isEmpty();
    if (selection.accessible) {
        selection.fingerprint = fingerprint(selection.id, nodes);
    } else if (!unavailable.isEmpty()) {
        selection.reason = unavailable;
    } else if (!complete || selection.typingNames.isEmpty()) {
        selection.reason =
            QStringLiteral("This USB keyboard group could not be verified completely.");
    } else {
        selection.reason =
            QStringLiteral("This session cannot read every interface of this keyboard.");
    }
    return group;
}

constexpr int keychronVendor = 0x3434;
constexpr int q1MaxWired = 0x0810;
constexpr int keychronLink = 0xd030;

bool q1MaxRoute(const Interface &interface)
{
    return interface.vendor == keychronVendor &&
           (interface.productId == q1MaxWired || interface.productId == keychronLink);
}
} // namespace

bool Interface::sameIdentity(const Interface &other) const
{
    return complete && other.complete && parentId == other.parentId && path == other.path &&
           syspath == other.syspath && deviceNumber == other.deviceNumber &&
           vendor == other.vendor && productId == other.productId && name == other.name &&
           keyboard == other.keyboard && typing == other.typing && serial == other.serial &&
           capabilities == other.capabilities && filesystemDevice == other.filesystemDevice &&
           inode == other.inode && ctimeSeconds == other.ctimeSeconds &&
           ctimeNanoseconds == other.ctimeNanoseconds;
}

Result<QList<Group>> groupInterfaces(const QList<Interface> &interfaces)
{
    using Groups = Result<QList<Group>>;
    if (interfaces.size() > maxNodes) {
        return Groups::failure(
            ErrorCode::Invalid,
            QStringLiteral("Too many input interfaces to identify a keyboard safely."));
    }

    QMap<QString, QList<Interface>> usbGroups;
    for (const auto &interface : interfaces) {
        if (usbIdentity(interface.parentId) &&
            !(interface.vendor == 0x1532 && interface.productId == 0x022b)) {
            usbGroups[interface.parentId].append(interface);
        }
    }

    QList<Group> groups;
    QList<Interface> q1MaxNodes;
    int wired = 0;
    int wireless = 0;
    bool incompleteRoute = false;
    for (auto it = usbGroups.cbegin(); it != usbGroups.cend(); ++it) {
        const auto &nodes = it.value();
        const bool hasTyping = std::ranges::any_of(nodes, &Interface::typing);
        const auto route = std::ranges::find_if(nodes, q1MaxRoute);
        if (route != nodes.end()) {
            if (route->productId == q1MaxWired) {
                ++wired;
            } else {
                ++wireless;
            }
            incompleteRoute = incompleteRoute || !hasTyping;
            q1MaxNodes.append(nodes);
        } else if (hasTyping) {
            groups.append(makeGroup(it.key(), nodes.first().product, nodes));
        }
    }

    if (!q1MaxNodes.isEmpty()) {
        QString name = QStringLiteral("Keychron Q1 Max — ");
        if (wired > 0 && wireless > 0) {
            name += QStringLiteral("wired / 2.4 GHz");
        } else {
            name += wired > 0 ? QStringLiteral("wired") : QStringLiteral("2.4 GHz");
        }
        QString unavailable;
        if (wired > 1 || wireless > 1) {
            unavailable = QStringLiteral("Multiple matching Keychron routes are connected. "
                                         "Disconnect the extra keyboard or receiver.");
        } else if (incompleteRoute) {
            unavailable = QStringLiteral("An attached Keychron keyboard route is incomplete.");
        }
        groups.append(
            makeGroup(QStringLiteral("keyboard:keychron-q1-max"), name, q1MaxNodes, unavailable));
    }

    std::ranges::sort(groups, [](const Group &first, const Group &second) {
        const int nameOrder =
            first.selection.name.compare(second.selection.name, Qt::CaseInsensitive);
        return nameOrder != 0 ? nameOrder < 0 : first.selection.id < second.selection.id;
    });
    return Groups::success(std::move(groups));
}

Result<QList<Group>> discoverGroups()
{
    using Groups = Result<QList<Group>>;
    const Udev udevContext(udev_new(), udev_unref);
    if (!udevContext) {
        return Groups::failure(ErrorCode::Io,
                               QStringLiteral("Cannot initialize input metadata discovery."));
    }
    const Enumeration enumeration(udev_enumerate_new(udevContext.get()), udev_enumerate_unref);
    if (!enumeration || udev_enumerate_add_match_subsystem(enumeration.get(), "input") < 0 ||
        udev_enumerate_scan_devices(enumeration.get()) < 0) {
        return Groups::failure(ErrorCode::Io,
                               QStringLiteral("Cannot enumerate Linux input metadata."));
    }

    qsizetype count = 0;
    QList<Interface> interfaces;
    for (auto *entry = udev_enumerate_get_list_entry(enumeration.get()); entry;
         entry = udev_list_entry_get_next(entry)) {
        const char *syspath = udev_list_entry_get_name(entry);
        const QString path = text(syspath);
        if (!eventName(QStringView(path).sliced(path.lastIndexOf(u'/') + 1))) {
            continue;
        }
        if (++count > maxNodes) {
            return Groups::failure(
                ErrorCode::Invalid,
                QStringLiteral("Too many input interfaces to identify a keyboard safely."));
        }
        const Device device(udev_device_new_from_syspath(udevContext.get(), syspath),
                            udev_device_unref);
        if (!device) {
            return Groups::failure(
                ErrorCode::Io,
                QStringLiteral("An input device changed during discovery. Try again."));
        }
        auto *usb =
            udev_device_get_parent_with_subsystem_devtype(device.get(), "usb", "usb_device");
        if (usb) {
            interfaces.append(inspect(device.get(), usb));
        }
    }
    return groupInterfaces(interfaces);
}

} // namespace xkeys::keyboard

namespace xkeys
{
Result<QList<KeyboardSelection>> discoverKeyboards()
{
    using Keyboards = Result<QList<KeyboardSelection>>;
    const auto groups = keyboard::discoverGroups();
    if (!groups) {
        return Keyboards::failure(groups.error().code, groups.error().message);
    }

    QList<KeyboardSelection> selections;
    for (const auto &group : groups.value()) {
        selections.append(group.selection);
    }
    return Keyboards::success(std::move(selections));
}
} // namespace xkeys
