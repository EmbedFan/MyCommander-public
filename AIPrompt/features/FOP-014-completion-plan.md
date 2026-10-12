# Implementation Plan — FOP-014 completion (queued delete + job overlap safety)

Source: `AIPrompt/my-commander.md`, `FOP-014`:

> The application may support queued and background file operations (copy and move; delete queueing and
> pause/resume are not yet implemented).

P2, `IN-MVP`, **Partial**. The original plan is `AIPrompt/features/FOP-014-plan.md` (Phases 1–3, shipped in
1.15.0). This plan covers what's left.

## Implementation check (2026-10-07)

| Planned in `FOP-014-plan.md` | Status | Where |
|---|---|---|
| D3 pure queue model: `JobKind {Copy, Move, Delete}`, states, FIFO, per-volume serialization | **Done**; `Delete` and `permanent` are already modelled | `OperationQueue.{h,cpp}`, `OperationQueueTests.cpp` |
| D1/D2 worker threads, prompt channel, cancel flags, progress | **Done**; the worker already runs `JobKind::Delete` via `DeleteItems` | `JobWorker.{h,cpp}` (`RunJob`, line ~185), `JobWorkerTests.cpp` (8 tests, copy only) |
| D5 queue keys | **Copy/Move only** (`Alt+F5`/`Alt+F6`). No way to queue a delete | `main.cpp` `QueueCopyOrMove` |
| D5 queue list | Done (`Alt+Q`; Enter cancels, a documented deviation from "Del") | `ShowJobQueue` |
| D5 status line | Done in the window title instead (documented deviation) | `FormatJobsTitle`, `PollBackgroundJobs` |
| D4 quit with jobs pending | Done | `ConfirmQuitWithJobs` |
| D6 scope: copy, move **and delete** | **Delete missing** | — |
| D7 delete confirmations apply to queued deletes | Not applicable yet (no queued delete) | — |
| Risks: "refuse a synchronous destructive operation on any path inside a running job" | **Not implemented** | — |
| Risks: elevation through the prompt channel | **Not implemented.** `ServiceJobPrompts` passes `win32Error = 0`, so *Elevate & Retry* is never offered for a job | `main.cpp` `ServiceJobPrompts` |
| Pause/resume | Out of FOP-014's own scope. The original plan assigns it to **FOP-015** (LATER). The FOP-014 row's wording is wrong to list it | spec |

### Why the missing overlap guard matters

Jobs are serialized per **volume of the destination** (for delete, of the first source). Two jobs on *different*
volumes run in parallel, even when they touch the same files:

- **Background job vs background job:** `Alt+F5` copies `C:\a` to `D:\x` (a job on volume D). A queued delete
  of `C:\a` (volume C) would start immediately, in parallel, and delete files the copy is still reading.
- **Background job vs synchronous operation:** the same problem exists today. While that copy runs, a plain `F8`
  on `C:\a`, an `F6` move, an `F2` rename or a `Ctrl+A` on it all run at once with the job.

Adding queued delete makes the first case easy to hit, so it needs the guard in the same change.

## Part 1 — Queue a delete

### Design

- **Keys:**
  - **`Alt+F8`**: queue a Recycle Bin delete of the selection (or cursor entry);
  - **`Alt+Shift+F8`**: queue a permanent one.

  This mirrors `F8`/`Shift+F8` and `F5`→`Alt+F5`. Both are free in `BuiltinShortcutInventory` and get added
  to it. `Alt+Delete` is not used, keeping the `F8` family consistent.
- **Confirmation (D7):** exactly the same rules as `DoDelete`:
  - `confirmRecycleBinDelete` / `confirmPermanentDelete` decide whether to ask;
  - the same Yes/No dialog and wording, with " (in the background)" appended to the title.

  The confirmation is never skipped for a queued job (the CLAUDE.md destructive-operation rule).
- **Refused, like `QueueCopyOrMove`,** when either panel shows plugin content (ZIP/tree): `kStrQueuePluginRefused`.
- **The job:**
  - `Job{kind=Delete, sources=folder/name..., permanent}`;
  - `destination` stays empty;
  - `volume` is `VolumeKey(first source)`, which the queue already fills in.
- **Queue list text:** `FormatJobLine` currently prints `"-> <destination>"` for every job. A delete prints
  `"Delete  name +N  (Recycle Bin)"` or `"(permanent)"`, with no arrow.
- **After the job:**
  - `PollBackgroundJobs` already refreshes panels showing a source's parent folder;
  - `SyncOtherPanelIfSamePath` covers the second panel;
  - failures are reported through the existing `ShowMessage` with `job.failures`.
- **Menu:** Commands gets "Queue delete" (`Alt+F8`), next to "Queue copy"/"Queue move" (MENU-002). The
  permanent variant stays key-only, like `Shift+F8`, which has no menu item either.

### Errors and prompts for a queued delete

The worker's `onError` already sends each failure through the prompt channel. Since IS-0018, the Recycle Bin
path deletes item by item too, so a locked file gets Retry/Skip/Skip all/Cancel for that item alone.

## Part 2 — Overlap safety between jobs, and between jobs and synchronous operations

### Design

- **New pure helpers in `OperationQueue`** (no filesystem access, lexical and case-insensitive, `/` = `\`,
  trailing separator ignored, like NAV-007's `FindFavoriteByPath`):

  ```cpp
  // Every path a job reads or writes: its sources, plus its destination for copy/move.
  std::vector<std::filesystem::path> JobPaths(const Job& job);

  // True if one path is the same as, or inside, the other.
  bool PathsOverlapLexically(const std::filesystem::path& a, const std::filesystem::path& b);

  // True if any path of `a` overlaps any path of `b`.
  bool JobsOverlap(const Job& a, const Job& b);

  // The first active (Running / WaitingForAnswer) job that touches any of `paths`, or nullopt.
  std::optional<Job> ActiveJobTouching(const std::vector<std::filesystem::path>& paths) const;
  ```
- **Job vs job:** `OperationQueue::NextToRun` already skips a queued job whose *volume* is busy. It
  additionally skips one that **overlaps** any active job, wherever the volume is. That job just waits its
  turn: FIFO among the rest, and no starvation, because the blocking job always finishes or gets cancelled.
- **Job vs synchronous operation:** before a synchronous operation starts, `main.cpp` checks the operation's
  paths with `JobWorker::ActiveJobTouching` (a locked wrapper):
  - **operations checked:** `F5`/`F6` (sources and destination), `Shift+F5` (source and target folder), `F8`
    and `Shift+F8`, `F2` rename, `Ctrl+A` attributes, `Shift+F7`/`F7` create (the folder);
  - **if a job touches them,** nothing runs. A message names the job and says to wait or cancel it (`Alt+Q`),
    e.g. *"A background job is using these files: Copy  a +2 -> D:\x. Wait for it to finish, or cancel it
    with Alt+Q."*;
  - **plain navigation, viewing and searching are never blocked.**
- **The same check runs when queueing:** a new job that overlaps an active one is **still accepted** and waits.
  This is the job-vs-job rule. A new *synchronous* operation, in contrast, is refused, because it can't wait in
  the background.

## Part 3 — Elevation for queued jobs: decision, not code

`ServiceJobPrompts` answers error prompts with `AskFileError(..., win32Error = 0)`, so *Elevate & Retry* is
never offered for a background job. Doing it properly means the worker must hand an elevation request to the UI
thread (the UAC prompt is modal) and wait. It's the same prompt-channel shape again, plus a third prompt kind and
carrying the Win32 error code through `AwaitError`.

**Recommendation:** keep it out of FOP-014 and document it. An access-denied item in a background job offers
Retry/Skip/Cancel; to elevate, run that item with plain `F5`/`F6`/`F8`. This is SEC-003's "one operation at a
time, explicitly" spirit, and it avoids a UAC prompt popping up while the user is doing something else.
(Open question 2.)

## Part 4 — Spec cleanup

- **`FOP-014` row:**
  - wording becomes "…queued and background file operations (copy, move and delete)";
  - **Implemented** once Parts 1–2 land;
  - the pause/resume mention moves out, since it's `FOP-015`'s, which stays `LATER` / Not Started, unchanged.
- **History note** for the release. It also records the Part 3 decision: no elevation for queued jobs.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/OperationQueue.{h,cpp}` | `JobPaths`, `PathsOverlapLexically`, `JobsOverlap`, `ActiveJobTouching`; `NextToRun` also skips overlapping jobs |
| `Source/MyCommander/JobWorker.{h,cpp}` | Locked `ActiveJobTouching` wrapper for the UI thread |
| `Source/MyCommander/main.cpp` | `QueueDelete` (`Alt+F8` / `Alt+Shift+F8`) with `DoDelete`'s confirmation rules; `FormatJobLine` for delete; busy-guard calls in the synchronous operations listed above |
| `Source/MyCommander/MenuBar.h` | Commands → "Queue delete" |
| `Source/MyCommander/PluginManagerLogic.cpp` | `Alt+F8`, `Alt+Shift+F8` in `BuiltinShortcutInventory` |
| `Source/MyCommander/Strings.h` | Queued-delete titles, the busy message |
| `Source/MyCommander/Help.cpp`, `README.md`, `USERGUIDE.md` | Keys, busy rule, no-elevation note |
| `AIPrompt/my-commander.md` | `FOP-014` row and history |
| `Source/MyCommander/Version.h` | MINOR bump (1.16 → 1.17): a new user-visible capability |
| `3_Reports/ChangeLogs/…` | Entry |

## Tests

**`OperationQueueTests.cpp`** (pure):

1. **`PathsOverlapLexically`:**
   - same path, any case or slash style: overlaps;
   - `C:\a` vs `C:\a\b`: overlaps, both ways;
   - `C:\a` vs `C:\ab`: **no** overlap (a prefix that isn't a path boundary);
   - `C:\a` vs `D:\a`: no overlap;
   - UNC paths: same rules.
2. **`JobPaths`:** copy/move gives sources plus destination; delete gives sources only.
3. **`JobsOverlap`:** a copy `C:\a → D:\x` vs a delete `C:\a`: overlaps. A copy into `D:\x` vs a delete of
   `D:\x\old`: overlaps. Unrelated trees: no overlap.
4. **`NextToRun` with overlap:**
   - job 1 copies `C:\a → D:\x` (Running);
   - job 2 deletes `C:\a`. It's on a different volume, so the volume rule alone would start it. It is **not**
     started;
   - job 3 deletes `E:\z`, which is unrelated. It **is** started, so there's no head-of-line blocking;
   - after job 1 finishes, job 2 starts.
5. **`ActiveJobTouching`:**
   - finds the running job for an overlapping path;
   - nullopt for unrelated paths;
   - nullopt once that job is Done.

**`JobWorkerTests.cpp`** (threads, `TempDir`):

6. **Queued permanent delete** of 20 files and a folder completes: everything is gone, the state is Done and
   progress reaches 20+1/21.
7. **Queued Recycle Bin delete** of 3 files completes. It uses the same "skip with a note if the Recycle Bin is
   unavailable" pattern as `FileOpsTests`.
8. **Queued delete with a locked file:**
   - the error prompt arrives for that file only;
   - answering Skip finishes the job as `Done` with the other files deleted (skipped isn't failed; a failure
     would leave the job `Failed`);
   - answering Cancel finishes it as `Cancelled`.
9. **Overlap end to end:** a slow copy job (a large file or many files) and a delete of its source queued right
   after it. The delete doesn't start until the copy is Done, and the destination is complete.

**`PluginManagerTests.cpp`:** `Alt+F8` and `Alt+Shift+F8` are reported as built-in.
**`MenuBarTests.cpp`:** Commands → "Queue delete" simulates `Alt+F8`.

**Manual:**

1. `Alt+F8` on a selection asks the same confirmation as `F8`, the title shows "1 running", and the panel
   refreshes when done.
2. Set `confirmRecycleBinDelete = false` and check that `Alt+F8` no longer asks.
3. Start a long `Alt+F5`, then `F8` its source: refused with the busy message.
4. Same as 3 with `Alt+F8`: queued, shown "Queued" until the copy finishes.

## Risks and notes

- **Main behavior change:** a synchronous operation on paths a background job is using is now refused instead
  of racing it. This is strictly safer, and the message says how to proceed (wait, or `Alt+Q` to cancel).
  Navigation, viewing and search are never blocked.
- **Conservative matching:** a copy's whole destination folder counts as "in use". A job copying into `D:\x`
  therefore also blocks a synchronous rename elsewhere inside `D:\x` while it runs. Finer tracking (only the
  items actually being written) would need per-item state from the worker and is out of scope.
- **Lexical matching only:** no filesystem access, so an offline share can't stall the check. A junction that
  points into a busy tree isn't recognized as the same place. NAV-009/SEC-006 already prevent operations
  descending *through* reparse points, which covers the dangerous direction.
- **Out of scope:** pause/resume (FOP-015), persisted or resumable jobs (ERR-007, deliberately never; ERR-006),
  elevation inside jobs (Part 3), queueing rename/attributes (instant operations, per the original D6).

## Phasing

1. **Phase 1:** overlap helpers in `OperationQueue`, the `NextToRun` change, tests 1–5. No user-visible change
   except that overlapping copy/move jobs now wait for each other.
2. **Phase 2:**
   - `QueueDelete` keys and menu, `FormatJobLine`, worker tests 6–9;
   - the synchronous busy guard;
   - the shortcut inventory.
3. **Phase 3:** docs, the spec row and history, version, changelog.

## Open questions

1. **Keys:** `Alt+F8` (Recycle Bin) / `Alt+Shift+F8` (permanent). OK?
2. **Elevation in background jobs:** leave it out and document "use plain F5/F6/F8 to elevate" (recommended),
   or build the third prompt kind now?
3. **Synchronous operation on a busy path:** refuse it with a message (recommended), or offer to queue it behind
   the job instead (more code, and only possible for copy/move/delete)?

## Status

Implemented 2026-10-07 (version 1.17.0), with the defaults for all three open questions: `Alt+F8` /
`Alt+Shift+F8`; no elevation in background jobs (documented); refuse a synchronous operation on busy paths.

Differences from the plan text:

- **Overlap with jobs queued earlier:** `NextToRun` also holds back an overlapping job behind an overlapping job
  queued *before* it, not only behind active ones. Without that, a delete of `C:\a` queued after a copy
  `C:\a → D:\x` that is itself still waiting for a busy D: would have started first. Covered by
  `OperationQueue_NextToRun_OverlappingJobNeverOvertakesEarlierQueuedOne_FOP014`.
- **How the synchronous guard reaches the worker:** a module-level `gJobWorker` pointer (set once in `wmain`,
  following the `gFileActions`/`gVisibleColumns` precedent), rather than adding the worker to seven function
  signatures.
- **Where the guard sits:** in `PerformCopyOrMove`, so F5/F6, Shift+F5 and drag-and-drop share it.
- **Delete confirmation:** factored out of `DoDelete` into `ConfirmDelete`, shared with `QueueDelete`.
- **Commands-menu mnemonic** for "Queue delete" is `E`.
