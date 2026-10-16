# Fix Plan — IS-0014

Source: `AIPrompt/issue_fix_requests.md`

> The Zip plugin do every time the full zipp access/process (repack), it is a weakness.
> It would be nice to update the zip only when we want to close, step back through '..'.
> In this case the software should ask a confirmation to be save the modifications or not.

## Summary

Confirmed by reading `Source/ZipPlugin/ZipPlugin.cpp` directly: every one of the archive browser's
five mutating operations — `AddFilesFromPanel` (`Shift+Insert`/F5-receive), `ChangeArchiveAttributes`
(`Ctrl+A`), `DeleteFromArchive` (`F8`/`Delete`), `CreateDirectoryInArchive` (`F7`), and
`ExtractOrMove`'s Move branch (`F6`, removing moved entries) — independently builds a `replacement`
copy of `view.entries` and calls `WriteArchiveEntries` **immediately**, which rebuilds the *entire*
archive from scratch (`mz_zip_writer_add_mem` for every single entry) and rewrites the whole file to
disk. For a large archive or a batch of several small edits, this repacks the whole thing every time,
exactly the weakness reported.

**Fix:** track an in-memory `dirty` flag on `ArchiveView`; every mutating operation updates
`view.entries` and sets `dirty = true` but no longer writes to disk itself. The actual save happens
only when leaving the archive (`LeaveOrUp` at the root level — the only "close" this plugin has), at
which point — if dirty — the user is asked to save, discard, or cancel, exactly as the issue
requests. Two additional guards are needed once writes are deferred, to avoid turning a performance
fix into a new data-loss footgun: `Ctrl+R` (reload) must not silently discard pending changes, and
quitting the whole app (`F10`) while an archive has unsaved changes needs a safety net.

## Design

### 1. `ArchiveView` gains a dirty flag

```cpp
struct ArchiveView {
    ...
    bool dirty = false;  // IS-0014: true once view.entries has unsaved changes
};
```

### 2. Two small helpers replace the repeated "write, then assign" pattern

```cpp
// Updates the in-memory listing and marks it unsaved -- no disk write here.
void ApplyArchiveChanges(ArchiveView& view, std::vector<ArchiveEntry> replacement) {
    view.entries = std::move(replacement);
    view.dirty = true;
}

// The one place an actual repack-and-write now happens, outside of Pack/
// Unpack's own one-shot operations. No-op (returns true) if nothing is
// pending, so callers can call this unconditionally.
bool SaveArchiveIfDirty(ArchiveView& view, ArchiveProgress* existing = nullptr) {
    if (!view.dirty) return true;
    if (!WriteArchiveEntries(view.path, view.entries, existing)) return false;
    view.dirty = false;
    return true;
}
```

### 3. Every mutating operation defers the write

Each of `ChangeArchiveAttributes`, `DeleteFromArchive`, `CreateDirectoryInArchive`,
`AddFilesFromPanel`, and `ExtractOrMove`'s Move branch currently ends with:

```cpp
if (WriteArchiveEntries(view.path, replacement, ...)) { view.entries = std::move(replacement); ... }
else { ... "Could not save" message ...; return; }
```

Replace with:

```cpp
ApplyArchiveChanges(view, std::move(replacement));
```

— dropping each operation's own "could not save" failure path entirely (there's no write at that
point to fail); the only place a save can now fail is the single deferred `SaveArchiveIfDirty` call
(§5). Each operation's existing success message (e.g. "Added 3 item(s) to the archive.") should note
the pending state, e.g. "Added 3 item(s) to the archive (not yet saved)." — small wording change,
same call sites.

### 4. `Ctrl+R` (reload) must not silently discard pending changes

`ReloadArchiveFromDisk` currently reloads unconditionally. Once edits can sit unsaved in memory, a
reload would silently throw them away. Add a guard:

```cpp
void ReloadArchiveFromDisk(ArchiveView& view) {
    if (view.dirty) {
        const wchar_t* lines[] = {L"This archive has unsaved changes -- reloading discards them."};
        McDialogOption options[] = {{L'Y', L"Reload anyway"}, {L'N', L"Cancel"}};
        if (g_host->ShowChoice(L"Refresh", lines, 1, options, 2) != 0) return;
    }
    if (LoadArchive(view.path, view)) {
        view.selected.clear();
        view.dirty = false;  // a fresh read from disk; any prior pending edits are gone
        LogInfo(...);
    } else { ... unchanged ... }
}
```

### 5. `LeaveOrUp`: the actual confirmation-to-save, at the archive's root level

The existing subtree-navigation branch (`if (!view.folder.empty()) { ...; return; }` — going up one
folder *within* the archive) is unchanged; it was never a "close." Only the root-level branch (the
one that actually leaves the archive) changes:

```cpp
void LeaveOrUp(ArchiveView& view) {
    if (!view.folder.empty()) { ...unchanged... return; }
    if (!view.dirty) {
        // Unchanged from today -- ZIP-007's own plain "Leave archive?" Yes/No.
        std::wstring question = L"Leave " + view.title + L"?";
        const wchar_t* lines[] = {question.c_str()};
        McDialogOption options[] = {{L'Y', L"Yes"}, {L'N', L"No"}};
        if (g_host->ShowChoice(L"Leave archive", lines, 1, options, 2) == 0) {
            g_host->ReleasePanelContent(view.side);
            view = ArchiveView{};
        }
        return;
    }
    // IS-0014: dirty -- ask whether to save, matching the issue's own wording.
    const wchar_t* lines[] = {L"This archive has unsaved changes."};
    McDialogOption options[] = {{L'Y', L"Save and leave"}, {L'N', L"Discard and leave"}, {L'C', L"Cancel"}};
    int choice = g_host->ShowChoice(L"Leave archive", lines, 1, options, 3);
    if (choice == 0) {
        if (!SaveArchiveIfDirty(view)) {
            const wchar_t* err[] = {L"Could not save the archive -- staying open so nothing is lost."};
            g_host->ShowMessage(L"Leave archive", err, 1);
            return;  // stay open, still dirty, let the user retry or discard next time
        }
        g_host->ReleasePanelContent(view.side);
        view = ArchiveView{};
    } else if (choice == 1) {
        g_host->ReleasePanelContent(view.side);
        view = ArchiveView{};  // discard -- the on-disk file was never touched, nothing to undo
    }
    // choice == 2 or Esc (-1): Cancel -- stay, change nothing.
}
```

A failed save deliberately keeps the archive open rather than leaving anyway — losing the user's
edits because of a transient write failure (disk full, file locked) would be strictly worse than
just not optimizing this operation at all.

### 6. Safety net: auto-save on app quit (`F10`)

The issue's own wording ("when we want to close, step back through '..'") covers leaving the
archive, not quitting the whole app — but once writes are deferred, pressing `F10` while an archive
has unsaved changes would silently lose them, a new footgun this fix would otherwise introduce.
There's no existing "may I block/confirm shutdown" hook in the plugin ABI, and adding one is bigger
than this issue asks for — instead, make `McPlugin_Shutdown` (already called on every exit,
`g_host` still valid at entry) opportunistically auto-save, best-effort, no prompt (a dialog during
teardown is riskier than it's worth):

```cpp
extern "C" __declspec(dllexport) void __stdcall McPlugin_Shutdown(void) {
    for (auto& view : g_views) {
        if (view.open && view.dirty) {
            if (WriteArchiveEntries(view.path, view.entries))
                LogInfo(L"Archive browser: auto-saved \"" + view.path + L"\" on shutdown.");
            else
                LogError(L"Archive browser: could not auto-save \"" + view.path + L"\" on shutdown -- unsaved changes may be lost.");
        }
    }
    g_host = nullptr;
}
```

### 7. Optional, low-risk nicety: a visible "unsaved" marker

`DrawArchive`'s own header row (`FormatArchiveLine(columns, L"", L"Name", L"Size", L"Packed",
L"Attr")`) could show e.g. `"Name *"` when `view.dirty` — directly addresses "the user should be
able to tell there's something to save," entirely within the plugin's own `PanelDrawText` calls, no
host/SDK change needed. Flagged as optional polish, not required for the core fix.

## Files touched

- `Source/ZipPlugin/ZipPlugin.cpp` only — `ArchiveView`, the two new helpers, all five mutating
  operations, `ReloadArchiveFromDisk`, `LeaveOrUp`, `McPlugin_Shutdown`, and (if included) `DrawArchive`.
  No host (`PluginManager.{h,cpp}`) or SDK (`McPluginAbi.h`/`McPluginHost.h`) changes at all — every
  piece needed (`ShowChoice`, `ShowMessage`, `WriteArchiveEntries`, `LoadArchive`) already exists.
- `README.md` — the ZIP archives Status bullet should mention changes are now batched and saved on
  leaving (with a confirmation), not after every edit.
- `AIPrompt/my-commander.md` — likely a new `ZIP-010` row (no existing requirement covers deferred
  saving) and a version-history paragraph.

## Test plan

No automated coverage applies — consistent with every other ZipPlugin fix this project has made
(`Source/ZipPlugin/` is a standalone project outside `MyCommanderTests`). Manual verification:

- Make several edits (add a file, toggle an attribute, create a directory) without leaving; confirm
  only one "Saving ZIP archive" progress dialog appears, at leave time, not one per edit.
- Leave with pending changes: confirm the three-way prompt appears; verify **Save and leave** writes
  and exits, **Discard and leave** exits without writing (re-enter and confirm the discarded edits
  are gone), **Cancel** stays in the archive with the edits still pending.
- Press `Ctrl+R` with pending changes: confirm the discard-confirmation appears before reloading;
  cancelling it leaves the pending edits intact.
- Make an edit, then quit the whole app (`F10`) without leaving the archive first; relaunch and
  confirm the change was auto-saved.
- Leave a *clean* (no pending changes) archive and confirm the plain "Leave archive?" Yes/No prompt
  is unchanged from today (ZIP-007 still satisfied as originally implemented).

## Risk / scope notes

- This changes the shape of the "every operation writes to disk" contract into "every operation
  marks dirty, one deferred write" across five existing call sites — each individual change is
  small and mechanical (replace an immediate write+assign with just assign+flag), but it's five
  call sites, not one.
- A crash between an edit and the next save (leave, or the shutdown safety net) still loses
  in-memory changes — an inherent, accepted trade-off of batching, and exactly what trading
  immediate-durability for repack performance means. The `F10` auto-save (§6) closes the most
  common version of this gap (a normal quit), not every possible one (a crash/kill).
- A failed save at leave-time keeps the archive open rather than discarding or exiting anyway (§5) —
  deliberately conservative, since losing edits to a transient write failure would be worse than the
  performance problem this issue is about.

## Status

**Implemented** (2026-10-02), per the design above, with no deviations:

- `ArchiveView::dirty`, `ApplyArchiveChanges`, `SaveArchiveIfDirty` landed as designed. All five
  mutating operations (`ChangeArchiveAttributes`, `DeleteFromArchive`, `CreateDirectoryInArchive`,
  `AddFilesFromPanel`, `ExtractOrMove`'s Move branch) now defer the write. `DeleteFromArchive`'s own
  `ArchiveProgress` object was removed entirely along with the immediate write, since an in-memory
  erase needs no progress UI.
- `ReloadArchiveFromDisk` (`Ctrl+R`) and `LeaveOrUp` both gained the designed confirmation prompts.
- `McPlugin_Shutdown` auto-saves any open, dirty archive on app quit, as designed.
- The optional `*`-after-"Name" header marker (§7) was included too — cheap and self-contained, no
  host/SDK change needed.
- New `ZIP-010` requirement added to `AIPrompt/my-commander.md` (`Implemented`).
- Verified via `MyCommander.sln` Debug|x64 build (0 errors, including the nested Release
  installer-payload rebuild, which now also rebuilds `ZipPlugin.vcxproj` per IS-0013) and the
  standalone `ZipPlugin.vcxproj` Debug|x64 rebuild (0 errors), plus `MyCommanderTests.exe`: 425 test
  cases, 11,795 checks, 0 failed (unchanged — no automated coverage applies here, as planned).
- Full interactive verification (confirming only one repack happens across several edits, exercising
  all three leave-prompt choices, the `Ctrl+R` discard-confirmation, and the shutdown auto-save)
  remains outstanding — the same standing no-interactive-console-capture limitation noted
  throughout this project.

See `3_Reports/ChangeLogs/changelog_2026-10-02.md`'s IS-0014 entry for the full file list.
