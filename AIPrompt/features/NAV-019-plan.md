# Implementation Plan — NAV-019 (reveal the current item in Windows Explorer)

Source: a new requirement, proposed here. Uses the same shell-launch ground as the app's "open in Explorer"-style
actions (`Process.h`, the F9 shell and the file-association launch) without a new third-party dependency.

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| NAV-019 (proposed) | The user shall be able to open Windows Explorer on the active folder, with the cursor entry (or the first selected entry) selected. | P3 | IN-MVP | Not Started |

## Summary

Sometimes the fastest way to see a file's context is Explorer: its preview pane, its Properties, its "open with". This
adds **View → Show in Explorer** (first planned under Commands; moved to View at the user's request).

- **Action:** opens a new Explorer window on the active folder with the cursor entry selected. With a selection, the
  first selected entry is selected (Explorer selects one item in this call).
- **Local folders only.** A provider or plugin panel is refused with a message (no Explorer path exists for it).
- **No key by default** (menu only); open question 2.
- **No elevation,** no new process privilege (SEC-002): an ordinary shell request by the current user.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| Shell launch of an ordinary program with argument quoting | `Process.h` `RunDetached`, `QuoteCommandLineArgument` (SEC-004) | Only if the API route is not used (see D1) |
| Shell association launch | `main.cpp` `RunFileAction` (`system:` actions, VEE-008) | Precedent for launching from the shell |
| The active panel's selection and cursor | `Panel::SelectionOrCursor`, `CursorFilePath` | What to select |
| Refusal for providers and plugins | `main.cpp` `RefuseInsideProvider`, `Content()` checks | The refusal |

## Design

### D1 — The shell call

- Preferred: `SHOpenFolderAndSelectItems` (shell32) with the folder's PIDL and the entry's PIDL from
  `SHParseDisplayName`. This selects the entry without building a command line, so no quoting problem can arise for a
  file name with spaces, `&`, or Unicode characters.
- Fallback: `explorer.exe /select,"<path>"` through `RunDetached` with `QuoteCommandLineArgument`, used only if the PIDL
  route fails (it should not on supported Windows versions); the fallback is tested for a path containing a space and an
  ampersand.
- The paths come from the panel (`Path()` and the entry name), never from typed text, so there is no injection surface.

### D2 — Pure part (`ExplorerReveal.{h,cpp}`)

- `RevealTargetFor(folder, selection)`: returns the folder and the single entry to select (the cursor entry, or the first
  selected one in listing order), or none for `..` alone (the folder is opened without a selection).
- The shell call itself is a thin wrapper, kept out of the pure module so the decision is unit-tested without Explorer.

### D3 — Command (`main.cpp`)

- `CommandId::RevealInExplorer` / `reveal-in-explorer`, menu item under **Commands**; no default key (open question 2).
- Refused with a message for provider and plugin panels, and for a location that is inaccessible (LIST-006).
- A failed shell call is logged (Warning, path only) and shown as one message line.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/ExplorerReveal.{h,cpp}` (new) | `RevealTargetFor` (pure), the shell wrapper |
| `Source/MyCommander/main.cpp` | `DoRevealInExplorer`, refusals |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp`, `MenuBar.h`, `Help.cpp`, `Strings.h` | Command, menu item, help row, strings |
| `Source/MyCommanderTests/ExplorerRevealTests.cpp` (new), `KeyMapTests.cpp` | Tests below |
| Both `.vcxproj` files | New files; `shell32.lib` (verify it is linked already for FileInfo/Clipboard) |

## Tests

- **`RevealTargetFor` (pure):** cursor entry chosen with no selection; the first selected entry in listing order when there
  is a selection (the cursor elsewhere); `..` alone → folder, no selection; an empty folder → folder, no selection.
- **Quoting fallback (pure):** `QuoteCommandLineArgument` on a path with a space, `&`, a quote and a non-ASCII name builds
  an argument `explorer.exe` receives unchanged (checked by splitting the built command line back).
- **Shell wrapper:** not automated (it opens a real Explorer window, like `RunElevated`'s UAC path); covered by the manual
  test and by a note in `CLAUDE.md`.
- **Refusal:** a provider panel is refused (tested through the refusal helper).
- **Manual:** a file with a space and an ampersand in its name, and a multi-selection; provider panel refused.

## Documentation updates

- `AIPrompt/my-commander.md`: new row **NAV-019**; version bump; history paragraph.
- `README.md`: the Commands menu list; a sentence in the Explorer-related section (where the file is shown).
- `USERGUIDE.md`: a short "Showing a file in Explorer" section.
- `Source/MyCommander/Help.cpp`: F1 row (menu item only).
- `CLAUDE.md`: `ExplorerReveal` in the source layout; the note that the shell wrapper is not automated, like
  `RunElevated`; Testing list.
- `AIPrompt/ManualTests.md`: new MT entry.
- `AIPrompt/features/NAV-019-plan.md`: status and differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.

## Phasing

1. `RevealTargetFor` and the quoting tests (pure). 2. Shell wrapper and refusals. 3. Command, menu, docs.

## Risks

- **Explorer may open a new window instead of reusing one:** that is Explorer's behaviour and is documented, not a bug.
- **A path that no longer exists:** the reveal reports it (a stale panel after an outside change is refreshed by LIST-007
  first in practice).

## Open questions

1. **Multi-selection:** Explorer selects one item; the first selected one is used (recommended), and the message says so
   when more than one was selected.
2. **Key:** menu only (recommended for now).

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.49.0, version 2.13.0): `ExplorerReveal.{h,cpp}`,
`ExplorerRevealTests.cpp`, View > Show in Explorer (menu only, mnemonic O), MT-066.

Open questions: first selected entry is revealed and a note says so; menu only.

Differences from the plan:

- No `CommandId`/key-map entry and no F1 row: like Secure delete and Export listing it is a menu-only command.
- The fallback argument builder is own (`/select,"<path>"`) rather than `QuoteCommandLineArgument`; a Windows file
  name cannot contain a quote, and it is tested with a space, ampersand and non-ASCII characters.
- With no item (".." alone) the folder is opened with `ShellExecuteW`.
