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
| Qt 6 | 6.12 or later with Core, Test, QML tools and the Qt Quick QML module | 6.12.0 |
| libevdev | Planned for hardware integration | 1.14.0 |
| libudev | Planned for hardware integration | 262 (pkg-config) |

Both presets use Clang. The installed GCC 16.2.1 can compile the smoke test.
Using Clang for the presets gives clang-tidy a compilation database it can read
directly. Qt's CMake targets select the matching Qt 6 QML tools even on systems
with Qt 5 tools on PATH. The build smoke test links Qt Core and Qt Test. The
QML checks load Qt Quick from the installed QML modules. The build has no
libevdev or libudev linkage.

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
`qml-check` runs Qt's QML linter with a zero-warning budget. Until editor sources
exist, the QML checks exercise a generated Qt Quick component in the build tree.
