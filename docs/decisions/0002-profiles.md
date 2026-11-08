# ADR 0002 — Versioned profiles with two blank layers

- **Status** Accepted
- **Date** 2026-11-08

## Context

Profiles need stable identity across renames, predictable blank defaults, and a
safe way to reject changes from an outdated editor. The service owns persistence
and activation as separate operations. The simulation uses provisional control
IDs until the hardware checks confirm device identities.

## Decision

Store one JSON object per profile under the application config root. The
filename is the canonical lowercase UUID plus `.json`. The UUID is assigned
at creation. The display name is trimmed, nonempty, and limited to 80 Unicode
characters. Slashes, backslashes, control characters, and the names `.` and
`..` are invalid. The store compares names case-insensitively when creating or
renaming a profile.

Version 1 requires these fields.

| Field | Value |
| --- | --- |
| `version` | Integer `1` |
| `id` | Lowercase UUID without braces |
| `name` | Valid display name |
| `revision` | Integer through `9007199254740991`. Zero denotes an unsaved profile. |
| `thumbLayer` | Boolean, initially `false` |
| `layers` | Exactly two objects containing a binding for each of the 28 known control IDs |

Each binding contains a string `label` and an array of ordered integer
`keys`. A blank binding has an empty array and an empty label. A chord holds
up to 16 distinct Linux keyboard codes in the range 1–255, excluding
`KEY_UNKNOWN`. Ctrl, Shift, Alt, and Meta keys precede ordinary keys. Labels
are optional for bound chords, limited to 80 Unicode characters, and free of
control characters. A new profile starts with both layers entirely blank.

Require an expected revision for updates and deletes. Creation expects zero
and assigns revision one. Updates match the saved and submitted revisions,
then increment once. The service serializes writes with a directory lock so
name and revision checks are made against the same store state. Replacement
uses a same-directory temporary file and atomic commit, with direct-write
fallback disabled. Removing a profile only removes the validated UUID file.

## Consequences

Invalid fields, missing controls, unsafe filenames, symlinks, unreadable
files, and unsupported versions report errors. A file is limited to 256 KiB,
and the directory holds up to 128 profiles. The store reports malformed JSON
when listing so an invalid file is visible to the caller. Revision conflicts
require a reload before retrying. Profile storage does not activate mappings.

Adding a control or changing the keycode rules requires a schema review and
an explicit migration for saved profiles.

## Rejected alternatives

- **Display names as filenames.** Rename would change identity and couple
  display text to path handling.
- **Sparse layers with implicit defaults.** A missing control could silently
  hide damaged data or an incompatible set of controls.
- **Direct overwrite.** A failed write could leave a truncated profile.
