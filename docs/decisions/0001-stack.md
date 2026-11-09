# ADR 0001 — Native editor and separate remapping service

- **Status** Accepted
- **Date** 2026-11-08

## Context

xkeys needs a visual keypad for recording and testing shortcuts on an Omarchy
desktop. The editor may close while the remapping service continues handling the
pad. The demo must exercise the same editor-to-service boundary using synthetic
input before any device is acquired.

## Decision

Use C++20 for the editor logic and service, Qt 6 Quick/QML for the window, and
Qt Test for automated checks. Build both executables with CMake and installed
system dependencies. The editor receives acknowledged state over a private
filesystem socket. The service resolves inputs and owns the lifetime of any
future remapping session. The demo service supports synthetic Test only.

For live remapping, a pad key holds its assigned shortcut until release.
Every login or service restart starts with no active profile. Opening or
focusing the editor, selecting a profile, and saving edits each leave activation
to a separate explicit action. Deactivate releases held outputs and returns the
pad to factory behaviour. Screen lock preserves activation. Suspend recovery
may resume an active session after identity and neutral-input checks.

Use Omarchy's launch-or-focus helper for normal editor launch and focus. Keep
the demo window class distinct from the future normal editor class. The helper
matches windows, while the service independently owns the endpoint and rejects
conflicting Test requests.

## Consequences

C++ ownership and IPC failure paths require explicit testing. Qt's declarative
UI keeps the editor layout separate from service state, while the versioned
protocol gives the UI a definite acknowledgement and disconnect path. Linux
input permissions, hardware discovery, and recovery will be settled before the
live service is introduced.

## Rejected alternatives

- **Python application logic.** Python was considered for readability. The
  native Omarchy target and the decision to delegate implementation favoured
  C++ with Qt.
- **Remap inside the editor process.** Closing the editor would end remapping
  and bind the lifetimes of the user interface and device session together.
