#include "hyprland_context.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPointer>
#include <QProcess>
#include <QSet>
#include <QTimer>

#include <algorithm>
#include <array>
#include <linux/input-event-codes.h>
#include <xkbcommon/xkbcommon.h>

namespace xkeys
{

namespace
{
constexpr int queryTimeoutMs = 3000;
constexpr qsizetype maxQueryBytes = 1024 * 1024;
constexpr int ctrlMask = 4;
constexpr int altMask = 8;

struct Modifier {
    int code;
    xkb_keysym_t symbol;
    const char *label;
};

constexpr std::array modifiers{
    Modifier{KEY_LEFTSHIFT, XKB_KEY_Shift_L, "Shift"},
    Modifier{KEY_RIGHTSHIFT, XKB_KEY_Shift_R, "Right Shift"},
    Modifier{KEY_LEFTCTRL, XKB_KEY_Control_L, "Ctrl"},
    Modifier{KEY_RIGHTCTRL, XKB_KEY_Control_R, "Right Ctrl"},
    Modifier{KEY_LEFTALT, XKB_KEY_Alt_L, "Alt"},
    Modifier{KEY_RIGHTALT, XKB_KEY_Alt_R, "Right Alt"},
    Modifier{KEY_LEFTMETA, XKB_KEY_Super_L, "Super"},
    Modifier{KEY_RIGHTMETA, XKB_KEY_Super_R, "Right Super"},
};

struct LayoutNames {
    QByteArray rules;
    QByteArray model;
    QByteArray layout;
    QByteArray variant;
    QByteArray options;
    int index = -1;

    bool operator==(const LayoutNames &) const = default;
};

struct DesktopBinding {
    QString key;
    int keycode = 0;
    int mask = 0;
    bool release = false;
    bool catchAll = false;
    QString description;

    bool unresolved() const { return key.isEmpty() && keycode == 0 && !catchAll; }
};

QString normalizedName(QString name) { return name.trimmed().toLower().replace(u' ', u'-'); }

bool sameOrNumberedName(const QString &name, const QString &wanted)
{
    if (name == wanted) {
        return true;
    }
    if (!name.startsWith(wanted + u'-')) {
        return false;
    }
    bool numeric = false;
    name.sliced(wanted.size() + 1).toUInt(&numeric);
    return numeric;
}

std::optional<LayoutNames> readLayout(const QJsonObject &object)
{
    LayoutNames names;
    const std::array fields{
        std::pair{QStringLiteral("rules"), &LayoutNames::rules},
        std::pair{QStringLiteral("model"), &LayoutNames::model},
        std::pair{QStringLiteral("layout"), &LayoutNames::layout},
        std::pair{QStringLiteral("variant"), &LayoutNames::variant},
        std::pair{QStringLiteral("options"), &LayoutNames::options},
    };
    for (const auto &[key, member] : fields) {
        const auto item = object.value(key);
        const auto name = item.toString();
        if (!item.isString() || name.size() > 512 || name.contains(QChar::Null)) {
            return std::nullopt;
        }
        names.*member = name.toUtf8();
    }
    names.index = object.value(QStringLiteral("active_layout_index")).toInt(-1);
    if (names.index < 0 || names.layout.isEmpty()) {
        return std::nullopt;
    }
    return names;
}

Result<LayoutNames> findLayout(const QJsonDocument &document, const KeyboardSelection &keyboard)
{
    using LayoutResult = Result<LayoutNames>;
    const auto devices = document.object().value(QStringLiteral("keyboards"));
    if (!devices.isArray() || keyboard.typingNames.isEmpty()) {
        return LayoutResult::failure(
            ErrorCode::Invalid, QStringLiteral("Cannot identify the keyboard layout in Hyprland."));
    }

    const auto keyboards = devices.toArray();
    std::optional<LayoutNames> layout;
    QSet<QString> typingNames;
    for (const auto &typingName : keyboard.typingNames) {
        const auto wanted = normalizedName(typingName);
        if (typingNames.contains(wanted)) {
            return LayoutResult::failure(
                ErrorCode::Conflict,
                QStringLiteral("The selected keyboard has indistinguishable typing interfaces."));
        }
        typingNames.insert(wanted);
        std::optional<LayoutNames> found;
        for (const auto item : keyboards) {
            const auto record = item.toObject();
            const auto name = normalizedName(record.value(QStringLiteral("name")).toString());
            if (wanted.isEmpty() || !sameOrNumberedName(name, wanted)) {
                continue;
            }
            // A numbered duplicate cannot be matched to a physical USB group using names.
            if (name != wanted || found) {
                return LayoutResult::failure(
                    ErrorCode::Conflict,
                    QStringLiteral("The selected keyboard name is ambiguous in Hyprland."));
            }
            found = readLayout(record);
            if (!found) {
                return LayoutResult::failure(
                    ErrorCode::Invalid,
                    QStringLiteral("Hyprland returned incomplete keyboard layout information."));
            }
        }
        if (!found || (layout && *layout != *found)) {
            return LayoutResult::failure(
                ErrorCode::Invalid,
                QStringLiteral(
                    "Hyprland could not confirm one layout for every typing interface."));
        }
        layout = found;
    }
    return LayoutResult::success(*layout);
}

std::optional<LayoutNames> readGlobalLayout(const QByteArray &bytes, bool &customKeymap)
{
    QJsonObject values{{QStringLiteral("active_layout_index"), 0}};
    const auto lines = bytes.split('\n');
    for (const auto &line : lines) {
        if (line.trimmed().isEmpty()) {
            continue;
        }
        const auto record = QJsonDocument::fromJson(line).object();
        const auto option = record.value(QStringLiteral("option")).toString();
        const auto str = record.value(QStringLiteral("str"));
        const auto set = record.value(QStringLiteral("set"));
        if (!option.startsWith(QStringLiteral("input:kb_")) || !str.isString() || !set.isBool()) {
            return std::nullopt;
        }
        const auto key = option.sliced(9);
        if (values.contains(key)) {
            return std::nullopt;
        }
        const auto value =
            !set.toBool() && str == QStringLiteral("[[EMPTY]]") ? QString{} : str.toString();
        values.insert(key, value);
    }
    const auto file = values.value(QStringLiteral("file"));
    if (values.size() != 7 || !file.isString()) {
        return std::nullopt;
    }
    customKeymap = !file.toString().isEmpty();
    return customKeymap ? std::nullopt : readLayout(values);
}

std::optional<QList<DesktopBinding>> readBindings(const QJsonDocument &document)
{
    if (!document.isArray()) {
        return std::nullopt;
    }

    QList<DesktopBinding> bindings;
    const auto entries = document.array();
    for (const auto item : entries) {
        const auto record = item.toObject();
        const auto key = record.value(QStringLiteral("key"));
        const int keycode = record.value(QStringLiteral("keycode")).toInt(-1);
        const int mask = record.value(QStringLiteral("modmask")).toInt(-1);
        const auto release = record.value(QStringLiteral("release"));
        const auto mouse = record.value(QStringLiteral("mouse"));
        const auto catchAll = record.value(QStringLiteral("catch_all"));
        const auto submap = record.value(QStringLiteral("submap"));
        const auto description = record.value(QStringLiteral("description"));
        const auto dispatcher = record.value(QStringLiteral("dispatcher"));
        if (!key.isString() || keycode < 0 || keycode > 65535 || mask < 0 || mask > 255 ||
            !release.isBool() || !mouse.isBool() || !catchAll.isBool() || !submap.isString() ||
            !description.isString() || !dispatcher.isString()) {
            return std::nullopt;
        }
        if (mouse.toBool()) {
            continue;
        }

        auto title = description.toString().simplified().left(160);
        if (title.isEmpty()) {
            title = dispatcher.toString().simplified().left(80);
        }
        if (title.isEmpty()) {
            title = QStringLiteral("Hyprland shortcut");
        }
        if (!submap.toString().isEmpty()) {
            title += QStringLiteral(" (submap %1)").arg(submap.toString().simplified().left(80));
        }
        if (release.toBool()) {
            title += QStringLiteral(" [on release]");
        }
        bindings.append(
            {key.toString(), keycode, mask, release.toBool(), catchAll.toBool(), title});
    }
    return bindings;
}

class HyprlandContext final : public ShortcutContext
{
  public:
    explicit HyprlandContext(QString program) : m_program(std::move(program)) {}

    ~HyprlandContext() override
    {
        const auto processes = findChildren<QProcess *>();
        for (auto *process : processes) {
            process->disconnect(this);
            if (process->state() != QProcess::NotRunning) {
                process->kill();
                process->waitForFinished(250);
            }
        }
    }

    void refresh(const KeyboardSelection &keyboard) override
    {
        ++m_generation;
        for (const auto &query : m_queries) {
            if (query) {
                query->kill();
            }
        }
        m_keyboard = keyboard;
        m_layout.reset();
        m_bindings.reset();
        m_translationComparable = false;
        m_results = {};
        m_pending = 3;
        query(0, {QStringLiteral("-j"), QStringLiteral("devices")});
        query(1, {QStringLiteral("-j"), QStringLiteral("binds")});
        query(2, {QStringLiteral("--batch"),
                  QStringLiteral("j/getoption input:kb_layout;j/getoption input:kb_rules;"
                                 "j/getoption input:kb_model;j/getoption input:kb_variant;"
                                 "j/getoption input:kb_options;j/getoption input:kb_file")});
    }

    QString label(const QList<int> &codes) const override
    {
        if (!m_layout || codes.isEmpty()) {
            return {};
        }
        QStringList names;
        for (const int code : codes) {
            auto name = keyLabel(code);
            if (name.isEmpty()) {
                return {};
            }
            names.append(std::move(name));
        }
        return names.join(u'+');
    }

    QStringList conflicts(const QList<int> &codes) const override
    {
        if (!m_layout) {
            return {};
        }

        const std::unique_ptr<xkb_state, decltype(&xkb_state_unref)> state(
            xkb_state_new(m_layout.get()), xkb_state_unref);
        QStringList overlaps;
        if (!state) {
            return overlaps;
        }
        xkb_state_update_mask(state.get(), 0, 0, 0, 0, 0, m_group);
        const auto check = [&](int code, xkb_key_direction direction) {
            const auto keycode = static_cast<xkb_keycode_t>(code + 8);
            const auto before = maskFor(state.get());
            const auto beforeSymbol = xkb_state_key_get_one_sym(state.get(), keycode);
            xkb_state_update_key(state.get(), keycode, direction);
            const auto after = maskFor(state.get());
            const auto afterSymbol = xkb_state_key_get_one_sym(state.get(), keycode);
            appendConflicts(overlaps, code, before, after, beforeSymbol, afterSymbol,
                            direction == XKB_KEY_UP);
            if (direction == XKB_KEY_DOWN &&
                ((before | after) & (ctrlMask | altMask)) == (ctrlMask | altMask) &&
                ((code >= KEY_F1 && code <= KEY_F10) || code == KEY_F11 || code == KEY_F12)) {
                overlaps.append(QStringLiteral("Linux virtual-terminal shortcut (Ctrl+Alt+F-key)"));
            }
        };
        for (const int code : codes) {
            check(code, XKB_KEY_DOWN);
        }
        for (auto it = codes.crbegin(); it != codes.crend(); ++it) {
            check(*it, XKB_KEY_UP);
        }
        overlaps.removeDuplicates();
        return overlaps;
    }

    bool bindingsAvailable() const override
    {
        return m_bindings.has_value() && m_translationComparable;
    }

    QString advisory() const override
    {
        auto coverage = QStringLiteral("Application and other global shortcuts may be missing.");
        if (!m_bindings) {
            return QStringLiteral("Could not check Hyprland shortcuts. ") + coverage;
        }
        if (std::ranges::any_of(*m_bindings, &DesktopBinding::unresolved)) {
            coverage.prepend(QStringLiteral("Hyprland omits some shortcut triggers. "));
        }
        if (!m_translationComparable) {
            coverage.prepend(
                QStringLiteral("Hyprland may translate shortcuts using a different layout. "));
        }
        return coverage;
    }

  private:
    using Keymap = std::unique_ptr<xkb_keymap, decltype(&xkb_keymap_unref)>;

    void query(std::size_t index, const QStringList &arguments)
    {
        auto *process = new QProcess(this);
        m_queries.at(index) = process;
        auto *deadline = new QTimer(process);
        deadline->setSingleShot(true);
        deadline->setTimerType(Qt::PreciseTimer);
        process->setStandardErrorFile(QProcess::nullDevice());
        process->setReadChannel(QProcess::StandardOutput);
        const auto data = std::make_shared<QByteArray>();
        const auto collect = [process, data] {
            if (data->size() <= maxQueryBytes) {
                *data += process->read(maxQueryBytes + 1 - data->size());
                if (data->size() > maxQueryBytes) {
                    process->kill();
                }
            }
        };
        const auto generation = m_generation;
        const auto done = [this, process, index, generation](const QByteArray &response) {
            process->deleteLater();
            if (generation != m_generation) {
                return;
            }
            m_queries.at(index).clear();
            m_results.at(index) = response;
            if (--m_pending == 0) {
                prepare();
            }
        };
        connect(process, &QProcess::readyReadStandardOutput, this, collect);
        connect(process, &QProcess::finished, this,
                [collect, data, done](int code, QProcess::ExitStatus status) {
                    collect();
                    done(status == QProcess::NormalExit && code == 0 &&
                                 data->size() <= maxQueryBytes
                             ? *data
                             : QByteArray{});
                });
        connect(process, &QProcess::errorOccurred, this, [done](QProcess::ProcessError error) {
            if (error == QProcess::FailedToStart) {
                done({});
            }
        });
        connect(deadline, &QTimer::timeout, process, &QProcess::kill);
        deadline->start(queryTimeoutMs);
        process->start(m_program, arguments);
    }

    void prepare()
    {
        const auto layout = findLayout(QJsonDocument::fromJson(m_results[0]), m_keyboard);
        if (!layout) {
            emit prepared(false, layout.error().message);
            return;
        }
        bool customKeymap = false;
        const auto global = readGlobalLayout(m_results[2], customKeymap);
        if (customKeymap) {
            emit prepared(
                false, QStringLiteral(
                           "Recording cannot identify key labels for a custom Hyprland keymap."));
            return;
        }

        const auto &names = layout.value();
        const auto asName = [](const QByteArray &name) {
            return name.isEmpty() ? nullptr : name.constData();
        };
        const xkb_rule_names rules{asName(names.rules), asName(names.model), asName(names.layout),
                                   asName(names.variant), asName(names.options)};
        const std::unique_ptr<xkb_context, decltype(&xkb_context_unref)> context(
            xkb_context_new(XKB_CONTEXT_NO_ENVIRONMENT_NAMES), xkb_context_unref);
        if (context) {
            m_layout.reset(
                xkb_keymap_new_from_names(context.get(), &rules, XKB_KEYMAP_COMPILE_NO_FLAGS));
        }
        m_group = static_cast<xkb_layout_index_t>(names.index);
        if (!m_layout || m_group >= xkb_keymap_num_layouts(m_layout.get())) {
            m_layout.reset();
            emit prepared(false,
                          QStringLiteral("The selected Hyprland keyboard layout is invalid."));
            return;
        }

        for (const auto &modifier : modifiers) {
            const auto symbol = symbolFor(modifier.code);
            if (symbol != modifier.symbol &&
                !(modifier.code == KEY_RIGHTALT && symbol == XKB_KEY_ISO_Level3_Shift)) {
                m_layout.reset();
                emit prepared(
                    false, QStringLiteral(
                               "The selected layout remaps modifier keys in an unsupported way."));
                return;
            }
        }
        m_bindings = readBindings(QJsonDocument::fromJson(m_results[1]));
        m_translationComparable = global && *global == names;
        m_results = {};
        emit prepared(true, {});
    }

    xkb_keysym_t symbolFor(int code) const
    {
        if (code <= KEY_RESERVED || code >= BTN_MISC || code == KEY_UNKNOWN) {
            return XKB_KEY_NoSymbol;
        }
        const auto keycode = static_cast<xkb_keycode_t>(code + 8);
        const xkb_keysym_t *symbols = nullptr;
        const int count =
            xkb_keymap_key_get_syms_by_level(m_layout.get(), keycode, m_group, 0, &symbols);
        return count == 1 ? symbols[0] : XKB_KEY_NoSymbol;
    }

    QString keyLabel(int code) const
    {
        const auto modifier = std::ranges::find(modifiers, code, &Modifier::code);
        const auto symbol = symbolFor(code);
        if (modifier != modifiers.end()) {
            return symbol == XKB_KEY_ISO_Level3_Shift ? QStringLiteral("Right Alt (AltGr)")
                                                      : QString::fromLatin1(modifier->label);
        }
        if (symbol == XKB_KEY_NoSymbol) {
            return {};
        }
        if (symbol == XKB_KEY_space) {
            return QStringLiteral("Space");
        }
        if (symbol == XKB_KEY_Escape) {
            return QStringLiteral("Esc");
        }
        std::array<char, 64> text{};
        if (xkb_keysym_to_utf8(xkb_keysym_to_upper(symbol), text.data(), text.size()) > 0) {
            const auto character = QString::fromUtf8(text.data());
            if (!character.isEmpty() && character.front().isPrint()) {
                return character;
            }
        }
        return xkb_keysym_get_name(symbol, text.data(), text.size()) > 0
                   ? QString::fromLatin1(text.data())
                   : QString{};
    }

    static int maskFor(xkb_state *state)
    {
        constexpr std::array names{"Shift", "Lock", "Control", "Mod1",
                                   "Mod2",  "Mod3", "Mod4",    "Mod5"};
        int mask = 0;
        for (std::size_t index = 0; index < names.size(); ++index) {
            if (xkb_state_mod_name_is_active(state, names[index], XKB_STATE_MODS_EFFECTIVE) > 0) {
                mask |= 1 << index;
            }
        }
        return mask;
    }

    void appendConflicts(QStringList &overlaps, int code, int before, int after,
                         xkb_keysym_t beforeSymbol, xkb_keysym_t afterSymbol, bool release) const
    {
        if (!m_bindings) {
            return;
        }

        const auto base = xkb_keysym_to_upper(symbolFor(code));
        const auto first = xkb_keysym_to_upper(beforeSymbol);
        const auto last = xkb_keysym_to_upper(afterSymbol);
        for (const auto &binding : *m_bindings) {
            if (binding.release != release || (binding.mask != before && binding.mask != after)) {
                continue;
            }
            if (binding.unresolved()) {
                overlaps.append(QStringLiteral(
                    "Hyprland reports shortcuts with these modifiers but omits their triggers."));
                continue;
            }
            const auto key = binding.key.toUtf8();
            const auto bound = xkb_keysym_to_upper(
                xkb_keysym_from_name(key.constData(), XKB_KEYSYM_CASE_INSENSITIVE));
            const bool symbolMatch =
                bound != XKB_KEY_NoSymbol && (bound == base || bound == first || bound == last);
            if (binding.catchAll || binding.keycode == code + 8 || symbolMatch) {
                overlaps.append(binding.description);
            }
        }
    }

    QString m_program;
    KeyboardSelection m_keyboard;
    Keymap m_layout{nullptr, xkb_keymap_unref};
    xkb_layout_index_t m_group = 0;
    std::optional<QList<DesktopBinding>> m_bindings;
    std::array<QByteArray, 3> m_results;
    std::array<QPointer<QProcess>, 3> m_queries;
    bool m_translationComparable = false;
    quint64 m_generation = 0;
    int m_pending = 0;
};

} // namespace

std::unique_ptr<ShortcutContext> makeShortcutContext()
{
    return makeShortcutContext(QStringLiteral("hyprctl"));
}

std::unique_ptr<ShortcutContext> makeShortcutContext(const QString &hyprctlPath)
{
    return std::make_unique<HyprlandContext>(hyprctlPath);
}

} // namespace xkeys
