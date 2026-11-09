# Development baseline

xkeys uses C++20 and system Qt 6 packages on Omarchy. The development baseline
uses the system dependencies approved for Phase 1 on 2026-11-08. No build step
fetches dependencies.

| Dependency | Requirement | Verified on Omarchy |
| --- | --- | --- |
| CMake | 3.25 or later | 4.4.4 |
| GNU Make | Unix Makefiles generator | 4.4.1 |
| Clang | C++20, matching AddressSanitizer and UndefinedBehaviorSanitizer runtimes | 23.1.3 |
| clang-format and clang-tidy | LLVM tools for the development checks | 23.1.3 |
| Qt 6 | 6.12 or later with Core, Gui, Network, Qml, Quick, QuickControls2, Test and QML tools | 6.12.0 |
| libevdev | Keyboard capture and input capability names | 1.14.0 |
| libudev | USB keyboard inventory and identity checks | 262 |
| xkbcommon | Hyprland keyboard layout and modifier interpretation | 1.13.2 |
| pkg-config | Resolve installed Linux libraries | 3.0.10 |

Both presets use Clang. The installed GCC 16.2.1 can compile the smoke test.
Using Clang for the presets gives clang-tidy a compilation database it can read
directly. Qt's CMake targets select the matching Qt 6 QML tools even on systems
with Qt 5 tools on PATH. The demo uses installed Qt Quick and Qt Quick Controls
modules, plus Qt Network for the local socket. The demo resolver uses Linux
input keycode definitions from the system headers. The separately launched
recorder and recording-enabled editor use the installed Linux libraries through
CMake's pkg-config targets. The profile and simulation service remains independent
of keyboard capture.

Run these commands from the repository root.

```sh
cmake --preset dev
cmake --build --preset dev
ctest --preset dev --output-on-failure --no-tests=error
cmake --build --preset dev --target format-check
cmake --build --preset dev --target clang-tidy
cmake --build --preset dev --target qml-check
cmake --preset asan
cmake --build --preset asan
ctest --preset asan --output-on-failure --no-tests=error
git diff --check
```

The presets build in `build/dev` and `build/asan` and place executables in each
tree's `bin` directory. The sanitizer preset enables AddressSanitizer and
UndefinedBehaviorSanitizer. Run its CTest command in a shell where LeakSanitizer
can inspect its process. Both CTest presets exclude tests labelled `hardware`.
Run hardware tests explicitly in a supervised session.

`format-check` checks C++ and compares Qt's formatted QML with the source.
`clang-tidy` analyzes project C++ using the preset's compile commands.
`qml-check` runs Qt's QML linter against the editor sources and generated type
metadata with a zero-warning budget.

## Run the demo

```sh
build/dev/bin/xkeys --demo
build/dev/bin/xkeysd --demo --stop
ctest --test-dir build/dev -L demo --output-on-failure --no-tests=error
```

The editor launches the sibling service if needed. Close the editor to leave
the demo service running, or use **Stop demo** to end it. Closing the editor
releases its held controls and simulated keys. The service retains the active
profile until explicit deactivation or service restart.

Saved demo profiles live under `$XDG_CONFIG_HOME/xkeys/demo/profiles`, with
`~/.config` as the usual configuration home. Opening the demo creates no
profile. Create or select a profile, choose **Edit** to save bindings, and
choose **Activate** to start simulation with the saved revision. In hold mode,
click an ordinary control to press it and click again to release it. Use
**Release all** to return to a neutral keyboard state. The output trace shows
the simulated transitions. The simulator sends no keyboard events to the operating system.

The default endpoint is in the user's private runtime directory. Both
executables accept `--socket /absolute/private/directory/service.sock` and
`--profiles-dir /absolute/profiles/directory`. A custom socket with no
profiles override uses a `profiles` directory beside the socket. The editor
forwards its chosen directory when it starts the service and verifies the
directory in the version 2 handshake before reporting Ready. Use the same
socket for the editor and stop command.

From the repository root, launch or focus the editor through Omarchy.

```sh
omarchy-launch-or-focus '^xkeys-demo$' 'build/dev/bin/xkeys --demo'
```

The `xkeys-demo` window class identifies the demo window. Automated editor
tests use Qt's offscreen platform with software rendering and temporary sockets.
The headless tests clear the desktop's GLX vendor override and the editor test
also clears input-method overrides. On the verified Omarchy host, inherited
NVIDIA GLX and Fcitx plugins allocated memory even in headless runs and
LeakSanitizer reported those allocations at exit. The isolated test environment
keeps leak detection active while testing the editor, service, and fake
recording adapters.

## Record a layer-one shortcut

```sh
build/dev/bin/xkeys --demo --recording
ctest --test-dir build/dev -L recording --output-on-failure --no-tests=error
```

In a Hyprland Wayland session, select a saved profile, choose **Edit**, select an
accessible USB keyboard, and click a layer-one control. Selection only displays its
binding; it does not acquire the keyboard or start a countdown. Click **Start
recording**, wait for **Press your shortcut**, then press and release the keys
together. Review the draft and choose **Save** or **Cancel**. The saved shortcut
appears on the control only after the
profile service acknowledges the write. Recording uses the selected keyboard's
layout as reported by Hyprland. A custom global keymap file prevents recording
when the editor cannot establish the key labels. Stored codes identify physical
keys.

The optional recorder uses existing session access. A keyboard with inaccessible
or busy interfaces cannot record, and the editor reports why. The editor and
helper verify that the selected USB group still matches the current inventory.
Where the inventory identifies wired and receiver interfaces as routes for one
keyboard, the helper captures them together. Other devices stay outside the
capture boundary. Cancel with the mouse or move focus away from the
editor. Release held keys to let the helper drain before it exits. The drain can
wait up to two seconds for keys to return to neutral. The helper has a ten-second
capture deadline and a fifteen-second total deadline. The editor has a watchdog
for a helper that stops responding. Compositor handling after a forced stop
requires the supervised release-bind trial.

The overlap check reports possible conflicts with Hyprland bindings and common
virtual-terminal shortcuts for later playback. Application hotkeys may be missing.
For a known conflict or incomplete coverage, choose **Use binding anyway** or
**Record different shortcut**. Recording creates a profile binding without
activating it or sending keys.
The default `--demo` launch keeps recording disabled. Automated tests use fake
keyboards, helper streams, and compositor metadata.

The **Recording device** list can include composite devices such as a gaming mouse
that exposes keyboard interfaces. Those entries remain available for keyboard
shortcut events. Native mouse buttons and wheel actions are not supported by the
current recorder, profile format or output implementation.
