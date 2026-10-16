# Fix Plan — IS-0022 (shared folders aren't marked in the list; Unshare says "not shared")

Source: `AIPrompt/issue_fix_requests.md`:

> When we share a directory there is not seen this on the list. It would be nice to prefix this file/directory name
> with ">". On the windows the sharing is seen. After that when I want to unshare the software tell, the folder is
> not shared.

Two parts: **(A)** the Unshare defect (a bug), **(B)** a new visual marker for shared folders (a small feature, new
requirement row `LIST-013`).

## Root cause of (A) — Unshare looks at the wrong folder

Checked against the code and on this machine:

- `DoShareFolder` (`main.cpp`) shares the **folder under the cursor** (falls back to the panel folder only when the
  cursor isn't on a real folder).
- `DoUnshareFolder` calls `EnumerateSharesFor(target.Path())`, the **panel's own folder**, never the cursor folder.
- So the normal sequence is: cursor on `Projects`, Share Folder → `C:\Work\Projects` is shared. Cursor still on
  `Projects`, Unshare Folder → it asks about `C:\Work` → "This folder is not shared." Windows confirms the share, as
  the user saw.
- It is not a rights problem: `NetShareEnum` level 2 returned the shares for this non-administrator process (probe
  run; the registry key `HKLM\SYSTEM\CurrentControlSet\Services\LanmanServer\Shares` is readable too). The comment in
  `NetShares.h` ("no admin rights required") holds up here.
- Smaller contributors in `EnumerateSharesFor`: it compares with `_wcsicmp` on `ToLongPath(folder)` minus `\\?\`, so a
  trailing separator or a `\\?\UNC\` prefix on the folder would also miss. The comparison goes into a pure helper
  (below).

## Fix

### F1 — Unshare uses the same target rule as Share

- A tiny shared helper in `main.cpp`, `ShareTargetFolder(const Panel&)`: the cursor entry when it is a real folder
  (not `..`), else the panel folder. Both `DoShareFolder` and `DoUnshareFolder` call it.
- The Unshare dialogs name the folder they looked at: "`C:\Work\Projects` is not shared." The list title shows the
  folder too, so a wrong guess is visible.
- If the cursor folder is not shared but the **panel folder** is, Unshare falls back to the panel folder's shares
  (covers "I stepped inside the shared folder and chose Unshare"). The list then shows the folder in its title.

### F2 — Pure path comparison for shares (`NetShares.{h,cpp}`)

- New pure `bool SharePathMatches(const std::wstring& sharePath, const std::filesystem::path& folder)`: lexical,
  case-insensitive, `\\?\` and `\\?\UNC\` prefixes removed, trailing separators ignored, a drive root equals `C:\`.
  `EnumerateSharesFor` uses it instead of the inline `_wcsicmp`.
- `ShareInfo` gains `isSpecial` (`shi2_type & STYPE_SPECIAL`: `C$`, `ADMIN$`, `IPC$`).

### F3 — Mark shared folders with `>` (new `LIST-013`)

- **Source of truth:** a new `std::vector<ShareInfo> EnumerateAllShares()` (one `NetShareEnum` call) in
  `NetShares.cpp`, administrative/special shares excluded. A pure `SharedFolderSet` class (in `NetShares.h`) is built
  from that list: `Contains(path)` through the same normalised key as `SharePathMatches`, case-insensitive, no disk
  probe.
- **When it is read:** a small cache in `main.cpp` (`gSharedFolders`), refreshed
  - on startup and once every **3 seconds** (the main loop's existing tick; one local API call, no thread),
  - immediately after Share Folder / Unshare Folder finish.
  Only local folders are marked (never inside a provider or plugin content). A failed enumeration leaves the previous
  set in place (no flicker, no marks lost to a transient error).
- **Drawing:** `>` marks every folder in the listing whose full path is in the set (a folder is marked when it *is* a
  share root; its subfolders are not). Shown in Full and Brief view; colours are untouched (cursor, selection, label,
  broken). It is plain ASCII, so it works in the ACC-006 fallback frame too, and never colour alone.
- **Where the `>` goes** (see open question 1): a third cell in the row marker prefix.
- Config `showShares` (default `on`) — same pattern as `showLabels` (LIST-009), so a user who doesn't want the extra
  call or column can switch it off (`off` also skips the 3-second polling).

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/NetShares.{h,cpp}` | `SharePathMatches`, `ShareInfo::isSpecial`, `EnumerateAllShares`, `SharedFolderSet`; `EnumerateSharesFor` uses the helper |
| `Source/MyCommander/main.cpp` | `ShareTargetFolder`; Share/Unshare use it (+ fallback and clearer messages); `gSharedFolders` cache + refresh; `>` in the Full and Brief rows |
| `Source/MyCommander/Config.{h,cpp}` | `showShares` (default on), commented line in the default file |
| `Source/MyCommanderTests/NetSharesTests.cpp` (new), `MyCommanderTests.vcxproj`, `ConfigTests.cpp` | Tests below |
| `AIPrompt/my-commander.md`, `README.md`, `USERGUIDE.md`, `CLAUDE.md` (NetShares line), manual tests, `Version.h`, changelog | spec 1.50.0 → 1.51.0 (new `LIST-013`, `NAV-015` note); 2.13.1 → 2.13.2 |

## Tests

**`NetSharesTests.cpp`** (pure, no real share is created or removed — TST-004):

- `SharePathMatches`: equal paths; different case; trailing `\`; `\\?\` and `\\?\UNC\srv\share` prefixes; a drive root
  `C:` / `C:\`; a child or parent is not a match; an empty path matches nothing.
- `SharedFolderSet`: built from several `ShareInfo` — contains the share root, not its subfolder or parent; special
  shares skipped; a duplicate path (two share names for one folder) is one entry; case and separator insensitive.
- `ShareTargetFolder`'s rule is tested as a pure function taking (panel path, cursor entry name, is-directory):
  folder under cursor; `..` falls back to the panel; a file falls back to the panel.
- `EnumerateAllShares` has one smoke case: it returns without crashing and never contains a `$` special share (it
  reads the real machine, makes no change).

**`ConfigTests.cpp`:** `showShares` default on, parsed `off`, a bad value warns and keeps the default.

## Manual test (added to `ManualTests.md`)

1. Cursor on a folder, Network → Share Folder, accept the UAC prompt. Within 3 seconds the folder shows `>`.
2. Cursor still on it, Network → Unshare Folder: the list shows the share; unshare; `>` disappears at once.
3. Step into the shared folder and choose Unshare: the fallback finds it.
4. `showShares = off`: no marks, no polling.
5. Share a folder from Explorer while MyCommander is open: the mark appears within 3 seconds.

## Decisions (2026-10-16)

1. **Marker position:** a third cell in the row marker (the recommended option), only while `showShares` is on.
2. **Admin shares:** hidden.
3. **Refresh timing:** by timer, and the period is a global config setting: `shareRefreshSeconds` (0-3600, default 3; 0 = no timer, refresh only after Share/Unshare Folder and when a panel re-lists).
4. **Version:** patch bump only (2.13.1 to 2.13.2); the spec minor goes to 1.51.0 for the new row.

(The original questions follow, for the record.)

## Open questions (answered)

1. **Where does `>` go?** A name prefix inside the name text would push shared names one cell to the right of
   every other name, so the column would no longer line up. Options:
   - **(Recommended)** a third cell in the marker prefix (`*`, label number, `>`): all names move right by one cell
     for every row; the name column loses one character. Never misaligned.
   - Replace the second marker cell (the label number), shared folder wins over a label: no layout change, but a
     labelled shared folder hides its label.
   - The name text itself gets the `>` (`>Projects`): exactly what you wrote, but only shared names shift right.
2. **Admin shares (`C$`, `ADMIN$`):** my plan hides them (they would mark `Windows` and every drive root, and
   Explorer doesn't mark them). OK? 
3. **Polling every 3 seconds:** fine, or refresh only after Share/Unshare and on directory refresh (cheaper but
   misses shares made in Explorer until the next refresh)?
4. **Version:** just a patch bump (2.13.2) with the spec minor bump 1.51.0 for `LIST-013`.

## Out of scope

- Showing the share name or permissions in the list (the Unshare dialog already lists name and access).
- Sharing from a provider panel (still refused, `RefuseInsideProvider`).
- Making Share Folder work without UAC (SEC-003 stays: one explicit, confirmed, elevated operation).

## Status

Planned 2026-10-16. **Implemented 2026-10-16** (spec 1.51.0, version 2.13.2) with the four decisions above.

Differences from the plan:

- The config key for the period is `shareRefreshSeconds` (0-3600, default 3); 0 disables the timer.
- The panel-refresh trigger uses `Panel::RefreshStarts()`, like LIST-010's invalidation.
- `EnumerateSharesFor` now skips administrative shares as well.
- Tests: `NetSharesTests` +6 cases, `ConfigTests` +1; full suite 1,048 cases, 18,028 checks, 0 failed (Debug and Release).

**Not verified here:** the real UAC share/unshare round trip and the look of the `>` (manual test MT-068).
