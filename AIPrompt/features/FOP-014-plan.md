# Implementation Plan — FOP-014 (Queued and background file operations)

Source: `AIPrompt/my-commander.md`, `FOP-014` — "The application may support queued and background file
operations." P2, currently `LATER` / `Not Started`. Listed in the Postponement Register (§30) with FOP-015 and
ERR-007 ("Adds concurrency and recovery complexity").

## Summary

Today every copy, move, delete, rename and attribute change runs synchronously on the UI thread. The
operation blocks the input loop, and the user waits (Esc cancels) until it finishes. FOP-014 lets a
copy/move/delete be queued and run in the background while the user keeps navigating and starting other work.

This plan keeps the scope to what FOP-014 asks for: queuing and background execution. It does not add
pause/resume (FOP-015) or persisted recovery (ERR-007).

## What exists today

| Piece | Location | Relevance |
|---|---|---|
| Synchronous engine | `FileOps.h`: `CopyItems`, `MoveItems`, `DeleteItems`, `ToggleAttributes` | Takes callbacks: `ConflictHandler`, `ErrorHandler`, `ItemStartHandler`, `CancelPoll`. Pure C++, no `Console` or `Panel`. |
| Prompts on the UI thread | `main.cpp`: `AskConflict` (line ~1079), `AskFileError` (~1133) | Modal dialogs. Must run on the thread that owns the console. |
| Progress and cancel | `main.cpp`: `ProgressUi`, `PeekEscapePressed`, `AnimatedProgressDialog` (`Dialog.h`) | Renderer runs on its own thread, but workers never call application code (`Dialog.h` comment). |
| Thread-safe console | none | `Console` is not thread-safe. Only the UI thread may draw or read input. |
| Elevation | `Elevation.h`, `ElevatedAction` | Triggered from the error prompt; must stay on the UI thread (a UAC prompt is modal). |
| Refresh after an operation | `main.cpp`: `SyncOtherPanelIfSamePath`, `Refresh()` | Panels are not thread-safe; only the UI thread may refresh them. |
| Crash safety | `Session.h`, `Config.h` (ERR-006) | A queued job must never be replayed after an abnormal exit. Queue state is therefore in memory only. |

## Design

### D1 — Threading model: one background worker, UI thread owns everything else

- **One worker per volume** (drive letter or UNC share). Jobs on the same volume run one after another in FIFO
  order, so two jobs never write to or delete the same tree at once. Jobs on different volumes run in parallel
  (decided: open item 5).
- The worker calls `FileOps` only, with no `Console`, `Panel` or `Dialog` access.
- Progress is published as plain data (`doneCount`, `totalCount`, `currentItem`, `state`) under a mutex. The UI
  thread reads it on every paint, the same way `AnimatedProgressDialog` already reads its own state.
- Completion is published the same way. The UI thread notices it on its next pass, refreshes the affected
  panels (`SyncOtherPanelIfSamePath`, the same helper the synchronous path uses) and shows any outcome summary
  (`ReportOutcome`).

### D2 — Prompts from a background job: the worker asks, the UI thread answers

The hard part. A job can hit a name conflict (`ConflictHandler`) or a non-collision failure (`ErrorHandler`,
including `ElevateAndRetry`). Both need an answer, but only the UI thread may show a dialog.

- The worker posts a **prompt request** (kind, paths, Win32 error text) and blocks on a condition variable.
- The UI thread's input loop checks for a pending request each time it wakes (it already wakes every 16 ms via
  `WaitForSingleObject(..., 16)`, `main.cpp:~3170`). It shows the existing `AskConflict`/`AskFileError` dialog,
  stores the answer, and signals the worker.
- While a prompt is outstanding the job shows state `WaitingForAnswer`. Other queued jobs wait behind it, and the
  user can answer at any time.
- Esc cancels a job through the existing `CancelPoll` mechanism (an atomic flag the worker polls between
  items, the same way `PollAndConsumeEscape` works today).

Rejected alternative: pre-resolving every conflict with a queue-wide default (ask at queue time). It is simpler
but can't answer a conflict that only appears mid-copy, so it would change what the user is asked and when.
Mentioned in Open items in case you prefer it.

### D3 — Queue model (pure, unit-tested before any threading)

New `OperationQueue.{h,cpp}`, `Console`-free and thread-free (TST-005 precedent):

```cpp
enum class JobKind { Copy, Move, Delete };
enum class JobState { Queued, Running, WaitingForAnswer, Done, Cancelled, Failed };
struct Job { uint64_t id; JobKind kind; std::vector<std::filesystem::path> sources;
             std::filesystem::path destination; bool permanent; JobState state; int done; int total; };
class OperationQueue {
public:
    uint64_t Enqueue(Job job);
    bool CancelQueued(uint64_t id);          // only a job that has not started
    std::optional<Job> NextToRun();           // FIFO; marks it Running
    void Finish(uint64_t id, JobState finalState);
    const std::vector<Job>& Jobs() const;
};
```

Only the state machine lives here. Thread handoff is in Phase 2. Running jobs cannot be removed from the queue,
only cancelled through the cancel flag.

### D4 — Persistence: none (ERR-006, ERR-007)

Queue contents live in memory only. Nothing is written to disk, so a crash can never make a queued job run
again on the next start. This matches ERR-006's rule that no operation state may survive an abnormal exit. On a
normal quit, the app asks before exiting if any job is running or queued, the same kind of confirmation
`ZipPlugin` already uses for unsaved archive edits (IS-0014).

### D5 — User interface

- **Queue a job:** a key that queues the current selection instead of running it. Recommended: **Alt+F5** for
  copy and **Alt+F6** for move (both unclaimed in `BuiltinShortcutInventory`), with a `Background` option on
  the existing confirmation dialog as an alternative path. Open items.
- **See the queue:** a list (`ShowListChoice`, already used by FEA-0009) with state, progress and item name.
  Enter on a job opens it; Del cancels a queued job or a running one (confirmed).
- **Status:** the bottom status line shows `1 running, 2 queued` while any job is active, and the hint bar gets a
  context for the queue list.

### D6 — Scope: which operations are queueable

Copy, move and delete only. Rename, create and attribute changes stay synchronous, since they are instant. ZIP
plugin operations are excluded: the plugin SDK has no asynchronous contract (documented gap, not solved here).

### D7 — Default behaviour is unchanged

Every existing key still runs synchronously. Background execution is opt-in per job. The existing
confirmation rules (`confirmRecycleBinDelete`, `confirmPermanentDelete`) apply to queued deletes too, since
the workflow must not skip a confirmation (CLAUDE.md, destructive operations).

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/OperationQueue.{h,cpp}` (new) | Job model, state machine, FIFO. Pure. |
| `Source/MyCommanderTests/OperationQueueTests.cpp` (new) | Unit tests (below). |
| `Source/MyCommander/JobWorker.{h,cpp}` (new, Phase 2) | Worker thread, prompt channel, progress publication. No `Console`. |
| `Source/MyCommander/main.cpp` | Queue keys, queue list dialog, servicing prompt requests in the input loop, panel refresh on completion, quit confirmation. |
| `Source/MyCommander/PluginManagerLogic.cpp` | New shortcuts added to `BuiltinShortcutInventory`. |
| `Source/MyCommander/MenuBar.h` | Queue items under File or Commands. |
| `Source/MyCommander/Strings.h` | Queue labels and status text. |
| `Source/MyCommander/Help.cpp`, `README.md`, `USERGUIDE.md` | Document the keys and the queue. |
| `AIPrompt/my-commander.md` | `FOP-014` status and history. |
| `Source/MyCommander/Version.h` | MINOR bump. |
| `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md` | Entry per phase. |

## Test plan

Phase 1 (pure, `OperationQueueTests.cpp`):

1. Enqueue returns increasing ids; a new job is `Queued`.
2. `NextToRun` is FIFO and marks the job `Running`; a second call returns the next job, not the running one.
3. `CancelQueued` removes a queued job; it returns false for a running or finished job.
4. `Finish` sets `Done`, `Cancelled` or `Failed`; an unknown id is ignored.
5. Empty queue: `NextToRun` returns nothing.

Phase 2 (threads, with real `TempDir` trees, TST-004):

6. A queued copy of 50 small files completes, and the destination holds every file.
7. A job whose conflict handler is answered with Skip finishes with the skipped item counted.
8. Cancel flag set mid-job stops the job at the next item, and the state becomes `Cancelled`.
9. Two jobs run in order, never concurrently (checked by a counter that the worker increments on start and
   decrements on finish).
10. Prompt channel: the worker blocks until answered, then continues with that answer.

Manual (the console cannot be captured in this project, same limitation as other UI features): queue a copy,
keep navigating, answer a conflict prompt that appears mid-job, cancel with Esc, and quit with a job running.

## Risks and notes

- **Concurrency bugs** are the main risk. Keeping the worker free of `Console` and `Panel` access, and making the
  prompt channel the only handoff, limits the surface. The Phase 1 pure model is tested before any thread
  exists.
- **Stale panels:** a job can change a directory the user is looking at. The panel shows the old listing until
  its next refresh. The completion notice refreshes the affected panels; a finer-grained live refresh is out of
  scope.
- **Same file in two places:** a queued move and a later synchronous rename of the same file can conflict. The
  single-worker FIFO avoids overlap between jobs, but a synchronous operation can still run while a job is
  active. Plan: refuse a synchronous destructive operation on any path inside a running job, and say so in the
  status line.
- **Elevation** always runs on the UI thread through the prompt channel, so the UAC prompt stays modal and
  behaves as it does today.
- **Memory:** queued jobs hold path lists only, so memory is proportional to the number of queued paths, not to
  file contents.

## Phasing

- **Phase 1:** `OperationQueue` pure model and its tests. No user-visible change.
- **Phase 2:** worker thread, prompt channel, progress publication, and the thread tests (6–10). Still no key bound.
- **Phase 3:** keys, queue list, status line, quit confirmation, menu items, docs, spec, version, changelog.

## Open items

1. **Prompt timing (resolved: answer as they happen, D2):** a background job stops and asks the user at a name
   clash or failure, and the queue waits behind that prompt.
2. **Keys (resolved: Alt+F5 copy, Alt+F6 move).**
3. **Default mode (resolved: opt-in):** F5/F6 stay synchronous, and Alt+F5/Alt+F6 queue in the background.
4. **Quit with work pending (resolved: ask each time):** when quitting with jobs queued or running, show
   "N jobs pending: wait, or cancel them and quit?"
5. **Concurrency (resolved: parallel across volumes, serial on one volume, see D1).**
