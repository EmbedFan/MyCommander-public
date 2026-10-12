# Implementation Check — ARC-001 (ZIP archives browsable as virtual directories)

Source: `AIPrompt/my-commander.md` §13:

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| ARC-001 | ZIP archives should be browsable as virtual directories. | P2 | LATER | Not Started |

## Result (2026-10-08): already implemented; only the spec row is stale

**No new code is needed.** ARC-001 was delivered through the ZIP plugin (FEA-0007, Phase 3), and is tracked
there under the `ZIP-` IDs. The §13 row was never updated, so it still says LATER / Not Started.

| ARC-001 needs | Delivered by | Where |
|---|---|---|
| Enter a `.zip` like a folder | `ZIP-005`: `Enter` on a `.zip` steps into it | `ZipPlugin.cpp`. The host side is the SDK's `kMcPluginCapArchiveHandler` (`McPlugin_CanHandleFile` / `McPlugin_EnterArchive`, `McPluginAbi.h`) and `IPanelContent` (`Panel.h`) |
| Navigate inside it like a directory | Sub-folders, `Backspace` to the parent folder, cursor and selection (`Insert`, mouse), a panel-rendered listing | `ZIP-005`/`006` |
| Leave it back to the real file system | Confirmation at the archive root | `ZIP-007` |
| Use it like a directory | `F3` view, `F5`/`F6` extract to the other panel, adding files from the other panel, `F8` delete, `F7` new folder, reload, `Ctrl+A` attributes, `Shift+F5` copy-as | `ZIP-006`, `009`, `011` |
| Edits saved safely | Batched in memory, saved/discarded/cancelled on leaving | `ZIP-010` |
| Available to users | `ZipPlugin.dll` is shipped and installed by the installer into `Plugins\` | IS-0013, `MyCommanderSetup` |

These are all marked IN-MVP / Implemented in §FEA-0007's ZIP table, with tests in `ZipNavigationTests.cpp`,
`ZipConsoleTests.cpp` and `ProgressDialogTests.cpp`. They are documented in the USERGUIDE ("Browsing ZIP archives")
and in the plugin's own F1 help.

**Related rows that are equally stale** (not asked for, but the same evidence):

| ID | Requirement | Evidence | Suggested status |
|---|---|---|---|
| ARC-002 | Create and extract ZIP archives | `ZIP-001`–`004`: File → Pack file / Unpack Package, plus extraction from the browser | IN-MVP / Implemented |
| ARC-003 | Additional archive types use an extensible provider mechanism | The SDK's `kMcPluginCapArchiveHandler` lets any plugin claim a file type as a virtual directory (`CanHandleFile` + `EnterArchive` + panel content). ZIP is the only provider so far | IN-MVP / Implemented (the mechanism exists; no second format is required) |
| ARC-004 | Archive passwords never in logs or command history | Password-protected archives aren't supported at all (`ZIP-008`, LATER), so there's nothing to leak yet | Unchanged: LATER / Not Started |

## Known limits (not gaps in ARC-001, recorded for honesty)

- **Only `.zip` by extension.** `.jar`, `.docx`, `.xlsx` and other ZIP-based formats aren't claimed. A one-line
  extension list in the plugin's `CanHandleFile` would cover them, but they'd open as plain archives, which is
  rarely what a user wants for `.docx`.
- **Nested archives and encrypted entries** are `ZIP-008` (LATER).
- **Host features that refuse plugin panels:** Alt+F7 search, Shift+F2 compare and Ctrl+Shift+F2 sync don't look
  inside archives. That is by design (SRC-004..007 list it as out of scope).
- **Without the plugin** (deleted, or disabled via `disabledPluginIds`), `Enter` on a `.zip` falls back to the
  normal Enter-on-a-file action. That is the documented plugin design (FEA-0002), not a defect.

## Proposed change (on approval)

1. **Spec §13:**
   - ARC-001 and ARC-002 → IN-MVP / Implemented, each with a note "via the ZIP plugin, `ZIP-001`–`011`";
   - ARC-003 → IN-MVP / Implemented, with a note "`kMcPluginCapArchiveHandler`";
   - ARC-004 unchanged;
   - a history note (no version bump: documentation only).
2. **Changelog entry.**
3. **Optional, only if wanted:** claim `.jar` (and other plain ZIP-container extensions) in the plugin. That is
   a ZipPlugin minor version and one test in `ZipNavigationTests.cpp`.

## Open questions

1. Update ARC-001 (and ARC-002/003) in the spec as above?
2. Should the plugin also browse `.jar` / other ZIP-based extensions, and if so which?

## Status

Checked 2026-10-08. Nothing to implement for ARC-001. Both open questions were answered yes, and both changes are
done:

1. **Spec:** ARC-001/002/003 are now IN-MVP / Implemented, with a §13 note and a history entry. No app version
   change.
2. **ZipPlugin 0.12.0** claims `.zip .jar .war .ear .apk .xpi .vsix .nupkg .whl`.
   - One `IsZipContainerName` list is used by `McPlugin_CanHandleFile`, Unpack Package and the archive info
     command.
   - Office/ODF/EPUB documents are deliberately left out.
   - `ZipNavigationTests.cpp` checks the claimed, excluded and rejected names. The harness reports 0 failures.
