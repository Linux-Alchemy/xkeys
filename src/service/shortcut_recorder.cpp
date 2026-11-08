#include "shortcut_recorder.h"

#include <algorithm>
#include <linux/input-event-codes.h>

namespace xkeys
{
namespace
{
bool isModifier(int code)
{
    switch (code) {
    case KEY_LEFTCTRL:
    case KEY_RIGHTCTRL:
    case KEY_LEFTALT:
    case KEY_RIGHTALT:
    case KEY_LEFTSHIFT:
    case KEY_RIGHTSHIFT:
    case KEY_LEFTMETA:
    case KEY_RIGHTMETA:
        return true;
    default:
        return false;
    }
}
} // namespace

Result<void> Recorder::begin(const KeyboardSelection &keyboard)
{
    const QSet<QString> nodes(keyboard.nodes.begin(), keyboard.nodes.end());
    if (keyboard.id.isEmpty() || keyboard.nodes.isEmpty() || keyboard.nodes.size() > 16 ||
        nodes.size() != keyboard.nodes.size() || nodes.contains(QString{})) {
        return Result<void>::failure(ErrorCode::Invalid,
                                     QStringLiteral("Select a complete keyboard group."));
    }

    m_held = QList<QSet<int>>(keyboard.nodes.size());
    m_releasing = false;
    m_update = {CapturePhase::Recording, {}, {}};
    return Result<void>::success();
}

CaptureUpdate Recorder::consume(const RawEvent &event)
{
    if (m_update.phase != CapturePhase::Recording) {
        return m_update;
    }

    if (event.source < 0 || event.source >= m_held.size()) {
        return reject(QStringLiteral("The keyboard event came from an unknown interface."));
    }
    if (event.code <= KEY_RESERVED || event.code >= BTN_MISC || event.code == KEY_UNKNOWN) {
        return reject(QStringLiteral("This key is not supported for shortcuts."));
    }
    if (event.value < 0 || event.value > 2) {
        return reject(QStringLiteral("The keyboard sent an invalid key event."));
    }

    auto &held = m_held[event.source];
    if (event.value == 2 || (event.value == 1 && held.contains(event.code))) {
        return m_update;
    }

    if (event.value == 1) {
        if (m_releasing) {
            return reject(QStringLiteral("Press one chord together, then release all keys."));
        }
        held.insert(event.code);
        if (!m_update.chord.contains(event.code)) {
            if (m_update.chord.size() == 16) {
                return reject(QStringLiteral("A shortcut can contain at most 16 keys."));
            }
            if (isModifier(event.code)) {
                const auto firstKey = std::ranges::find_if_not(m_update.chord, isModifier);
                m_update.chord.insert(firstKey, event.code);
            } else {
                m_update.chord.append(event.code);
            }
        }
        return m_update;
    }

    if (!held.remove(event.code)) {
        return reject(QStringLiteral("The keyboard reported a release without a press."));
    }
    m_releasing = true;
    if (std::ranges::all_of(m_held, [](const auto &keys) { return keys.isEmpty(); })) {
        m_update.phase = CapturePhase::Complete;
    }
    return m_update;
}

void Recorder::cancel()
{
    m_held.clear();
    m_update = {CapturePhase::Cancelled, {}, {}};
}

CaptureUpdate Recorder::reject(const QString &message)
{
    m_held.clear();
    m_update = {CapturePhase::Rejected, {}, message};
    return m_update;
}

CaptureUpdate Recorder::snapshot() const { return m_update; }

} // namespace xkeys
