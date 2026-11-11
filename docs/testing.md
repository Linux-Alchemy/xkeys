# Recording checkpoint

The current editor supports real keyboard recording on both layers and simulated pad
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

Choosing a profile immediately shows its saved bindings as a read-only preview.
Use **Layer 1** and **Layer 2** to inspect both sets without entering Edit, starting
recording or activating output. The keypad cannot simulate presses in preview.
If another profile is already active, it keeps running and remains named in the
active-profile banner; the preview identifies the selected profile separately.

Choose **Edit** to change bindings, then choose the intended recording keyboard.
A uniquely identified Q1 Max wired/Keychron Link receiver pair appears as one logical keyboard;
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

## Layer-two assignment and simulation

1. In **Edit**, select **Layer 1**, choose button **03**, and record/save
   **Ctrl+Alt+F**. Select **Layer 2**: its saved binding and map labels must reflect
   that layer, with no capture or countdown starting automatically.
2. On the same button, explicitly start recording and save **Ctrl+Alt+E**.
   Switch between the layers: each must retain its own shortcut, and output stays off.
3. On Layer 2, record another draft and **Cancel draft**. Repeat and switch to
   Layer 1 before saving: the draft must be discarded, with both saved bindings
   intact. Layer switching is disabled while recording, stopping or saving.
4. Return to Layer 2 and **Clear binding**. Layer 2 must show Blank while Layer 1
   retains Ctrl+Alt+F. Record/save Ctrl+Alt+E on Layer 2 again. Close the editor
   using the window manager's close shortcut (Super+W on the review desktop), then
   reopen it with the command above. Select the same profile: its saved bindings
   must appear immediately, with both layers available for read-only preview.
   Editing, recording and activation must remain off until explicitly requested.
   The window has no built-in close button.
5. Choose **Edit**, then enable **Hold thumb for Layer 2**. The thumb tile reads Layer 2 and cannot record
   its own shortcut while this setting is enabled. It remains possible to inspect
   and clear its stored shortcut. Editing Layer 2 is possible with this setting
   off, but using Layer 2 during playback requires enabling it.
6. Click **Activate**, then enable **Hold controls** in the simulator. Click the
   on-screen thumb, then button 03: the result must be Ctrl+Alt+E on Layer 2.
   Release the thumb first: the map returns to Layer 1, but held output remains
   Ctrl+Alt+E until button 03 is released. Repeat with button 03 released first;
   both sequences must end with Held output: None. A plain button 03 press uses
   Ctrl+Alt+F. A blank Layer 2 button must resolve to Blank.
7. Repeat the desktop overlap and mouse Cancel/focus-loss checks below on Layer 2.
   Judge whether the edited layer, saved shortcut and unsaved draft are clear.

All pad presses in this checklist are on-screen simulation. Physical Tartarus
selection and emitted keystrokes remain future work. Hands-on acceptance is
recorded separately from the automated checks.

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
