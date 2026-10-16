# Implementation Plan — FOP-026 (create a shortcut to the selection)

Source: a new requirement, proposed here. Reuses the shortcut-creation code the installer already has
(`Source/MyCommanderSetup/main.cpp`, `IShellLinkW` for the desktop shortcut, INST-005).

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| FOP-026 (proposed) | The user shall be able to create Windows shortcut files (`.lnk`) to the selected files and folders, in the active folder or on the desktop, without overwriting an existing file. | P3 | IN-MVP | Not Started |

## Summary

A shortcut to a frequently used file or project folder, placed where it is needed, without leaving the manager.

- **Command:** **File → Create shortcut**, menu only (no key by default; open question 2).
- **Target:** each selected item, or the cursor entry when nothing is selected (`..` is refused).
- **Where:** a choice in a small form: *In this folder* (default) or *On the desktop*.
- **Name:** `<name> - Shortcut.lnk` (the Windows convention); a folder's shortcut is named the same way.
- **Properties set:** target path, working directory (the target's own folder), description (the target's full path), and
  the target's icon (the shell's default for the target).
- **No overwrite:** an existing `.lnk` with the chosen name gets a numbered name (` (2)`), never a replacement.
- **Local targets only:** a provider or plugin panel is refused (a server or archive entry is not a Windows path).

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| `IShellLinkW` + `IPersistFile` shortcut creation | `Source/MyCommanderSetup/main.cpp` (INST-005 desktop shortcut) | The COM pattern, moved into a shared place |
| Desktop folder lookup | `SHGetKnownFolderPath` (installer) | The "On the desktop" destination |
| Selection and cursor | `Panel::SelectionOrCursor` | The targets |
| Unique name generation | `FileOps.cpp` conflict naming (the existing "Rename" rule) | ` (2)` suffixes |
| Refusals for providers | `main.cpp` `RefuseInsideProvider` | The refusal |

## Design

### D1 — The shortcut writer (`Shortcut.{h,cpp}`)

- `CreateShortcutFile(linkPath, targetPath, workingDir, description)` wraps `IShellLinkW` and `IPersistFile::Save`, with
  COM initialized and released in a scope guard.
- The installer's own `IShellLinkW` code is moved here and the installer calls this function, so the COM pattern exists
  once (INST-005 keeps its behaviour; its tests are the existing installer checks).
- Failure is returned as a Win32 error string, never thrown.

### D2 — Pure naming (`Shortcut.{h,cpp}`)

- `ShortcutNameFor(name, existingNames)`: `<name> - Shortcut.lnk`, then ` (2)`, ` (3)` ... when taken (case-insensitive).
- The name is validated with the existing `ValidateNewFileName` rules (MultiRename, reserved names and length).

### D3 — Command (`main.cpp`)

- `DoCreateShortcut`: resolves targets (refuses `..`-only); the small form (destination choice); computes each link path;
  creates the links; reports the outcome through `ReportOutcome`; refreshes the destination panel when it is the active
  folder.
- Refused for provider and plugin panels, and for an inaccessible location.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/Shortcut.{h,cpp}` (new) | `CreateShortcutFile`, `ShortcutNameFor` |
| `Source/MyCommanderSetup/main.cpp` | Calls the shared writer instead of its own copy (behaviour unchanged) |
| `Source/MyCommander/main.cpp` | `DoCreateShortcut` |
| `Source/MyCommander/MenuBar.h`, `Strings.h`, `Help.cpp` | Menu item, messages, help row |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp` | Only if the command is given a key (open question 2) |
| `Source/MyCommanderTests/ShortcutTests.cpp` (new) | Tests below |
| Both `.vcxproj` files | New source and test files; the installer project gains the shared file |

## Tests

- **`ShortcutNameFor` (pure):** a free name; a taken name gives ` (2)`; case-insensitive clash; a name with a dot in the
  middle; a name that fails validation is reported.
- **`CreateShortcutFile` on `TempDir`:** creates the `.lnk`; reading it back with `IShellLinkW::GetPath` returns the target
  and the working directory; a second creation with the same name is not made (the naming step prevents it, tested with
  the naming function).
- **Installer regression:** the existing installer checks still pass after the move (the installer's own test target,
  unchanged).
- **Refusals:** provider and plugin panels, `..` alone.
- **Manual:** create a shortcut to a file and a folder in the folder and on the desktop; double-click one; a second one
  gets ` (2)`.

## Documentation updates

- `AIPrompt/my-commander.md`: new row **FOP-026**; INST-005's note that the shortcut writer is shared; version bump;
  history paragraph.
- `README.md`: the File menu list; a short "Shortcuts (FOP-026)" note in the file-operations section.
- `USERGUIDE.md`: a short section with the naming and no-overwrite rules.
- `Source/MyCommander/Help.cpp`: F1 row (menu item).
- `CLAUDE.md`: `Shortcut` in the source layout (the one `IShellLinkW` home, shared with the installer); the installer
  paragraph updated to say it calls the shared writer; Testing list.
- `AIPrompt/ManualTests.md`: new MT entry; MT-046 (installer) gets a note that the shortcut code moved.
- `AIPrompt/features/FOP-026-plan.md`: status and differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.

## Phasing

1. `ShortcutNameFor` and tests (pure). 2. `CreateShortcutFile`, moved from the installer, with the installer regression.
3. Command, form, menu, docs.

## Risks

- **Changing the installer:** the shortcut code is moved, not rewritten, and the installer's own checks cover it. The
  installer is the riskiest part; the plan keeps its change to one function call.
- **Shortcut to a file that is later moved:** a shortcut breaks, as in Explorer; expected, not handled.

## Open questions

1. **Destination default:** in this folder (recommended), or the desktop?
2. **Key:** menu only (recommended for now).
3. **Icon:** the shell default for the target (recommended), no custom icon.

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.47.0, version 2.11.0): `Shortcut.{h,cpp}`,
`ShortcutTests.cpp`, `File > Create shortcut` (menu only, mnemonic H), the installer calls the shared writer, MT-064.

Open questions: default destination this folder; menu only; shell default icon.

Differences from the plan:

- Name validation by `ValidateNewFileName` was not used: the name is derived from an existing file name, so only the
  255-character limit is checked (`ShortcutNameFor` returns empty beyond it), keeping `Shortcut.cpp` free of MultiRename
  so the installer can compile it.
- No F1 row, as for the other menu-only commands (Secure delete, Export listing).
- The installer project compiles `Shortcut.cpp` directly (the `Process.cpp` precedent) and its `CreateDesktopShortcut`
  now makes one call; the whole solution builds.
