# Recording checkpoint

The current editor supports real keyboard recording on layer 1 and simulated pad
output. The physical Tartarus is not acquired or remapped by this build.

## Start the recording-enabled editor

From the checkout:

```sh
build/dev/bin/xkeys --demo --recording
```

The ordinary `--demo` launch keeps keyboard recording disabled. If that editor is
already open, close its window before relaunching with `--recording`; launch-or-focus
focuses an existing window and does not change its startup options. The profile
service can remain running.

Choose a profile and **Edit**. Choose the intended recording keyboard. A uniquely
identified Q1 Max wired/Keychron Link receiver pair appears as one logical keyboard;
all of its currently verified keyboard-bearing routes are acquired together. A
changed connection or fingerprint requires fresh selection. Do not change transport
mid-chord. Other devices remain outside the selected capture boundary.

## Layer-one assignment

1. Click on-screen button **03**. Its saved binding must appear without starting
   recording or a countdown. You can select another button or clear a binding
   without capturing the keyboard. Click **Start recording**, then wait until the
   card invites you to press a shortcut. Preparing is not Ready; keep keys released
   while it prepares.
2. Press **Ctrl+Alt+F**, then release all keys. The card must show that draft clearly,
   separately from the previously saved shortcut.
3. **Save** must update button 03's saved label without activating the profile.
4. Select 03 again, click **Start recording**, record another chord, then
   **Cancel draft**. The prior saved binding must remain. **Clear binding** removes
   the saved assignment explicitly.

No keyboard events should reach another app or trigger a compositor shortcut during
Ready/Recording. A failure before Ready must report an error rather than invite typing.

## Desktop overlap check

After the basic trial succeeds, record an existing harmless desktop-bound chord
whose effect is easy to observe. Confirm that its desktop action does not occur
while recording. The editor should report the known overlap and offer **Use binding
anyway** or **Record different**. Choosing the latter must preserve the old saved
binding until another recording is explicitly saved.

The overlap inventory is advisory and cannot enumerate every application's private
hotkeys. Recording protection and future real keypad playback are separate; output
is still simulated in this build.

## Cancellation and recovery

Click **Start recording** before each recovery check. Check mouse Cancel and focus
loss while recording. Release held keys promptly; the helper may drain for up to
two seconds. Let an idle recording reach its ten-second
capture timeout, then confirm the keyboard works normally and no draft was saved.
The helper also has a fifteen-second total limit and a separate parent watchdog.

The supervised fault trial should additionally exercise a held-key timeout and a
release-only binding, then verify normal keyboard operation and no unexpected action.
That physical fault-path acceptance is not implied by passing fake tests. If recovery
is needed, a terminal can stop this user's recorder helpers without stopping the
profile service:

```sh
pkill -TERM -x xkeys-record
```

An unresponsive helper can be forcibly terminated with `pkill -KILL -x xkeys-record`.
Release physical keys and verify normal input afterward. Do not leave a recording
helper running for unrelated keyboard use.

## Automated checks

Use the commands in [the development baseline](development-baseline.md). Default
CTest uses synthetic event streams, helper fixtures and temporary profiles; it does
not acquire physical input devices. The metadata-only inventory command is:

```sh
build/dev/bin/xkeys-record --list
```

Listing uses udev/sysfs metadata and filesystem access checks, not evdev event reads.
Keep local hardware inventories, profiles, traces and captures out of commits.
