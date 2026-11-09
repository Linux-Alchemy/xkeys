# ADR 0004 — Unprivileged, temporary keyboard capture

- **Status:** Accepted for the promoted recording implementation; physical acceptance pending
- **Date:** 2026-11-08

## Context

Recording an assignment must consume the chosen keyboard's input before it can
trigger compositor bindings or applications. A USB keyboard can expose several
keyboard-bearing interfaces, so selecting an event number or one convenient
interface is insufficient. The current Omarchy session already grants the user
read access to its keyboard event devices through ACLs.

Hyprland 0.56.2's shortcut inhibitor is not an exclusive input boundary. Its
activation notification does not establish that every shortcut is disabled;
window policy, bypass bindings and internal handlers also affect processing.
Qt's Wayland window grab is not a substitute for device capture.

## Decision

The editor remains an ordinary desktop-user application. A separate, unprivileged
one-shot helper captures the explicitly selected USB keyboard using the existing
session ACLs and grouped `EVIOCGRAB`. No root service, new udev rules, blanket input
group membership or permission changes are introduced. If access is missing or a
member cannot be acquired, recording stays unavailable with a reason.

Discover groups from udev/sysfs metadata and the physical USB parent. A candidate
must expose a typing keyboard interface; capture includes every keyboard-bearing
interface in that physical group, including consumer/system controls. Pure pointer
interfaces are excluded. Virtual devices and the Tartarus mapping device are not
recording sources. The first implementation supports USB recording keyboards.

The user confirmed that the Keychron Q1 Max wired identity (3434:0810) and
Keychron Link 2.4 GHz receiver (3434:d030) are two routes for the same keyboard.
When uniquely identifiable, present them as one logical recording choice and
acquire all keyboard-bearing interfaces on its currently connected routes. Multiple
matching instances are ambiguous and unavailable, not automatically paired.
The expected fingerprint includes each route's identity and fresh device-node
metadata; a replacement at the same USB address cannot inherit capture approval.
Topology changes invalidate recording and require a fresh selection/arm, with no
hot continuation. Switching transport mid-chord is outside the supported trial.

Metadata listing does not open event descriptors. On explicit capture, rediscover
the group, verify each opened character device's identity, acquire all interfaces
or roll back, and prove neutral input before reporting Ready. Event numbers are
never persisted as device identity. Incomplete, changed, busy or inaccessible
groups are rejected rather than guessed.

Ordinary `--demo` remains hardware-free. `--demo --recording` permits this transient
helper only after the user chooses a keyboard and selects a target in an explicitly
entered Edit session. It does not enable keypad input acquisition or output.

## Consequences

The selected keyboard is unavailable to other consumers for the short recording
interval; the pointer remains usable for Cancel. Other keyboards/devices are
outside that capture boundary. Input-remapper and desktop configuration are not
changed. Reconnection requires fresh identity checks and explicit source selection
if the earlier identity is no longer present.

Helper, GUI and test lifetimes must be checked independently. Automated tests use
synthetic device/clock implementations; physical capture requires the supervised
checkpoint. Access policy for the later persistent Tartarus remapper is still
unsettled and needs its own decision before that hardware phase.

## Rejected alternatives

- **Qt key events or a window-only grab:** compositor shortcuts may already have
  consumed the event before the editor sees it.
- **Wayland shortcut inhibition alone:** it does not promise suppression of every
  compositor shortcut and internal handler.
- **Persistent global permission changes:** unnecessary on the verified session
  and substantially broader than a temporary user-initiated capture.

## Evidence

- [Hyprland 0.56.2 shortcut inhibitor](https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/protocols/ShortcutsInhibit.cpp)
- [Hyprland 0.56.2 key dispatch](https://github.com/hyprwm/Hyprland/blob/v0.56.2/src/managers/KeybindManager.cpp)
- [Linux input event interface](https://docs.kernel.org/input/input.html)

Local read-only inspection verified the compositor version, physical USB groups
and existing per-user ACLs. No physical event descriptors were opened for that inspection.
