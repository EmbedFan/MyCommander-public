# Implementation Plan — FOP-015 (pause and resume long-running operations)

Source: `AIPrompt/my-commander.md` §10:

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| FOP-015 | The application may pause and resume supported long-running operations. | P2 | LATER | Not Started |

## Implementation check (2.0.0.368)

**Long operations and how they are interrupted today:**

| Operation | Runs on | Interruption point |
|---|---|---|
| Copy / move (F5/F6, drag) | UI thread with a progress box (FOP-009) | `CancelPoll` per item, and the `CopyFileExW` / `MoveFileWithProgressW` progress routine during a file (`FileOps.cpp:75`, rate-limited by PER-005) |
| Delete (Recycle Bin per item, permanent) | UI thread | `CancelPoll` per item |
| Background jobs (FOP-014: copy, move, delete) | Worker threads (`JobWorker`) | A per-job `std::atomic<bool>` cancel flag (`cancelFlags_`) checked by the same `CancelPoll` |
| Find files, compare folders, synchronize, group rename, checksum/diff (planned) | UI thread | `CancelPoll` / `PeekEscapePressed` |
| Plugin work (ZIP pack/unpack, FTP/SFTP transfers) | UI thread, inside the plugin | `McPluginHost::PollCancel` |

**Facts the design builds on:**

- Every one of these loops already calls a cancel poll often (at least per item, and per progress tick inside a
  file). A pause is "the cancel poll waits while paused", so it needs no new hook points in `FileOps`.
- Blocking inside the `CopyFileExW` progress routine pauses the copy mid-file. Windows keeps both handles open; the
  pause is safe for local volumes.
- `AnimatedProgressDialog` already has `Pause()` / `Resume()` (it stops its renderer thread while a modal question is
  shown).
- `JobState` is `{Queued, Running, WaitingForAnswer, Done, Cancelled, Failed}`, with no Paused state.
- **FTP/SFTP clients** measure their bounded network waits from when the wait starts, and they poll cancel *inside*
  those waits. If `PollCancel` blocked while paused, a pause would look like a server timeout and drop the session.
  Plugins therefore need an explicit safe point instead (D4).

## Design

### D1 — A shared pause token (`PauseToken`, `Source/MyCommander/PauseToken.{h,cpp}`, pure plus a condition variable)

```cpp
class PauseToken {
public:
    void Pause(); void Resume(); bool Paused() const;
    // Blocks while paused; returns false at once if cancelled meanwhile.
    bool WaitWhilePaused(const std::function<bool()>& cancelled, const std::function<void()>& tick = {});
};
```

- It wraps the existing cancel poll as `CancelPoll pausable = [&]{ return !token.WaitWhilePaused(cancel, tick); }`.
  Every engine (FileOps, Search, DirCompare, DirSync, MultiRename, TextDiff) gets pause **without changing its
  signature**.
- `tick` keeps the UI alive while paused: it redraws "Paused", handles resize, and polls keys.
- **Cancel always wins:** a cancel while paused returns immediately.

### D2 — Foreground operations: the **P** key and Space in progress boxes

- **Keys:** `PollAndConsumeEscape` becomes `PollProgressKeys`, returning `{cancel, togglePause}`:
  - `Esc` cancels, as today;
  - **`P` or `Pause/Break`** toggles pause.
- **While paused:**
  - the progress box shows **"Paused — P: resume, Esc: cancel"**, and its title gets `[paused]`;
  - the speed and ETA freeze rather than decay;
  - the elapsed-time counter excludes paused time.
- **Supported:** copy, move, delete, find, compare, synchronize (execution), group rename and the planned
  checksum/disk-usage scans.
- **Not pausable:** short or atomic steps, for example a single same-volume rename (`MoveFileEx`). These simply
  finish.

### D3 — Background jobs: Paused state and job-list commands

- **State:** `JobState::Paused` is new, plus a `PauseToken` per job next to its cancel flag.
- **Job queue keys** (`Alt+Q`): on the selected job, **`P`** pauses or resumes it, and **`Del`** cancels it (as
  today). New: **"Pause all" / "Resume all"**.
- **Overlap rules (FOP-014):** a **paused job still holds its paths**. Jobs waiting behind it keep waiting, and
  synchronous operations on those files are still refused. Releasing the paths while paused would let another
  operation change files a half-done job depends on.
- **Quitting with paused jobs:** asks, as for active jobs (`ConfirmQuitWithJobs`). Paused jobs count as active.
- **Window title:** shows `n paused` alongside the running count.

### D4 — Plugins: an explicit safe point (plugin API 1.11, backward compatible)

- **New host function** `McHost_WaitWhilePausedFn WaitWhilePaused` (appended to `McPluginHost`):
  - returns `false` if the user cancelled;
  - blocks while paused, showing the host's paused state in the plugin's progress box;
  - returns `true` to continue.
- **Callers:** plugins call it at safe points only:
  - ZIP between entries;
  - FTP/SFTP between transfer chunks, when no network wait is in progress.
- **`PollCancel` never blocks.** Plugins built against API ≤ 1.10 simply can't be paused; the progress box hides the
  `P` hint for them.
- **Resuming a remote transfer after a long pause:** if the server dropped the idle session, the next chunk gets
  `ConnectionLost`, and the existing reconnect prompt runs (FTP-009/SFTP-008). Resuming the partial transfer itself
  stays FTP-011/SFTP-011 (later).
- **Updated:** ZIP, FTP and SFTP plugins, and the SDK headers and `DEVELOPER_GUIDE.md`.

### D5 — What is out of scope

- **Pause across restarts:** that is journaling, ERR-007 (later). A pause lives only as long as the process.
- **Throttling / bandwidth limits.**

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/PauseToken.{h,cpp}` (new) + `PauseTokenTests.cpp` | Token |
| `Source/MyCommander/Console.{h,cpp}` / `Dialog.{h,cpp}` | `PollProgressKeys`; the paused rendering of the progress boxes; ETA/elapsed excluding paused time |
| `Source/MyCommander/main.cpp` | Wire the token into every long operation; the job-list keys |
| `Source/MyCommander/OperationQueue.{h,cpp}`, `JobWorker.{h,cpp}` | `JobState::Paused`, the per-job token, Pause/Resume/PauseAll/ResumeAll; paused jobs keep their paths |
| `Source/MyCommanderPluginSdk/McPluginHost.h`, `McPluginAbi.h`, `DEVELOPER_GUIDE.md` | API 1.11 `WaitWhilePaused` |
| `Source/MyCommander/PluginManager.cpp` | Implement it |
| `Source/ZipPlugin`, `Source/FtpPlugin`, `Source/PluginCommon/RemotePanel.cpp` | Call it at safe points |
| `Help.cpp`, `HintBar.h`, README, USERGUIDE, CLAUDE.md, spec (FOP-015 → IN-MVP / Implemented; new PLUG-015 for the SDK call), changelog, `Version.h` | Docs |

## Tests

- **`PauseToken`:**
  - waits while paused, and resumes;
  - a cancel while paused returns at once;
  - `tick` is called while paused;
  - thread safety: pause and resume from another thread.
- **FileOps with a pausable poll:**
  - pausing for 300 ms during a multi-file copy delays it but completes it, and the files are identical;
  - a cancel while paused leaves no partial target (FOP-012);
  - pausing inside a large single file (progress routine) resumes and finishes intact.
- **`OperationQueue` / `JobWorker`:**
  - Paused state transitions;
  - a paused job keeps its paths, so an overlapping job keeps waiting and `ActiveJobTouching` still reports it;
  - Pause all / Resume all;
  - the destructor cancels paused jobs without hanging.
- **Plugin harnesses:**
  - ZIP pack pauses between entries;
  - SFTP panel harness with the fake client: a pause between chunks resumes cleanly;
  - an old-API plugin is never blocked.
- **Manual:**
  - pause a 2 GB copy mid-file, check with Explorer that the target is partial and locked, resume, and compare;
  - pause a background job and check that a second job on the same files waits;
  - pause an SFTP upload for 10 minutes (the server drops the session), resume, and see the reconnect prompt.

## Phasing

1. `PauseToken`, `PollProgressKeys`, and foreground copy/move/delete.
2. Background jobs (`Paused` state, job-list keys).
3. The other foreground scans (find, compare, sync, rename, checksum).
4. SDK 1.11 `WaitWhilePaused`, then the ZIP/FTP/SFTP plugins.

## Open questions

1. **Key:** `P` (and `Pause/Break`) in progress boxes and the job list? Or Space?
2. **Paused jobs:** a paused background job keeps its paths locked (recommended, safe)? Or releases them?
3. **Plugins:** include the plugin SDK part (phase 4) now? Or leave plugin operations unpausable for this release?

## Status

Planned 2026-10-12. **Implemented 2026-10-14** (spec 1.32.0, plugin API 1.11) with the recommended answers: `P` and
`Pause/Break`, a paused background job keeps its paths, and the plugin SDK part included.

Differences from the plan:

- **`PollProgressKeys` lives in `Console.cpp`; the pause wait lives in `main.cpp`'s `PeekEscapePressed`** (a shared
  `gForegroundPause` token), because the paused rendering needs the animated box and the header-line scans' own screen.
  Scans that have no box (find, compare, hashing, disk usage) show "Paused -- P: resume, Esc: cancel" on the bottom row.
- **Speed/ETA:** the progress box has no ETA or elapsed counter, only a smoothed speed; it is re-based on resume so a
  long pause does not decay it. The box spinner freezes as `[=]` while paused.
- **Job list:** `P` pauses/resumes the selected job, `A`/`R` pause/resume all (`Enter`/`Del` still cancel). "Wait until
  they finish" on quit resumes paused jobs first, since waiting for a paused job would never end.
- **A job paused while a prompt is open** keeps `WaitingForAnswer` until answered, then becomes `Paused`.
- **Plugins:** the host ignores `P` for a plugin built against API < 1.11. The ZIP plugin and `RemotePanel` (FTP and SFTP)
  call `WaitWhilePaused` per item and (remote transfers) per progress chunk; the Esc that cancels a wait is consumed by
  the host, so each plugin keeps it in a per-operation flag. The "hide the P hint for old plugins" detail was not needed:
  plugin progress boxes carry no key hint.
- **Not done:** an SFTP-panel-harness pause case (the FTP panel harness covers the shared `RemotePanel` code).
