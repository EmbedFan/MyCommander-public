# Fix Plan — IS-0001

Source: `AIPrompt/issue_fix_requests.md`

> When we create an empty file, after the file creation and panel refreshing the new [item]
> shall be selected. The same request with create directory. Now [this] does not happen.

## Summary

After `F7` (create directory) or `Shift+F7` (create an empty file), the panel refreshes but
the cursor stays wherever it was before the operation — it does not move to the item that was
just created. Requested behavior: the newly created directory/file should end up selected
(cursor on it) once the panel redraws.

This affects **FOP-004** (create directory) and **FOP-005** (create file), both in `Source/MyCommander/main.cpp`.

## Root cause

`DoMakeDirectory` and `DoCreateNewFile` (`main.cpp`) both end with a bare `target.Refresh()`
after the create call:

```cpp
auto result = CreateNewDirectory(target.Path(), *nameOpt);
target.Refresh();
```

`Panel::Refresh()` takes no arguments. Its only cursor-preservation behavior is remembering
*whichever entry the cursor was already on* before the refresh and trying to land back on that
same name afterward (LIST-005) — it has no way to be told "select this other, newly-created
name instead." So after creating `newfolder`, the cursor stays wherever it was (e.g. on `..`
or on whatever entry happened to be selected before `F7` was pressed), not on `newfolder`.

`Panel` already has a mechanism for "navigate/refresh and land on a specific name" —
`NavigateTo(dir, selectName)` / the private `SetLocation(dir, selectName)` — used today by
`DoSearch` to jump the panel to a chosen search result and select it. `DoMakeDirectory`/
`DoCreateNewFile` simply never call this; they only call the no-argument `Refresh()`. That
alone looks like the whole fix (swap `target.Refresh()` for
`target.NavigateTo(target.Path(), *nameOpt)`), **but there is a second, deeper bug** that
would make that swap silently not work for the app's real panels — see below.

### Second, deeper bug: `selectName` doesn't survive incremental (`LIST-008`) refresh

Both of the app's live panels are constructed with `incrementalLoading = true`
(`main.cpp`'s `wmain`: `Panel left(leftStart, config.groupDirectoriesFirst,
/*incrementalLoading=*/true);`, same for `right`). `Panel`'s default (`incrementalLoading =
false`, `Panel.h`) is a synchronous, single-call `Refresh()` — that path is only exercised by
tests that construct a bare `Panel(path)`, never by the shipped app.

Trace `SetLocation(dir, selectName)` (`Panel.cpp`) for an **incremental** panel:

```cpp
void Panel::SetLocation(const fs::path& dir, const std::wstring& selectName) {
    path_ = dir;
    Refresh();                      // <-- for an incremental panel, this only *starts* the load
    if (!selectName.empty()) {
        auto found = std::find_if(entries_.begin(), entries_.end(), ...);   // entries_ is still
        ...                                                                  // empty/partial here
    }
}
```

`Refresh()` immediately delegates to `BeginRefresh()` when `incrementalLoading_` is true, and
`BeginRefresh()` sets `loading_ = true` and returns right away — the directory listing is
filled in gradually over subsequent `Panel::PumpRefresh()` calls from the main input loop, not
by the time `Refresh()`/`BeginRefresh()` returns. So `SetLocation`'s `find_if` over `entries_`
runs against a list that's still empty (or only partially populated), can never find
`selectName`, and the cursor silently stays wherever `FinishRefresh()`'s own restore logic
(`refreshCursorName_`, filled from the pre-refresh cursor position in `BeginRefresh()`, *not*
from `selectName`) leaves it once loading actually finishes.

**Consequence:** `SetLocation`'s `selectName` parameter is effectively dead code for both of
the app's real panels today. This means:

- Fixing IS-0001 by only changing `DoMakeDirectory`/`DoCreateNewFile` to call
  `target.NavigateTo(target.Path(), *nameOpt)` would compile, look right, but **not actually
  select the new item** in the running app — `Panel` itself needs to learn to carry
  `selectName` across an in-progress incremental refresh.
- This same gap already affects `DoSearch`'s jump-to-result (`target.NavigateTo(dir,
  selectName)`, `main.cpp` line ~954, SRC-002's "navigable in a panel") whenever the target
  directory needs a real listing pass — pressing `Enter` on a search result likely does not
  reliably land the cursor on the exact matched entry today either. Not something IS-0001
  itself asked about, but the same root cause and the same fix covers it — worth fixing
  together rather than patching IS-0001's symptom only and leaving this live.
- It's not caught by the existing automated suite because `PanelTests.cpp` constructs plain
  `Panel(path)` instances (`incrementalLoading` defaults to `false`), which take the
  synchronous `Refresh()` path where `SetLocation`'s `find_if` runs *after* the listing is
  already complete — so `selectName` happens to work in every existing test, just not in the
  app itself.

## Proposed fix

1. **`Panel.h`/`Panel.cpp`: give incremental refresh a real "select this name" input**,
   distinct from the "preserve current cursor" fallback `refreshCursorName_` already provides:
   - Add a field (e.g. `std::wstring pendingSelectName_;`) that `SetLocation` sets *before*
     calling `Refresh()`, instead of trying to search `entries_` itself right after.
   - `BeginRefresh()`: if `pendingSelectName_` is non-empty, seed `refreshCursorName_` from it
     (taking priority over the pre-refresh cursor's own name) and clear `pendingSelectName_`;
     otherwise keep today's behavior (remember the current cursor's name).
   - `Refresh()`'s synchronous (non-incremental) branch needs the same priority rule, so the
     existing default-`Panel` test path keeps working: prefer `pendingSelectName_` over the
     pre-refresh cursor name when both are present, then clear it.
   - Remove `SetLocation`'s own now-redundant/broken `find_if` block — the rename-and-restore
     logic already handles it uniformly for both the sync and incremental paths once the above
     lands.
2. **`main.cpp`: `DoMakeDirectory`/`DoCreateNewFile`** — replace the bare `target.Refresh();`
   with `target.NavigateTo(target.Path(), *nameOpt);`. Because `dir == path_` in this call,
   `NavigateTo`'s existing same-directory branch (`if (dir == path_) { SetLocation(dir,
   selectName); return; }`) applies — no back/forward-history entry is created, matching plain
   `Refresh()`'s current behavior; only the cursor-selection outcome changes. Do this for both
   the success and (existing) failure paths consistently — i.e. keep selecting the new name
   even though `ShowMessage` also runs on failure, so partial/renamed-on-conflict results (if
   `CreateNewDirectory`/`CreateNewFile` ever start supporting that) still land the cursor
   sensibly; today's behavior on failure is unaffected either way since nothing new was
   created.
3. Leave `DoRename`'s `target.Refresh()` alone for now — out of scope for this issue (IS-0001
   only asks about create-file/create-directory), though the same "select the item that just
   changed" idea would apply there too if ever requested separately; noting it so it isn't
   independently rediscovered as a surprise later.

## Files touched (implementation phase, not yet made)

- `Source/MyCommander/Panel.h` — new private field, doc comment.
- `Source/MyCommander/Panel.cpp` — `SetLocation`, `BeginRefresh`, `Refresh`, `FinishRefresh` as needed.
- `Source/MyCommander/main.cpp` — `DoMakeDirectory`, `DoCreateNewFile`.

## Test plan

Existing `PanelTests.cpp` only exercises the non-incremental constructor, which already
happens to make `selectName` "work" today (masking the real bug) — new tests must exercise
`incrementalLoading = true` explicitly to actually catch a regression here:

1. **New regression test** (incremental panel): construct `Panel(dir, ..., /*incrementalLoading=*/true)`,
   call `NavigateTo(samePath, "newname")` for a name that doesn't exist yet, create the file on
   disk, pump refresh to completion (`PumpRefresh` in a loop until `!IsLoading()`), assert
   `Cursor()` lands on `"newname"`. Repeat for a name that already existed before the call, to
   confirm the "select a specific existing entry after an incremental refresh" path also works
   (this is the same mechanism `DoSearch` relies on).
2. **Regression coverage for the masked-by-sync-path gap**: an incremental-panel test that
   calls `NavigateTo` with `selectName` pointing at an entry, but where the refresh takes more
   than one `PumpRefresh` batch to complete, confirming the selection still lands correctly
   once `FinishRefresh()` runs (not just when the whole directory fits in one batch).
3. **`FileOpsTests.cpp`/`PanelTests.cpp` integration-style check**: create a directory (or
   file) via `CreateNewDirectory`/`CreateNewFile`, then call the same `NavigateTo(path,
   name)` sequence `DoMakeDirectory`/`DoCreateNewFile` will use, and assert the created name is
   selected — as close as an automated test gets to the actual `main.cpp` code path without a
   live console (per this project's established limit: `main.cpp`'s own functions have no
   direct test coverage — see ACC-001/ACC-002/TST-005's precedent in
   `3_Reports/ChangeLogs/`).
4. Manual smoke test after the fix (no automated coverage exists for `main.cpp` itself): press
   `F7`, type a name, Enter — confirm the new directory is highlighted; repeat with `Shift+F7`
   for a file. Confirm `Alt+F7` → `Enter` (jump to a search result) also lands correctly, as a
   check that the shared fix didn't regress that path.

## Risk / scope notes

- Low risk, contained to `Panel`'s refresh bookkeeping and two `main.cpp` call sites; no
  change to `FileOps`, no change to any file-system behavior.
- The `DoSearch` jump-to-result fix is a **side effect of fixing the shared root cause**, not
  scope creep bolted on separately — flagging it here so it's an intentional, documented part
  of this fix rather than something that looks unrelated in the eventual diff.
- No config/spec/requirement-ID status change is implied by this fix by itself; if/when
  implemented, update `AIPrompt/my-commander.md`'s FOP-004/FOP-005 rows only if their current
  `Implemented` status commentary needs correcting (check current wording before editing).

## Status

**Implemented** (2026-09-24). Followed the plan as written:

- `Panel.h`/`Panel.cpp`: added `pendingSelectName_`; `SetLocation` sets it before calling
  `Refresh()` instead of searching `entries_` itself afterward; `BeginRefresh()` and
  `Refresh()`'s synchronous branch both consume it (taking priority over the pre-refresh
  cursor name) and clear it; `FinishRefresh()` needed no change — it already read
  `refreshCursorName_`, which the above now seeds correctly.
- `main.cpp`: `DoMakeDirectory`/`DoCreateNewFile` now call `target.NavigateTo(target.Path(),
  *nameOpt)` instead of the bare `target.Refresh()`.
- `DoRename` left untouched, as scoped.
- Added 4 `PanelTests.cpp` cases (`_IS0001` suffix) covering: selecting a newly created name,
  selecting a pre-existing name (the `DoSearch` case), selection surviving a multi-batch
  incremental refresh, and confirming the no-`selectName` "preserve cursor" behavior still
  works — all using `incrementalLoading=true`, the mode the existing suite didn't exercise for
  this path. Full suite: 272 cases / 10,913 checks / 0 failed (Debug and Release).
- No `FileOps`-level integration test or automated coverage of `main.cpp`'s two call sites
  themselves — consistent with this project's established limit (`main.cpp` has no direct test
  coverage). Manual verification of `F7`/`Shift+F7`/`Alt+F7`→`Enter` in the running app is
  still outstanding (no interactive-console capture available in this environment); the
  `Panel`-level tests exercise the exact same code path those call, which is the strongest
  automated verification available here.
- Documented in `AIPrompt/my-commander.md`'s version-history paragraph (0.41 -> 0.42) and
  `3_Reports/ChangeLogs/changelog_2026-09-24.md`. No `Implemented`/Disposition change to
  FOP-004/FOP-005 — this was a bug fix within an already-shipped feature.
