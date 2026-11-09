# ADR 0005 — Record a chord into a visible draft

- **Status:** Accepted for implementation; layer-one hands-on acceptance pending
- **Date:** 2026-11-08

## Context

A sample-shortcut dropdown exercised the simulator but did not deliver the intended
assignment workflow. The user requested selecting a button, pressing the actual
shortcut, seeing it clearly, and saving or cancelling. Keyboard recording therefore
moves ahead of further layer-two review, with Omarchy shortcut protection included.

## Decision

Use the access boundary in ADR 0004. A selected layer-one button identifies the
profile, control and revision being edited. Show Preparing until all keyboard
interfaces are acquired and neutral; only then invite the user to press a shortcut.
Display the live chord and finish when every captured key is released. The helper
releases its descriptors before completion, and the editor verifies clean process
completion before enabling Save.

Collect one chord, preserve left/right modifiers, ignore kernel repeats and
reject sequences, unsupported input, sync loss and ambiguous state. Store physical
Linux keycodes. The installed xkbcommon library supplies labels using the chosen
keyboard's current Hyprland keymap and active layout. A keymap mismatch or unknown
layout is an error, not an invented label.

Save uses the existing atomic, revision-checked profile service and never activates
a profile. Cancel, focus loss, service loss, helper failure or timeout preserves
the previous saved assignment. Saved shortcuts appear on button tiles separately
from the unsaved draft. Layer-two data remains intact while its recording workflow
awaits the layer-one checkpoint.

The helper uses a monotonic capture deadline, parent heartbeat and bounded neutral
key drain; the GUI has a separate hard process watchdog. No key events are replayed
or injected. Mouse Cancel remains available and Esc is recordable. Normal completion
ends neutral. Forced termination releases kernel grabs, but its end-to-end handling
must be checked on the actual compositor/libinput stack during supervised testing.

During editing, query current Hyprland bindings for potential playback overlaps.
Known overlaps or an unavailable inventory require explicit acknowledgement before
Save. The user confirmed a warning followed by explicit choices: use the binding
anyway, or record a different shortcut. No desktop bindings are rewritten.
Application/private global shortcuts are not fully enumerable, so the UI must not
claim that an assignment is universally conflict-free.

## Consequences

Recording suppression and later playback routing are different problems. This
slice supplies capture protection and advisory overlap checks, not real keypad
output. The hardware-output phase must separately decide and test Hyprland's
per-device keybind policy for the eventual virtual keyboard, including shared
modifier state across physical and virtual devices.

The development editor enables recording explicitly; ordinary simulation and
headless tests cannot acquire hardware. Tests use fake capture backends and device
streams. Installed libevdev/libudev/xkbcommon are linked without package changes.
The first physical check must include an ordinary chord, a harmless desktop-bound
chord, cancellation, focus loss, a release-only binding and timeout recovery.

## Rejected alternatives

- **Keep selecting sample shortcuts:** it does not exercise the required workflow.
- **Save on key-down:** it conceals partial chords and removes review/cancellation.
- **Capture in the GUI event loop alone:** a frozen editor could retain the input
  boundary without an independent deadline.
- **Assume a known-overlap scan proves safety:** private application hotkeys and
  mixed-device modifiers are not fully represented by the compositor bind list.

## Evidence

The verified libinput 1.32.0 physical-keyboard path drops a release when it did not
observe the corresponding press, and includes a regression test for that case.
Hyprland itself has fallback release handling, so neutral draining and physical
recovery checks remain necessary rather than relying on that filter universally.

- [libinput 1.32.0 keyboard regression](https://chromium.googlesource.com/external/gitlab.freedesktop.org/libinput/libinput/+/refs/tags/upstream/1.32.0/test/test-keyboard.c#98)
- [Hyprland key dispatch and release handling](https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/managers/KeybindManager.cpp)
- [Hyprland per-device settings](https://wiki.hypr.land/configuring/core/devices/)
