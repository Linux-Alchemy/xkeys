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
| libevdev | Planned for hardware integration | 1.14.0 |
| libudev | Planned for hardware integration | 262 (pkg-config) |

Both presets use Clang. The installed GCC 16.2.1 can compile the smoke test.
Using Clang for the presets gives clang-tidy a compilation database it can read
directly. Qt's CMake targets select the matching Qt 6 QML tools even on systems
with Qt 5 tools on PATH. The demo uses installed Qt Quick and Qt Quick Controls
modules, plus Qt Network for the local socket. The demo resolver uses Linux
input keycode definitions from the system headers. The build has no libevdev
or libudev linkage.

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
the demo service running, or use **Stop demo** to end it. The default endpoint
is in the user's private runtime directory. Both executables accept
`--socket /absolute/private/directory/service.sock` for an isolated session.
Use the same path for the editor and the stop command.

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
keeps leak detection active while testing the editor and service.
