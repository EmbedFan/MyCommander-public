# Fix Plan — IS-0012

Source: `AIPrompt/issue_fix_requests.md`

> App
>     Add refresh panel using shortcut, that can trigger the plugin panel refresh call
> Zip plugin:
>     Implement create directory

## Summary

Two independent asks filed together under one issue:

1. **A host-wide "Refresh panel" shortcut** (`Ctrl+R`) that also reaches a plugin's own panel
   content — no new SDK surface needed, since the host's existing key-routing already sends every
   key but `Tab`/`F1`/`F10` straight to a plugin's `OnPanelKey` while its content is active; the
   fix is picking/documenting the key for the real-panel case and having `ZipPlugin` itself honor
   it for its own archive-browsing content.
2. **"Create directory" inside the ZIP archive browser** (`F7`) — not implemented at all today.
   Implemented as a genuine, persisted empty directory (a zero-byte entry whose name ends in `/`,
   the universal zip convention miniz's own reader already recognizes), not a purely virtual
   folder that would vanish if the user never adds a file to it.

## Part 1 — Refresh-panel shortcut (Ctrl+R)

### Current behavior

No dedicated "refresh/reread" key exists anywhere. `Panel::Refresh()` is only ever called
internally, after an operation completes. Confirmed `Ctrl+R` is completely unclaimed: no
`case 'R':` in `main.cpp`'s key switch, not in `BuiltinShortcutInventory()`
(`PluginManagerLogic.cpp`), not a menu mnemonic anywhere in `MenuBar.h` (menu mnemonics are
`Alt+letter`, a different modifier, so no collision risk there regardless).

`main.cpp`'s input loop already does, before its own switch runs:

```cpp
if (target.Content() && key != VK_TAB && key != VK_F10 && key != VK_F1) {
    target.Content()->OnKey(key, record.Event.KeyEvent.dwControlKeyState);
    continue;
}
```

So whichever key is picked for "Refresh" already reaches a plugin's own `OnPanelKey` untouched
whenever that panel's content is active — the host's own new `case` for it only ever runs for a
normal (real-filesystem) panel. No new `McPluginHost`/`McPlugin_*` export needed, unlike the
IS-0011 follow-up's `McPlugin_OnReceiveFiles` (that one was needed because the key is pressed on
the *other* panel, not the content-holding one).

### Design

1. Add `{static_cast<WORD>(L'R'), true, false, false}` to `BuiltinShortcutInventory()`
   (`PluginManagerLogic.cpp`) so no plugin can claim `Ctrl+R` as its own global shortcut.
2. Add `case 'R':` to `main.cpp`'s key switch, matching the existing per-letter pattern
   (`'F'`/`'A'`/`'H'`): `if (ctrlHeld) target.Refresh(); else appendToCommandLine(...)`.
3. Document `Ctrl+R` in `Help.cpp`'s shortcut reference and `README.md`'s keybindings table.
4. Add a short note in `DEVELOPER_GUIDE.md`'s "Panel content" section: `Ctrl+R` reaches a
   plugin's own `OnPanelKey` like any other key, and content backed by something that can go
   stale (data read from disk, e.g.) should consider handling it as this project's own
   conventional "Refresh" key.
5. In `ZipPlugin.cpp`'s `HandleArchiveKey`, handle `Ctrl+R` by re-running
   `LoadArchive(view.path, view)` (already exists, already used by `McPlugin_EnterArchive`) — on
   success, clear `view.selected` (the entry set may have changed) and log it; on failure (file
   deleted/corrupted externally), show a message but leave the existing in-memory listing intact,
   matching `LoadArchive`'s own existing failure behavior (it only clears `view.entries` *after*
   confirming the read succeeded). Placed before the `items.empty()` early return — same
   reasoning as `Shift+Insert` — refreshing an empty/stale folder is exactly when you'd want it.
6. Add the line to the plugin's own `McPlugin_GetHelpMarkdown`.

### Files touched

- `Source/MyCommander/PluginManagerLogic.cpp` — `BuiltinShortcutInventory()`.
- `Source/MyCommanderTests/PluginManagerTests.cpp` — extend
  `BuiltinShortcutInventory_IncludesKnownAppShortcuts` with a `Ctrl+R` check.
- `Source/MyCommander/main.cpp` — new `case 'R':`.
- `Source/MyCommander/Help.cpp`, `README.md` — keybindings documentation.
- `Source/MyCommanderPluginSdk/DEVELOPER_GUIDE.md` — "Panel content" section note.
- `Source/ZipPlugin/ZipPlugin.cpp` — `HandleArchiveKey`'s new `Ctrl+R` branch, help markdown.

### Test plan

`BuiltinShortcutInventory`'s new entry is covered by the extended existing test case. The rest
(real key dispatch, archive reload) is real-OS-side-effect/console-interactive behavior with no
existing headless test precedent in this project (matching `Console::SetTitle`'s own precedent
from IS-0009) — manual verification: `Ctrl+R` on a normal panel re-reads its directory; `Ctrl+R`
while browsing an archive reloads it from disk (e.g. edit the underlying `.zip` externally, then
refresh and confirm the listing updates).

## Part 2 — "Create directory" inside the ZIP archive browser (F7)

### Current behavior

`VK_F7` isn't handled anywhere in `HandleArchiveKey` — pressed while browsing an archive, it
silently does nothing (the host's own `DoMakeDirectory` never runs, since content-panel key
routing intercepts the key first, same mechanism as Part 1 above).

### Design

Confirmed via `third_party/miniz/miniz.c:4277-4292`
(`mz_zip_reader_is_file_a_directory`) that miniz's reader determines "is this entry a directory"
purely from the entry **name ending in `/`** — the universal zip convention, not anything specific
to this codebase. So a zero-byte entry named e.g. `"newdir/"` round-trips correctly as a real
directory through miniz (and any other zip tool) — making a genuine "create an empty directory
that survives closing and reopening the archive" possible, rather than a purely virtual folder
that vanishes the moment nothing has been added to it.

This touches a few existing spots that currently assume no explicit directory entries ever exist
(the project's own prior documented convention — `ExtractZipArchive`'s own comment: "directories
are implied by their own entries' paths"):

1. **`LoadArchive`** currently does `if (... || stat.m_is_directory) continue;`, unconditionally
   discarding any explicit directory entry on load. Change it to instead load a lightweight
   zero-byte `ArchiveEntry` (name forced to end in `/`) for a directory entry, so one created this
   way survives a save → close → reopen round trip. `CurrentItems` needs **no change** — its
   existing slash-detection logic already treats any entry whose name contains (or ends in) `/`
   as implying a directory, so a directory-marker entry is discovered exactly like any nested
   file already is.
2. **`ExtractZipArchive`** (the full on-disk "Unpack Package" path) has the same
   `if (stat.m_is_directory) continue;` — without a fix, unpacking an archive containing one of
   these new empty directories to real disk would silently drop it. Change it to call the
   already-existing `EnsureDirectoryExists` on the entry's mapped destination path instead of
   skipping it.
3. **`ComputeArchiveStats`** (IS-0011's "Archive info", operating on already-loaded
   `ArchiveEntry`s) currently counts every entry as a file unconditionally — once (1) makes
   directory markers appear in `view.entries`, this would wrongly inflate the file count. Skip
   entries whose name ends in `/` from the file/byte counts (they already contribute to the
   directory count via the existing `RegisterDirectoryPrefixes`, no change needed there).
   `AccumulateArchiveStatsFromFile` (the disk-reading path) already handles this correctly — no
   change needed there.
4. **New `CreateDirectoryInArchive(ArchiveView&)`**: `ShowTextPrompt` for a name, validate
   (non-empty, no `/`, no `..`), reject a collision with an existing *file* of the same name (an
   existing directory of the same name is a harmless no-op/merge), append a zero-byte
   directory-marker entry via the same `replacement`/`WriteArchiveEntries` pattern every other
   mutating operation here already uses, move the cursor onto the new entry afterward (mirroring
   IS-0001's "a newly created item is selected" convention), show a success/failure message.
5. Wire `VK_F7` in `HandleArchiveKey` to call it, placed before the `items.empty()` early return
   (creating the first directory in an empty archive/folder is the common case).
6. Add it to the plugin's help markdown and `README.md`.

### Files touched

- `Source/ZipPlugin/ZipPlugin.cpp` — `LoadArchive`, `ExtractZipArchive`, `ComputeArchiveStats`,
  new `CreateDirectoryInArchive`, `HandleArchiveKey`'s new `F7` branch, help markdown.
- `README.md` — ZIP archives Status bullet.

### Test plan

No automated test coverage exists for any of `ZipPlugin.cpp`'s own archive-browsing logic today
(it's a standalone plugin project outside `MyCommanderTests`, consistent with every other
ZIP-plugin fix this session) — manual verification: create a directory, confirm it appears and can
be navigated into/out of; close and reopen the archive, confirm it's still there; unpack the whole
archive via "Unpack Package" and confirm the empty directory is created on disk; run "Archive
info" and confirm the new directory counts toward `Directories`, not `Files`.

## Risk / scope notes

- **Part 1** is small and additive — one new key, no behavior change for any existing key, and the
  plugin-reload branch degrades gracefully (shows a message, keeps the stale listing) rather than
  risking data loss on a failed reload.
- **Part 2** changes the *meaning* of `stat.m_is_directory` in two existing, previously-reachable
  code paths (`LoadArchive`, `ExtractZipArchive`) that currently assume it never matters — reviewed
  both call sites above specifically to confirm no downstream code (`CurrentItems`,
  `WriteArchiveEntries`, `ComputeArchiveStats`) silently mishandles a zero-byte,
  trailing-slash-named `ArchiveEntry` once it starts actually appearing in `view.entries`.
- No part of this plan touches `FileOps`, `Elevation`, or any destructive real-filesystem
  operation beyond what `ExtractZipArchive` (unpacking to disk) already does.

## Status

**Implemented** (2026-10-02), per the design above, with one addition found necessary during
implementation:

- **Part 1 (Ctrl+R):** landed exactly as designed.
- **Part 2 (F7 create directory):** landed exactly as designed, plus one extra fix not called out
  in the original plan: `ExtractOrMove` (F5/F6 extract-from-archive) also needed to recognize a
  directory-marker entry and just call `EnsureDirectoryExists` rather than attempt `WriteFileBytes`
  on it — without this, extracting a selection that included a created-via-F7 directory would have
  produced a spurious "Could not write destination" error for that entry. Found and fixed during
  implementation, not predicted by the original plan's "touches `LoadArchive`/`ExtractZipArchive`/
  `ComputeArchiveStats`" list.
- New requirement IDs added to `AIPrompt/my-commander.md`: `NAV-011` (Refresh shortcut), `ZIP-009`
  (Create directory + reload), both `Implemented`.
- Verified via `MyCommander.sln` Debug|x64 build (0 errors) and the standalone `ZipPlugin.vcxproj`
  Debug|x64 rebuild (0 errors), plus `MyCommanderTests.exe`: 425 test cases, 11,795 checks, 0
  failed (up from 11,794 — the 1 new `Ctrl+R` inventory check). Full interactive verification
  (actually pressing F7/Ctrl+R through the running UI) remains outstanding, the same standing
  no-interactive-console-capture limitation noted throughout this project.

See `3_Reports/ChangeLogs/changelog_2026-10-02.md`'s IS-0012 entry for the full file list.
