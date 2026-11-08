#include "remapper.h"

#include "profile_store.h"

#include <algorithm>
#include <linux/input-event-codes.h>
#include <ranges>
#include <utility>

namespace xkeys
{

namespace
{
QString keyLabel(int code)
{
    static const QMap<int, QString> labels{
        {KEY_LEFTCTRL, QStringLiteral("Ctrl")},
        {KEY_RIGHTCTRL, QStringLiteral("Right Ctrl")},
        {KEY_LEFTALT, QStringLiteral("Alt")},
        {KEY_RIGHTALT, QStringLiteral("Right Alt")},
        {KEY_LEFTSHIFT, QStringLiteral("Shift")},
        {KEY_RIGHTSHIFT, QStringLiteral("Right Shift")},
        {KEY_LEFTMETA, QStringLiteral("Super")},
        {KEY_RIGHTMETA, QStringLiteral("Right Super")},
        {KEY_A, QStringLiteral("A")},
        {KEY_B, QStringLiteral("B")},
        {KEY_C, QStringLiteral("C")},
        {KEY_D, QStringLiteral("D")},
        {KEY_E, QStringLiteral("E")},
        {KEY_F, QStringLiteral("F")},
        {KEY_G, QStringLiteral("G")},
        {KEY_H, QStringLiteral("H")},
        {KEY_I, QStringLiteral("I")},
        {KEY_J, QStringLiteral("J")},
        {KEY_K, QStringLiteral("K")},
        {KEY_L, QStringLiteral("L")},
        {KEY_M, QStringLiteral("M")},
        {KEY_N, QStringLiteral("N")},
        {KEY_O, QStringLiteral("O")},
        {KEY_P, QStringLiteral("P")},
        {KEY_Q, QStringLiteral("Q")},
        {KEY_R, QStringLiteral("R")},
        {KEY_S, QStringLiteral("S")},
        {KEY_T, QStringLiteral("T")},
        {KEY_U, QStringLiteral("U")},
        {KEY_V, QStringLiteral("V")},
        {KEY_W, QStringLiteral("W")},
        {KEY_X, QStringLiteral("X")},
        {KEY_Y, QStringLiteral("Y")},
        {KEY_Z, QStringLiteral("Z")},
        {KEY_0, QStringLiteral("0")},
        {KEY_1, QStringLiteral("1")},
        {KEY_2, QStringLiteral("2")},
        {KEY_3, QStringLiteral("3")},
        {KEY_4, QStringLiteral("4")},
        {KEY_5, QStringLiteral("5")},
        {KEY_6, QStringLiteral("6")},
        {KEY_7, QStringLiteral("7")},
        {KEY_8, QStringLiteral("8")},
        {KEY_9, QStringLiteral("9")},
        {KEY_F1, QStringLiteral("F1")},
        {KEY_F2, QStringLiteral("F2")},
        {KEY_F3, QStringLiteral("F3")},
        {KEY_F4, QStringLiteral("F4")},
        {KEY_F5, QStringLiteral("F5")},
        {KEY_F6, QStringLiteral("F6")},
        {KEY_F7, QStringLiteral("F7")},
        {KEY_F8, QStringLiteral("F8")},
        {KEY_F9, QStringLiteral("F9")},
        {KEY_F10, QStringLiteral("F10")},
        {KEY_F11, QStringLiteral("F11")},
        {KEY_F12, QStringLiteral("F12")},
        {KEY_UP, QStringLiteral("Up")},
        {KEY_DOWN, QStringLiteral("Down")},
        {KEY_LEFT, QStringLiteral("Left")},
        {KEY_RIGHT, QStringLiteral("Right")},
        {KEY_SPACE, QStringLiteral("Space")},
        {KEY_ENTER, QStringLiteral("Enter")},
        {KEY_ESC, QStringLiteral("Esc")},
        {KEY_TAB, QStringLiteral("Tab")},
        {KEY_BACKSPACE, QStringLiteral("Backspace")},
        {KEY_DELETE, QStringLiteral("Delete")},
        {KEY_INSERT, QStringLiteral("Insert")},
        {KEY_HOME, QStringLiteral("Home")},
        {KEY_END, QStringLiteral("End")},
        {KEY_PAGEUP, QStringLiteral("Page Up")},
        {KEY_PAGEDOWN, QStringLiteral("Page Down")},
    };

    const auto label = labels.constFind(code);
    return label == labels.cend() ? QStringLiteral("Key %1").arg(code) : label.value();
}

bool isNotch(const ControlId &control)
{
    return control == QStringLiteral("wheelUp") || control == QStringLiteral("wheelDown");
}
} // namespace

const QList<Control> &demoControls()
{
    static const QList<Control> controls = [] {
        QList<Control> result;
        for (int number = 1; number <= 19; ++number) {
            const QString label = QStringLiteral("%1").arg(number, 2, 10, QLatin1Char('0'));
            result.append({QStringLiteral("key") + label, label});
        }
        result.append({
            {QStringLiteral("wheelUp"), QStringLiteral("Wheel up")},
            {QStringLiteral("wheelDown"), QStringLiteral("Wheel down")},
            {QStringLiteral("wheelClick"), QStringLiteral("Wheel click")},
            {QStringLiteral("thumb"), QStringLiteral("Thumb")},
            {QStringLiteral("dpadUp"), QStringLiteral("D-pad up")},
            {QStringLiteral("dpadRight"), QStringLiteral("D-pad right")},
            {QStringLiteral("dpadDown"), QStringLiteral("D-pad down")},
            {QStringLiteral("dpadLeft"), QStringLiteral("D-pad left")},
            {QStringLiteral("paddle"), QStringLiteral("Paddle")},
        });
        return result;
    }();
    return controls;
}

Resolution resolveDemo(ControlId control)
{
    Resolution result;
    result.control = std::move(control);
    result.valid = std::ranges::any_of(demoControls(), [&result](const Control &candidate) {
        return candidate.id == result.control;
    });

    if (!result.valid) {
        result.error = QStringLiteral("Unknown control.");
    } else if (result.control == QStringLiteral("key01")) {
        result.chord = {KEY_LEFTCTRL, KEY_LEFTALT, KEY_E};
    } else if (result.control == QStringLiteral("key02")) {
        result.chord = {KEY_LEFTSHIFT, KEY_F};
    }
    if (result.valid) {
        result.blank = result.chord.isEmpty();
        result.label = chordLabel(result.chord);
    }

    return result;
}

QString chordLabel(const QList<int> &chord)
{
    if (chord.isEmpty()) {
        return QStringLiteral("Blank");
    }

    QStringList names;
    names.reserve(chord.size());
    for (const int code : chord) {
        names.append(keyLabel(code));
    }
    return names.join(QLatin1Char('+'));
}

Result<void> Remapper::activate(const Profile &profile)
{
    if (!m_held.isEmpty()) {
        return Result<void>::failure(
            ErrorCode::Conflict,
            QStringLiteral("Release all held controls before changing profiles."));
    }
    const auto validation = validateProfile(profile);
    if (!validation) {
        return validation;
    }

    m_profile = profile;
    m_layer = 1;
    return Result<void>::success();
}

bool Remapper::isActive() const { return m_profile.has_value(); }

int Remapper::activeLayer() const { return m_layer; }

QStringList Remapper::heldControls() const
{
    QStringList held;
    held.reserve(m_held.size());
    for (const auto &control : demoControls()) {
        if (m_held.contains(control.id)) {
            held.append(control.id);
        }
    }
    return held;
}

QList<int> Remapper::ownedKeys() const { return m_ownedKeys; }

Resolution Remapper::consume(const ControlEvent &event)
{
    Resolution result;
    result.control = event.control;
    result.layer = m_layer;
    if (!m_profile) {
        result.error = QStringLiteral("No profile is active.");
        return result;
    }

    const auto &layer = m_profile->layers[static_cast<std::size_t>(m_layer - 1)];
    const auto binding = layer.constFind(event.control);
    if (binding == layer.cend()) {
        result.error = QStringLiteral("Unknown control.");
        return result;
    }

    const auto held = m_held.find(event.control);
    if (held != m_held.end()) {
        result = held.value();
        if (event.type == ControlEventType::Release) {
            m_held.erase(held);
            if (result.layerControl) {
                m_layer = 1;
                result.layer = m_layer;
                result.label = QStringLiteral("Layer 1");
            } else {
                releaseChord(result.chord, result.output);
            }
        }
        return result;
    }

    const bool notch = isNotch(event.control);
    if (event.type != ControlEventType::Press && !notch) {
        result.error = QStringLiteral("This control is not held.");
        return result;
    }

    result.valid = true;
    if (m_profile->thumbLayer && event.control == QStringLiteral("thumb")) {
        m_layer = 2;
        result.layer = m_layer;
        result.layerControl = true;
        result.label = QStringLiteral("Layer 2");
        m_held.insert(event.control, result);
        return result;
    }

    result.chord = binding.value().chord;
    result.blank = result.chord.isEmpty();
    result.label = chordLabel(result.chord);
    if (event.type == ControlEventType::Press) {
        if (!notch) {
            // Latch the chosen binding and layer with no transient output events.
            m_held.insert(event.control, result);
        }
        pressChord(result.chord, result.output);
        if (notch) {
            releaseChord(result.chord, result.output);
        }
    }
    return result;
}

void Remapper::pressChord(const QList<int> &chord, OutputBatch &output)
{
    for (const int code : chord) {
        if (m_owners[code]++ == 0) {
            output.append({code, KeyTransition::Down});
            m_ownedKeys.append(code);
        }
    }
}

void Remapper::releaseChord(const QList<int> &chord, OutputBatch &output)
{
    for (const int code : chord | std::views::reverse) {
        const auto owner = m_owners.find(code);
        if (--owner.value() == 0) {
            output.append({code, KeyTransition::Up});
            m_owners.erase(owner);
            m_ownedKeys.removeOne(code);
        }
    }
}

OutputBatch Remapper::reset()
{
    OutputBatch output;
    output.reserve(m_ownedKeys.size());
    for (const int code : std::as_const(m_ownedKeys) | std::views::reverse) {
        output.append({code, KeyTransition::Up});
    }

    m_held.clear();
    m_owners.clear();
    m_ownedKeys.clear();
    m_layer = 1;
    return output;
}

} // namespace xkeys
