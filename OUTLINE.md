# xkeys

An Omarchy desktop app for binding shortcuts to a Razer Tartarus V2 by pressing the keys you want to use.

## Why build it?

Matt uses the Tartarus for six TopstepX shortcuts and wants to use it for Warcraft again. With input-remapper, he records each pad button but has to type the output combination. When the remapper listened on the wrong Tartarus interface, a button meant for break-even sent plain Space. Matt had to rebuild bindings when he needed to use them.

## What v1 puts in Matt’s hands

- **Bind by position or feel.** Show a simple map of the real pad, including the wheel and thumb controls, in the active Omarchy theme. Click a control or press it on the pad, then play the desired shortcut on the regular keyboard. Editing consumes those presses.
- **Choose a profile.** Create and save named profiles. Select one, then explicitly choose Activate or Edit. Opening the app or saving an edit never activates it. Active mappings keep working when the editor closes or the screen locks. Deactivate returns the pad to factory behaviour; reboot starts with no active profile.
- **Launch or focus.** Use Omarchy’s existing launch-or-focus helper: open the editor when absent, or focus its existing window. Bringing up the editor never changes the active profile.
- **Use two layers.** Both start blank. In the Warcraft profile, holding the small thumb button above the D-pad activates layer 2. Releasing it returns to layer 1. A blank control on either layer emits nothing, including its factory input. Any assignment can be cleared.
- **Test by touch.** Press a control in Test mode. Its position lights up and a small popup beside the map shows the shortcut the running remapper resolved on the active layer. A detected but unassigned press lights up and reads **Blank**. Test mode keeps the keystroke out of the focused app.

## What earns a live trial

The main risk is reliably identifying which of the Tartarus’s interfaces carries each physical control after reconnect. Verify the keys, wheel and thumb controls on Matt’s pad, unplug and reconnect it, then repeat with the same saved profile. If the source is ambiguous, xkeys must report the problem and decline to activate that mapping.

Matt can trial the six TopstepX binds with xkeys handling the Tartarus independently. input-remapper stays available for everyday use. The plan must establish how to hand the pad between them so only one remapper handles it at a time.

## Boundaries and handoff

General device discovery and support for additional peripherals come later. RGB control is outside the foreseeable scope. Profiles switch manually. V1 covers recorded keyboard shortcuts. Full-time adoption is a separate milestone, when xkeys covers Matt’s input-remapper workflow across devices.

**Technology choice.** C++ for the application logic, with Qt 6 and Qt Quick/QML for the editor. Matt chose C++ for an agent-built native application after considering Python’s readability.

**Build mode:** orchestrated. Agents implement small runnable pieces. Matt designs the experience, tests on the physical pad and approves continuation. The plan starts with a bind that survives reconnect and can be independently checked in a neutral app.

**References:** [input-remapper](https://github.com/sezanzeb/input-remapper) for device-specific input and virtual keyboard output, [OpenRazer](https://github.com/openrazer/openrazer) for the hardware driver, and [Razer’s Tartarus V2 diagram](https://mysupport.razer.com/app/answers/detail/a_id/3628) for the control layout.
