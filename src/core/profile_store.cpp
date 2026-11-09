#include "profile_store.h"

#include "remapper.h"

#include <QDir>
#include <QDirListing>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QSaveFile>
#include <QSet>
#include <QUuid>

#include <algorithm>
#include <linux/input-event-codes.h>

namespace xkeys
{

namespace
{
constexpr int profileVersion = 1;
constexpr qsizetype maxTextLength = 80;
constexpr qsizetype maxChordLength = 16;
constexpr qsizetype maxProfiles = 128;
constexpr qint64 maxProfileBytes = 256 * 1024;

bool validId(const ProfileId &id)
{
    return QUuid::fromString(id).toString(QUuid::WithoutBraces) == id;
}

bool validText(const QString &text)
{
    if (!text.isValidUtf16()) {
        return false;
    }
    const auto codepoints = text.toUcs4();
    return codepoints.size() <= maxTextLength &&
           std::ranges::none_of(codepoints, [](char32_t code) {
               return QChar::category(code) == QChar::Other_Control;
           });
}

bool isModifier(int code)
{
    switch (code) {
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
        return true;
    default:
        return false;
    }
}

QString bindingError(const Binding &binding)
{
    if (!validText(binding.label)) {
        return QStringLiteral("The label must contain at most 80 characters and no controls.");
    }
    if (binding.chord.isEmpty() && !binding.label.isEmpty()) {
        return QStringLiteral("A blank binding must have an empty label.");
    }
    if (binding.chord.size() > maxChordLength) {
        return QStringLiteral("A chord can contain at most 16 keycodes.");
    }

    QSet<int> seen;
    bool hasOrdinaryKey = false;
    for (const int code : binding.chord) {
        if (code <= KEY_RESERVED || code >= BTN_MISC || code == KEY_UNKNOWN) {
            return QStringLiteral("Unsupported keyboard keycode %1.").arg(code);
        }
        if (seen.contains(code)) {
            return QStringLiteral("Duplicate keyboard keycode %1.").arg(code);
        }
        seen.insert(code);

        if (isModifier(code)) {
            if (hasOrdinaryKey) {
                return QStringLiteral("Modifier keys must precede ordinary keys.");
            }
        } else {
            hasOrdinaryKey = true;
        }
    }
    return {};
}

Result<bool> inspectRoot(const QString &root)
{
    if (root.isEmpty()) {
        return Result<bool>::failure(ErrorCode::Invalid,
                                     QStringLiteral("Profile directory is empty."));
    }
    const QFileInfo info(root);
    if (info.isSymLink()) {
        return Result<bool>::failure(ErrorCode::Invalid,
                                     QStringLiteral("Profile directory is a symbolic link."));
    }
    if (!info.exists()) {
        return Result<bool>::success(false);
    }
    if (!info.isDir() || !info.isReadable() || !info.isExecutable()) {
        return Result<bool>::failure(ErrorCode::Io,
                                     QStringLiteral("Cannot read profile directory %1.").arg(root));
    }
    return Result<bool>::success(true);
}

Result<void> lockStore(QLockFile &lock)
{
    const QFileInfo info(lock.fileName());
    if (info.isSymLink() || (info.exists() && !info.isFile())) {
        return Result<void>::failure(ErrorCode::Invalid,
                                     QStringLiteral("The profile lock path is unsafe."));
    }
    lock.setStaleLockTime(0);
    if (lock.tryLock()) {
        return Result<void>::success();
    }
    if (lock.error() == QLockFile::LockFailedError) {
        return Result<void>::failure(
            ErrorCode::Conflict,
            QStringLiteral("Another profile write is in progress. Try again."));
    }
    return Result<void>::failure(ErrorCode::Io,
                                 QStringLiteral("Cannot lock the profile directory."));
}
} // namespace

Profile makeBlankProfile(const ProfileId &id, const QString &name)
{
    Profile profile;
    profile.id = id;
    profile.name = name;
    for (auto &layer : profile.layers) {
        for (const auto &control : demoControls()) {
            layer.insert(control.id, Binding{});
        }
    }
    return profile;
}

Result<void> validateProfile(const Profile &profile)
{
    if (!validId(profile.id)) {
        return Result<void>::failure(ErrorCode::Invalid,
                                     QStringLiteral("Profile ID must be a lowercase UUID."));
    }
    const QString &name = profile.name;
    if (name.isEmpty() || name != name.trimmed() || !validText(name) || name.contains(u'/') ||
        name.contains(u'\\') || name == QStringLiteral(".") || name == QStringLiteral("..")) {
        return Result<void>::failure(
            ErrorCode::Invalid, QStringLiteral("Use a trimmed profile name of 1 to 80 characters "
                                               "without path separators or control characters."));
    }
    if (profile.revision < 0 || profile.revision > maxRevision) {
        return Result<void>::failure(ErrorCode::Invalid,
                                     QStringLiteral("Invalid profile revision."));
    }
    for (std::size_t index = 0; index < profile.layers.size(); ++index) {
        const auto &layer = profile.layers[index];
        if (layer.size() != demoControls().size()) {
            return Result<void>::failure(
                ErrorCode::Invalid,
                QStringLiteral("Layer %1 must contain every known control exactly once.")
                    .arg(index + 1));
        }
        for (const auto &control : demoControls()) {
            const auto binding = layer.constFind(control.id);
            if (binding == layer.cend()) {
                return Result<void>::failure(ErrorCode::Invalid,
                                             QStringLiteral("Layer %1 is missing control %2.")
                                                 .arg(index + 1)
                                                 .arg(control.id));
            }
            const QString error = bindingError(binding.value());
            if (!error.isEmpty()) {
                return Result<void>::failure(ErrorCode::Invalid,
                                             QStringLiteral("Layer %1, control %2. %3")
                                                 .arg(index + 1)
                                                 .arg(control.id, error));
            }
        }
    }
    return Result<void>::success();
}

QJsonObject profileToJson(const Profile &profile)
{
    QJsonArray layers;
    for (const auto &layer : profile.layers) {
        QJsonObject controls;
        for (auto binding = layer.cbegin(); binding != layer.cend(); ++binding) {
            QJsonArray keys;
            for (const int code : binding.value().chord) {
                keys.append(code);
            }
            controls.insert(binding.key(),
                            QJsonObject{{QStringLiteral("label"), binding.value().label},
                                        {QStringLiteral("keys"), keys}});
        }
        layers.append(controls);
    }
    return {{QStringLiteral("version"), profileVersion},
            {QStringLiteral("id"), profile.id},
            {QStringLiteral("name"), profile.name},
            {QStringLiteral("revision"), profile.revision},
            {QStringLiteral("thumbLayer"), profile.thumbLayer},
            {QStringLiteral("layers"), layers}};
}

Result<Profile> profileFromJson(const QJsonObject &object)
{
    if (object.value(QStringLiteral("version")) != profileVersion) {
        return Result<Profile>::failure(ErrorCode::Invalid,
                                        QStringLiteral("Unsupported profile schema version."));
    }
    const auto id = object.value(QStringLiteral("id"));
    const auto name = object.value(QStringLiteral("name"));
    const auto revision = object.value(QStringLiteral("revision"));
    const auto thumbLayer = object.value(QStringLiteral("thumbLayer"));
    const auto layers = object.value(QStringLiteral("layers"));
    if (object.size() != 6 || !id.isString() || !name.isString() || !revision.isDouble() ||
        !thumbLayer.isBool() || !layers.isArray()) {
        return Result<Profile>::failure(ErrorCode::Invalid,
                                        QStringLiteral("Invalid profile fields."));
    }

    Profile profile;
    profile.id = id.toString();
    profile.name = name.toString();
    profile.revision = revision.toInteger(-1);
    profile.thumbLayer = thumbLayer.toBool();
    const QJsonArray layerArray = layers.toArray();
    if (layerArray.size() != static_cast<qsizetype>(profile.layers.size())) {
        return Result<Profile>::failure(
            ErrorCode::Invalid, QStringLiteral("A profile must contain exactly two layers."));
    }

    for (std::size_t index = 0; index < profile.layers.size(); ++index) {
        const auto layer = layerArray.at(static_cast<qsizetype>(index));
        if (!layer.isObject() || layer.toObject().size() != demoControls().size()) {
            return Result<Profile>::failure(
                ErrorCode::Invalid,
                QStringLiteral("Layer %1 must contain every known control exactly once.")
                    .arg(index + 1));
        }
        const auto controls = layer.toObject();
        for (const auto &control : demoControls()) {
            const auto entry = controls.value(control.id).toObject();
            const auto label = entry.value(QStringLiteral("label"));
            const auto keys = entry.value(QStringLiteral("keys"));
            if (entry.size() != 2 || !label.isString() || !keys.isArray() ||
                keys.toArray().size() > maxChordLength) {
                return Result<Profile>::failure(
                    ErrorCode::Invalid, QStringLiteral("Invalid binding in layer %1, control %2.")
                                            .arg(index + 1)
                                            .arg(control.id));
            }
            Binding binding;
            binding.label = label.toString();
            const QJsonArray keyArray = keys.toArray();
            for (const auto code : keyArray) {
                binding.chord.append(code.toInt(-1));
            }
            profile.layers[index].insert(control.id, binding);
        }
    }

    const auto validation = validateProfile(profile);
    if (!validation) {
        return Result<Profile>::failure(validation.error().code, validation.error().message);
    }
    return Result<Profile>::success(std::move(profile));
}

ProfileStore::ProfileStore(QString root)
    : m_root(root.isEmpty() ? QString{} : QDir(root).absolutePath())
{
}

QString ProfileStore::profilePath(const ProfileId &id) const
{
    return QDir(m_root).filePath(id + QStringLiteral(".json"));
}

Result<Profile> ProfileStore::load(const ProfileId &id) const
{
    if (!validId(id)) {
        return Result<Profile>::failure(ErrorCode::Invalid,
                                        QStringLiteral("Profile ID must be a lowercase UUID."));
    }
    const auto root = inspectRoot(m_root);
    if (!root) {
        return Result<Profile>::failure(root.error().code, root.error().message);
    }

    const QFileInfo info(profilePath(id));
    if (info.isSymLink() || (info.exists() && !info.isFile())) {
        return Result<Profile>::failure(
            ErrorCode::Invalid,
            QStringLiteral("Profile file %1 must be a regular file.").arg(info.fileName()));
    }
    if (!info.exists()) {
        return Result<Profile>::failure(ErrorCode::NotFound,
                                        QStringLiteral("Profile %1 was not found.").arg(id));
    }

    QFile file(info.filePath());
    if (!file.open(QIODevice::ReadOnly)) {
        return Result<Profile>::failure(
            ErrorCode::Io,
            QStringLiteral("Cannot open profile %1. %2").arg(info.fileName(), file.errorString()));
    }
    const QByteArray bytes = file.read(maxProfileBytes + 1);
    if (file.error() != QFileDevice::NoError) {
        return Result<Profile>::failure(
            ErrorCode::Io,
            QStringLiteral("Cannot read profile %1. %2").arg(info.fileName(), file.errorString()));
    }
    if (bytes.size() > maxProfileBytes) {
        return Result<Profile>::failure(
            ErrorCode::Invalid,
            QStringLiteral("Profile file %1 exceeds 256 KiB.").arg(info.fileName()));
    }

    QJsonParseError error{};
    const QJsonDocument document = QJsonDocument::fromJson(bytes, &error);
    if (error.error != QJsonParseError::NoError) {
        return Result<Profile>::failure(ErrorCode::Invalid,
                                        QStringLiteral("Invalid JSON in profile file %1. %2")
                                            .arg(info.fileName(), error.errorString()));
    }
    if (!document.isObject()) {
        return Result<Profile>::failure(
            ErrorCode::Invalid,
            QStringLiteral("Profile file %1 must contain a JSON object.").arg(info.fileName()));
    }

    auto profile = profileFromJson(document.object());
    if (!profile) {
        return Result<Profile>::failure(
            profile.error().code,
            QStringLiteral("Profile file %1. %2").arg(info.fileName(), profile.error().message));
    }
    if (profile.value().id != id || profile.value().revision == 0) {
        return Result<Profile>::failure(
            ErrorCode::Invalid,
            QStringLiteral("Profile file %1 must match its ID and have a saved revision.")
                .arg(info.fileName()));
    }
    return profile;
}

Result<QList<Profile>> ProfileStore::list() const
{
    using Profiles = Result<QList<Profile>>;
    const auto root = inspectRoot(m_root);
    if (!root) {
        return Profiles::failure(root.error().code, root.error().message);
    }
    if (!root.value()) {
        return Profiles::success({});
    }

    QList<Profile> profiles;
    using Flag = QDirListing::IteratorFlag;
    for (const auto &entry :
         QDirListing(m_root, {QStringLiteral("*.json")},
                     Flag::CaseSensitive | Flag::IncludeHidden | Flag::IncludeBrokenSymlinks)) {
        if (profiles.size() == maxProfiles) {
            return Profiles::failure(ErrorCode::Invalid,
                                     QStringLiteral("The profile directory exceeds 128 profiles."));
        }
        const auto filename = entry.fileName();
        const auto id = filename.chopped(5);
        if (!validId(id)) {
            return Profiles::failure(
                ErrorCode::Invalid,
                QStringLiteral("Profile file %1 must use a lowercase UUID filename.")
                    .arg(filename));
        }

        auto profile = load(id);
        if (!profile) {
            return Profiles::failure(profile.error().code, profile.error().message);
        }
        profiles.append(std::move(profile.value()));
    }
    std::ranges::sort(profiles, {}, &Profile::id);
    return Profiles::success(std::move(profiles));
}

Result<Revision> ProfileStore::save(const Profile &profile, Revision expected)
{
    const auto validation = validateProfile(profile);
    if (!validation) {
        return Result<Revision>::failure(validation.error().code, validation.error().message);
    }
    if (profile.revision != expected || expected == maxRevision) {
        return Result<Revision>::failure(
            ErrorCode::Conflict, QStringLiteral("The profile revision cannot be advanced."));
    }

    const auto root = inspectRoot(m_root);
    if (!root) {
        return Result<Revision>::failure(root.error().code, root.error().message);
    }
    if (!root.value()) {
        if (expected != 0) {
            return Result<Revision>::failure(ErrorCode::Conflict,
                                             QStringLiteral("The saved profile no longer exists."));
        }
        if (!QDir().mkpath(m_root)) {
            return Result<Revision>::failure(
                ErrorCode::Io, QStringLiteral("Cannot create the profile directory."));
        }
    }

    QLockFile lock(QDir(m_root).filePath(QStringLiteral(".profiles.lock")));
    const auto locked = lockStore(lock);
    if (!locked) {
        return Result<Revision>::failure(locked.error().code, locked.error().message);
    }

    const auto profiles = list();
    if (!profiles) {
        return Result<Revision>::failure(profiles.error().code, profiles.error().message);
    }
    const auto current = std::ranges::find(profiles.value(), profile.id, &Profile::id);
    if ((current == profiles.value().cend() && expected != 0) ||
        (current != profiles.value().cend() && current->revision != expected)) {
        return Result<Revision>::failure(
            ErrorCode::Conflict,
            QStringLiteral("The saved profile changed. Reload it and try again."));
    }
    if (expected == 0 && profiles.value().size() == maxProfiles) {
        return Result<Revision>::failure(
            ErrorCode::Conflict,
            QStringLiteral("The profile directory already contains 128 profiles."));
    }
    for (const auto &other : profiles.value()) {
        if (other.id != profile.id && other.name.compare(profile.name, Qt::CaseInsensitive) == 0) {
            return Result<Revision>::failure(
                ErrorCode::Conflict, QStringLiteral("A profile with that name already exists."));
        }
    }

    Profile updated = profile;
    updated.revision = expected + 1;
    const QByteArray bytes = QJsonDocument(profileToJson(updated)).toJson(QJsonDocument::Compact);
    QSaveFile file(profilePath(profile.id));
    file.setDirectWriteFallback(false);
    if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() || !file.commit()) {
        return Result<Revision>::failure(
            ErrorCode::Io,
            QStringLiteral("Cannot save profile %1. %2").arg(profile.id, file.errorString()));
    }
    return Result<Revision>::success(updated.revision);
}

Result<void> ProfileStore::remove(const ProfileId &id, Revision expected)
{
    if (!validId(id) || expected <= 0 || expected > maxRevision) {
        return Result<void>::failure(
            ErrorCode::Invalid, QStringLiteral("Removal requires a valid ID and saved revision."));
    }
    const auto root = inspectRoot(m_root);
    if (!root) {
        return Result<void>::failure(root.error().code, root.error().message);
    }
    if (!root.value()) {
        return Result<void>::failure(ErrorCode::NotFound,
                                     QStringLiteral("The profile was not found."));
    }

    QLockFile lock(QDir(m_root).filePath(QStringLiteral(".profiles.lock")));
    const auto locked = lockStore(lock);
    if (!locked) {
        return locked;
    }
    const auto current = load(id);
    if (!current) {
        return Result<void>::failure(current.error().code, current.error().message);
    }
    if (current.value().revision != expected) {
        return Result<void>::failure(
            ErrorCode::Conflict,
            QStringLiteral("The saved profile changed. Reload it and try again."));
    }

    QFile file(profilePath(id));
    if (!file.remove()) {
        return Result<void>::failure(
            ErrorCode::Io,
            QStringLiteral("Cannot remove profile %1. %2").arg(id, file.errorString()));
    }
    return Result<void>::success();
}

} // namespace xkeys
