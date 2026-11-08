#include "remapper.h"

#include <algorithm>
#include <linux/input-event-codes.h>
#include <utility>

namespace xkeys
{

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
        result.label = QStringLiteral("Ctrl+Alt+E");
        result.chord = {KEY_LEFTCTRL, KEY_LEFTALT, KEY_E};
    } else if (result.control == QStringLiteral("key02")) {
        result.label = QStringLiteral("Shift+F");
        result.chord = {KEY_LEFTSHIFT, KEY_F};
    } else {
        result.blank = true;
        result.label = QStringLiteral("Blank");
    }

    return result;
}

} // namespace xkeys
