# Implementation Plan — NAV-017 (recently visited folders)

Source: a new requirement, proposed here. Related but different from NAV-006 (per-panel back/forward) and NAV-007
(named favorites you add by hand).

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| NAV-017 (proposed) | The application shall keep a list of the most recently visited local folders, across both panels, and let the user jump to one from a popup list. | P3 | IN-MVP | Not Started |

## Summary

Back/forward (`Alt+Left`/`Alt+Right`) is per panel and linear; the hotlist (`Ctrl+D`) holds only what you saved. A
**recent folders** list fills the gap: "that folder I was in ten minutes ago, in either panel".

- **Open:** `Alt+F12` (or **Commands → Recent folders**) shows the list, newest first.
- **Go:** `Enter` takes the active panel to the folder. `Del` removes an entry. `Ctrl+D`-style "add to favorites":
  `F2` copies the entry into the hotlist (offered, see open questions).
- **What is recorded:** every local folder either panel settles on, at most once (moved to the top when revisited),
  capped at 30. Plugin/provider locations are not recorded.
- **Storage:** its own small file, written at a clean exit like the command history; a config key turns persistence
  off (then the list lives for the session only).

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| A separate persisted list, written only after a clean exit, capped, atomic write | `CommandHistory.{h,cpp}`, `Utf8TextFile.h` | The storage pattern, including CLI-004's "persist" preference |
| A scrollable list with caller-chosen extra keys | `Dialog.h` `ShowListChoiceWithKeys` (NAV-007) | The popup |
| Lexical, case-insensitive path equality that never touches the disk | `Favorites.h` `FindFavoriteByPath`, `PathUtil.h` | De-duplication, so an offline share can't block it |
| A loop that compares each panel's path every iteration | `main.cpp` `PollAutoRefresh` (LIST-007) | Recording a location change without hooking every navigation |
| `Alt+F12` is free | `KeyMap.cpp` | The default key |

## Design

### D1 — `RecentLocations` (pure, `Source/MyCommander/RecentLocations.{h,cpp}`)

- `Note(path)`: moves/inserts `path` at the front; case-insensitive and trailing-separator-insensitive; trims to the cap.
- `Remove(path)`, `Entries()`, `Clear()`.
- `ParseRecentFile(text)` / `FormatRecentFile(entries)`: UTF-8, one path per line, newest first, blank lines and
  duplicates ignored, over-long lists trimmed.
- No filesystem access, no `Console`.

### D2 — Recording (`main.cpp`)

- Per side, remember the last recorded path. Each loop iteration, if the side shows no plugin content and no provider and
  its `Path()` differs from the remembered one, `Note` the new path.
- A location is recorded only after it has been listed successfully (`!LocationInaccessible()`), so a mistyped or dead
  path never enters the list.
- The two drive roots at startup (restored from the session) are recorded too — they are real places visited.

### D3 — The popup

- Rows show the full path. A folder that no longer exists is **not** probed (an offline share would stall the list);
  choosing it navigates through the normal `NavigateTo` path, which already shows LIST-006's banner for a missing place.
- `Enter`: go. `Del`: remove the entry (no confirm: it is only a recent-list entry). `F2`: add the entry to the hotlist
  through the existing Favorites code, with its default name.
- The panel that gets the folder is the active one (`NavigateTo`, so it lands in that panel's own history too).

### D4 — Storage and config

- File: `%APPDATA%\MyCommander\recent-folders.txt`; loaded at startup, saved at the normal exit point beside the command
  history and session.
- Config: `persistRecentFolders = true | false` (default `true`). When `false` nothing is read or written and the list
  is session-only.
- **ERR-006:** the list is navigation history only; nothing in it can be replayed as an operation. It carries no
  commands, selections or file operations, the same reasoning as the command history's own file.
- **ERR-005:** paths are not secrets, and are not written to the log.

### D5 — Keys, menu, help

- `CommandId::RecentFolders` / `recent-folders` with `K(VK_F12, _, _, A)`; inventory entry and test updated.
- **Commands → Recent folders**, hint-bar row, F1 and README/USERGUIDE tables.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/RecentLocations.{h,cpp}` (new) | List logic and file format |
| `Source/MyCommander/Config.{h,cpp}` | `persistRecentFolders` |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp` | Command and key |
| `Source/MyCommander/MenuBar.h`, `HintBar.h`, `Help.cpp`, `main.cpp` | Menu, hint, help row, recording, popup, load/save |
| `Source/MyCommanderTests/RecentLocationsTests.cpp` (new), `ConfigTests.cpp`, `KeyMapTests.cpp` | Tests below |
| both `.vcxproj`, `README.md`, `USERGUIDE.md`, `CLAUDE.md`, spec (new NAV-017 row), changelog, `Version.h` | Docs |

## Tests

- **`RecentLocations`:**
  - a new path goes to the front; a revisit moves it to the front without a duplicate;
  - the comparison ignores case and a trailing `\`;
  - the cap drops the oldest;
  - `Remove` and `Clear`;
  - parse/format round trip, Unicode paths, CRLF input, blank lines, duplicates in the file.
- **Config:** the key's default and parsing (`ConfigTests.cpp`).
- **Manual:** navigate both panels around, open the list, confirm order and de-duplication across the panels; delete an
  entry; restart and confirm persistence; set the key to `false`, restart, confirm nothing is written; choose a folder
  that has since been deleted (banner, not a hang); choose a path on an unplugged network drive.

## Phasing

1. `RecentLocations` and its tests. 2. Recording, popup, load/save, config. 3. Keys, menu, docs.

## Risks

- **A noisy list:** every folder you pass through (stepping into a folder with Enter, then straight on) is recorded.
  Mitigation: record only when the location is the one the panel *rests* on for the current loop iteration, which is
  what D2 does; a stricter rule (only after N seconds) is an open question.
- **Privacy:** the file reveals folder names in clear text. Same exposure as `session.ini` and `favorites.conf`;
  `persistRecentFolders = false` is the opt-out.

## Open questions

1. **Persist by default?** Recommended: yes, like the command history.
2. **A dwell time** (record a folder only after you stayed 2 seconds)? Recommended: no, keep it simple; revisit if the list
   proves noisy.
3. **`F2` = add to the hotlist** from this list? Recommended: yes, it is the natural way to promote a recent folder.

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.35.0) with the recommended answers: persisted by default, no
dwell time, `F2` adds to the hotlist.

Differences from the plan:

- **Recording** skips a panel showing plugin content, a provider or an unreachable location (`LocationInaccessible`),
  and uses `Panel::Path()` -- so the folders restored from the session at startup are recorded on the first loop pass.
- **File format** carries a two-line `#` header like `favorites.conf`; `#` lines are ignored on load.
- **Module state:** the list is a file-scope `gRecentFolders` in `main.cpp` (the `gJobWorker` precedent) so the popup,
  the recording hook and the load/save share it without a new parameter.
- **Menu:** `Commands > Recent folders` (mnemonic `R`), a simulated `Alt+F12`.
