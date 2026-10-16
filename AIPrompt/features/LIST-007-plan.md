# Implementation Plan — LIST-007 (automatic panel refresh on external changes)

Source: `AIPrompt/my-commander.md` §8:

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| LIST-007 | Background monitoring may refresh a panel after external file-system changes. | P2 | LATER | Not Started |

## Implementation check (2.0.0.368)

- **Change notifications already exist, but only for two narrow cases:**
  - IS-0002 watches the folder of a file being edited in an external editor (`PendingEdit`,
    `FindFirstChangeNotificationW` with `LAST_WRITE|SIZE`, polled in `PollPendingEdits`, `main.cpp:3036`);
  - IS-0006 watches the config file (`ConfigWatch`).

  Both are polled from the main loop, which wakes at least every 16 ms
  (`WaitForSingleObject(console.InputHandle(), 16)`, `main.cpp:4867`). A watcher can therefore be polled without any
  extra thread.
- **`Panel::Refresh()`** (`Panel.cpp:85`) re-lists the folder and keeps the cursor on the same name (LIST-005), with
  incremental loading for main-screen panels (LIST-008). It does **not keep the selection**: entries are rebuilt with
  `selected = false`. Today that's acceptable, because a refresh is user-initiated or follows an operation the user
  just did. An automatic refresh must not silently drop the user's selection.
- **NAV-012** already refreshes the other panel when both show the same folder.
- **Plugin panels** (ZIP/FTP/SFTP/tree) show content, not the real folder. The real folder underneath may change;
  it's refreshed when the content is released.

## Design

### D1 — One watcher per panel (`FolderWatch`, `Source/MyCommander/FolderWatch.{h,cpp}`)

- **What it watches:** `FindFirstChangeNotificationW(path, FALSE,
  FILE_NAME | DIR_NAME | ATTRIBUTES | SIZE | LAST_WRITE)`, not recursive. That covers the folder's own entries:
  created, deleted or renamed files and folders, size, time and attribute changes.
- **No `ReadDirectoryChangesW`:** a full refresh of one folder is already cheap and incremental (LIST-008). A
  "something changed" signal is enough and much simpler; per-file deltas aren't needed.
- **Lifetime:** re-armed whenever the panel's path changes (`NavigateTo`, history, drive). It is closed while a plugin
  or tree content is installed, and re-armed on release.
- **Unavailable notifications** (some network shares, FAT on removable media, `\\?\` paths over the limit): the
  watcher reports `Unavailable`, and the panel falls back to **no automatic refresh**. That's the current behaviour,
  with an optional slow poll (D3). The status line never lies about it.

### D2 — Debounced, state-keeping refresh

- **Debounce:** a signal arms a **300 ms quiet timer**, re-armed on further signals and capped at 2 s of continuous
  changes, so a large copy into the folder refreshes about once every 2 s, not 1,000 times. Then
  `Panel::RefreshPreservingState()` runs.
- **`RefreshPreservingState()`** is new and tested. It keeps:
  - the **selection** (by name; names that disappeared are dropped);
  - the cursor (by name; if that entry is gone, the nearest index);
  - the top row (scroll position), the quick filter, the sort order and the hidden/system setting.

  The existing `Refresh()` behaviour stays as it is for explicit refreshes.
- **Deferred while the user is busy:** no automatic refresh while:
  - a modal dialog or the menu is open;
  - an incremental load is in progress;
  - the panel is the target of a running foreground operation (that operation refreshes at its end anyway);
  - the user is mid-way through a typed quick-search or filter.

  The pending refresh runs as soon as it's allowed.
- **Never fires on the app's own operations twice:** a refresh already done by an operation clears the pending
  signal, by comparing a generation counter.

### D3 — Settings (CFG)

- **`autoRefresh = on | off | poll`** (default **on**):
  - `on`: notifications only;
  - `poll`: also re-lists every `autoRefreshPollSeconds` (default 10) where notifications are unavailable, typically
    network drives;
  - `off`: today's behaviour.
- **Live reload:** like the other config keys (IS-0006).

### D4 — Plugin panels

- **Out of scope** for the plugin content itself: FTP/SFTP servers can't push changes, and ZIP is the plugin's own
  in-memory model.
- **Optional extension (not this plan):** a host API `RequestRefresh(side)`, which a plugin could call. The existing
  `Ctrl+R` already asks the plugin to refresh.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/FolderWatch.{h,cpp}` (new) | Watch handle, arm/re-arm/close, debounce state machine (the timer logic is pure: `Debouncer` with an injected clock) |
| `Source/MyCommander/Panel.{h,cpp}` | `RefreshPreservingState()`; a generation counter |
| `Source/MyCommander/main.cpp` | One `FolderWatch` per panel; poll in the main loop next to `PollPendingEdits`; the defer rules; re-arm on navigation and content release |
| `Source/MyCommander/Config.{h,cpp}` | `autoRefresh`, `autoRefreshPollSeconds` |
| Tests: `FolderWatchTests.cpp` (new), `PanelTests.cpp`, `ConfigTests.cpp` | See below |
| `README.md`, `USERGUIDE.md`, `CLAUDE.md`, spec (LIST-007 → IN-MVP / Implemented), changelog, `Version.h` | Docs |

## Tests

- **`Debouncer` (pure, injected clock):**
  - one signal fires after 300 ms;
  - a burst fires once;
  - continuous changes fire at least every 2 s;
  - deferred while busy, then fires on release.
- **`FolderWatch` against a real `TempDir`:**
  - creating, deleting and renaming a file, and changing its size, each signal within a bounded wait;
  - a change in a subfolder does not signal;
  - re-arming after a path change watches the new folder only.
- **`Panel::RefreshPreservingState`:**
  - the selection is kept and a vanished selected file is dropped;
  - the cursor stays on its name, or moves to the nearest index if the entry was deleted;
  - the filter, sort order and top row are kept.
- **Config:** parsing and defaults for the two keys.
- **Manual:**
  - Explorer creates and deletes files in a shown folder;
  - a large copy into a shown folder (refresh rate);
  - a network share (fallback), and `poll` mode;
  - the selection survives.

## Risks

- **Many watchers:** two panels mean two handles, which is trivial. Handles must be closed on every navigation
  (tested), or they leak.
- **Notification storms on busy folders** (`%TEMP%`, logs): the debounce plus the 2 s cap bound the cost. A folder
  with 100,000 entries re-lists incrementally (LIST-008) and keeps input responsive (PER-002).
- **Removable media:** a removed drive signals, then the refresh shows LIST-006's "location not available" banner,
  which is correct. The watcher then reports `Unavailable` and stops.

## Open questions

1. **Default:** automatic refresh **on** by default (recommended)? Or opt-in?
2. **Fallback:** a polling fallback for network drives (`poll`, every 10 s)? Or notifications only?
3. **Status line:** show a small indicator when a panel has no live notifications (e.g. `[no auto-refresh]`)?

## Status

Planned 2026-10-12. **Implemented 2026-10-14** (spec 1.31.0) with the recommended answers: on by default, `poll`
mode available, no status-line indicator.

Differences from the plan:

- **No per-navigation re-arm hooks.** `PollAutoRefresh` compares the panel's path with the armed path every loop
  iteration (a string comparison) and re-arms on change, so every navigation path is covered without touching each one.
- **"Already refreshed" uses `Panel::RefreshStarts()`**, not a generation bumped at the end of a listing: a signal
  raised before a listing *started* is covered by it, so the loop drains the watch when the count changes. This also
  avoids a second, redundant refresh after the app's own copy/move/delete.
- **Deferred while** a menu is open, a drag is in progress or the command line holds text. Modal dialogs need no rule:
  they run their own input loops, so the main loop (and this poll) does not run.
- **Plugin/tree content:** the watch is closed while content is installed; releasing it re-lists the real folder.
