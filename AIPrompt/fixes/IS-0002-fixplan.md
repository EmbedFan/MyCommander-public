# Fix Plan — IS-0002

Source: `AIPrompt/issue_fix_requests.md`

> When the F4 is applied on a text file, the app opens the file in external editor, but wait
> for it is closed. I think it would be better to return immediately.
>
> I think the software now does not use file monitoring, so the OS does not notify the
> software when a file has been changed. I think it would nice:
> - activate notification for this file
> - start the editor software and returns back to app — in the editor user is working in
>   background
> - refresh file list — when notification arrived also refresh the file entry as it shown

## Summary

`F4` (`DoEdit`, `main.cpp`) currently launches the configured external editor and **blocks**
until it exits (`RunAndWait`), handing the console screen over to the child process for the
whole time (`Console::SuspendForChildProcess`/`ResumeAfterChildProcess`). The request: launch
the editor and return control to MyCommander immediately, so the app stays usable while the
user edits in a separate window; since MyCommander can no longer rely on "the editor just
exited, refresh now" to learn the file changed, watch the file for changes (Windows file
change notifications) and refresh that entry — and re-select it, reusing IS-0001's fix — when
a change is detected.

This is a real behavior change with genuine trade-offs (below), not a pure bug fix, so the
plan proposes it as a **new, configurable default** (Total Commander — this project's own
stated inspiration, `CLAUDE.md`'s Project description — already works this way by default)
rather than an unconditional change with no opt-out.

## Current behavior (root cause / why it blocks today)

```cpp
void DoEdit(Console& console, Panel& left, Panel& right, bool leftActive) {
    ...
    std::wstring editor = ResolveEditorCommand();
    console.SuspendForChildProcess();
    ProcessRunResult result = RunAndWait(editor, {filePath->wstring()}, target.Path());
    console.ResumeAfterChildProcess();
    target.Refresh();
    ...
}
```

`Process.h`'s `RunAndWait` deliberately does **not** create a new console for the child — its
own doc comment says so explicitly ("a console-based editor behaves exactly as it would run
standalone"). That's why `Console::SuspendForChildProcess()`/`ResumeAfterChildProcess()` exist:
they swap the console's *active screen buffer* from MyCommander's own private double-buffer
back to the original screen buffer the console host started with (and back again after), so a
console-subsystem editor sharing the same window can actually draw. This is a deliberate,
working design for the blocking case — but it's also exactly what makes "return immediately"
non-trivial: if `DoEdit` returned right away and MyCommander resumed drawing its own UI to the
console, a still-running **console-based** editor sharing that same window would have its
display fought over by both processes (only one screen buffer can be the visible one at a
time). A **GUI-based** editor (`notepad.exe` — the default — VS Code, Notepad++, Sublime, ...)
has no such problem; it owns its own window and never touches the console at all.

## Design

### 1. Launch detached, without waiting

Add `Process.h`/`Process.cpp`'s `RunDetached` (new function, alongside the existing
`RunAndWait`):

```cpp
struct DetachedProcessResult {
    bool started = false;
    HANDLE processHandle = nullptr;  // caller closes it once done watching; nullptr if !started
    std::wstring error;
};
DetachedProcessResult RunDetached(const std::wstring& exePath, const std::vector<std::wstring>& args,
                                   const std::filesystem::path& workingDirectory);
```

Implementation: `CreateProcessW` (same argument-quoting as `RunAndWait`, `SEC-004`) without
waiting; return the process handle instead of an exit code. **Does not create a new console**
(same as `RunAndWait` today) and **`DoEdit` no longer calls
`SuspendForChildProcess`/`ResumeAfterChildProcess`** — MyCommander keeps its own screen buffer
active and keeps redrawing normally throughout.

**Trade-off, stated plainly:** this means a console-subsystem `%EDITOR%` (rare on Windows —
essentially everyone's real-world choice is a GUI editor, but a user could set `%EDITOR%=vim`
under some POSIX-layer setup) would have its output interleave unpredictably with
MyCommander's own redraws, sharing one visible screen buffer with no handoff protocol this
time. Two ways to handle this, in increasing order of effort — **recommending the first**:

- **(Recommended) Accept the limitation, document it, provide the config escape hatch below.**
  Zero extra code. A user with a console-based editor sets `waitForEditorToClose = true` and
  keeps today's exact behavior, unaffected.
- **(More thorough, deferred)** Detect whether `editor` is a console- or GUI-subsystem
  executable by reading its PE header (`IMAGE_OPTIONAL_HEADER::Subsystem`, a small, dependency-
  free file read — no `CreateProcess` trial needed) and launch console-subsystem editors with
  `CREATE_NEW_CONSOLE` (their own independent window, no conflict with MyCommander's) while
  launching GUI-subsystem editors exactly as above (no new console, so no stray empty console
  window appears next to the editor's real window — which is the failure mode of naively
  always passing `CREATE_NEW_CONSOLE`). Worth doing if a console-editor user actually shows up
  and wants non-blocking too; not needed to satisfy IS-0002's stated ask, which was framed
  around "the editor" in the singular/generic sense with no console-editor mention.

### 2. Watch the file for changes, refresh (and re-select) on notification

New tracked state (proposed home: a small `std::vector<PendingEdit>` in `main.cpp`'s `wmain`,
alongside `commandLine`/`commandHistory` — this is UI-loop state, not `Panel` state, matching
how `Panel` stays free of anything about *why* a refresh happens):

```cpp
struct PendingEdit {
    std::filesystem::path filePath;      // the exact file being edited
    HANDLE editorProcess = nullptr;      // from RunDetached; signaled when the editor exits
    HANDLE changeNotification = nullptr; // FindFirstChangeNotificationW on filePath's parent
};
```

`DoEdit`, on a successful `RunDetached`, calls `FindFirstChangeNotificationW(parentDir, FALSE,
FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE)` and appends a `PendingEdit` to the
list (passed in/out, or returned) instead of refreshing immediately — there's nothing to
refresh yet.

**Where it's watched, and why polling instead of `WaitForMultipleObjects`:** `wmain`'s main
loop already calls `WaitForSingleObject(console.InputHandle(), 16)` every iteration — a 16 ms
poll specifically so LIST-008's incremental panel loading keeps making progress even when the
user is idle. That same cadence is a natural, already-existing place to also check pending
edits, via cheap non-blocking `WaitForSingleObject(handle, 0)` calls (`0` timeout = poll,
never blocks) — no need to fold these into a `WaitForMultipleObjects` array (which would also
cap concurrent pending edits at `MAXIMUM_WAIT_OBJECTS` (64) minus the input handle, and
requires figuring out *which* array slot signaled). A small `PollPendingEdits(edits, left,
right)` helper, called once per main-loop iteration:

- For each `PendingEdit`, non-blockingly check `changeNotification`. If signaled: re-arm via
  `FindNextChangeNotificationW`, then for **both** `left`/`right` panels, if that panel's
  `Path()` equals `filePath.parent_path()`, call `panel.NavigateTo(panel.Path(),
  filePath.filename())` — refreshes and re-selects the edited entry in place, reusing
  IS-0001's fix (`Panel::pendingSelectName_`) directly; no new `Panel` API needed.
- Then non-blockingly check `editorProcess`. If signaled (the editor has exited): do one final
  same refresh-and-reselect pass (in case a save happened right before exit and hasn't been
  picked up yet), then `FindCloseChangeNotification`/`CloseHandle` both handles and drop this
  entry from the list — the watch's lifetime is tied to the editor's own lifetime, so it can't
  accumulate unboundedly across a long session of editing many different files one after
  another.

**Known, accepted limitation:** each of the app's several modal dialogs (`ShowChoiceDialog`/
`ShowMessage`/`ShowTextPrompt`, the viewer, the help screen, the drive/search pickers) runs its
*own* blocking `ReadConsoleInputW` loop, independent of `wmain`'s outer loop — so a pending
edit's notification isn't polled while any dialog is open. Nothing is lost (a
`FindFirstChangeNotificationW` handle simply stays signaled until
`FindNextChangeNotificationW` is called, it doesn't need to be "caught" the instant it fires),
just delayed until control returns to the main panel view. Threading the poll through every
dialog's own input loop would be a much larger, disproportionate change for a self-healing
delay — not attempted here.

### 3. Config: `waitForEditorToClose` (new setting, changes the *default* behavior)

```cpp
// Config.h
// IS-0002: false (default) launches the external editor without waiting —
// MyCommander stays usable immediately, and the edited file's panel entry
// is refreshed (and re-selected) automatically via a file-change
// notification once the editor saves. Set true to restore the original
// behavior (wait for the editor to close before refreshing) -- needed for
// a console-subsystem %EDITOR% sharing this console window, which the
// non-blocking path doesn't hand off to cleanly (see IS-0002's fix plan).
bool waitForEditorToClose = false;
```

This is called out specifically because it's a **default-behavior change**, unlike every other
`Config` field added so far this project (UI-005/UI-010/etc. all defaulted to preserving prior
behavior exactly). Justification: the user explicitly asked for this as an improvement ("I
think it would be better"), and it matches how Total Commander — this project's own named
inspiration — already behaves by default. `DoEdit` branches on this flag: `true` keeps today's
exact code path (`RunAndWait` + suspend/resume + immediate `Refresh()`) untouched; `false`
takes the new `RunDetached` + watch path above.

## Files touched (implementation phase, not yet made)

- `Source/MyCommander/Process.h` / `Process.cpp` — new `RunDetached`.
- `Source/MyCommander/Config.h` / `Config.cpp` — new `waitForEditorToClose` field, parsing,
  default config file documentation.
- `Source/MyCommander/main.cpp` — `PendingEdit`, `PollPendingEdits`, `DoEdit` branching on
  `config.waitForEditorToClose`, one poll call added to the main loop.

## Test plan

- **`ProcessTests.cpp` (new file — verified `Process.cpp` currently has no dedicated test file
  and `RunAndWait` has zero existing test coverage anywhere in the suite, so there's no
  existing style to match)**: `RunDetached` against a short-lived real process (e.g.
  `cmd.exe /c exit 0`), asserting `started` is true and the returned handle eventually signals
  (`WaitForSingleObject` with a short real timeout, not `0`, in the test only) without this
  test itself blocking on the *app's* polling logic.
- **`ConfigTests.cpp`**: `waitForEditorToClose` parsing — defaults to `false`, parses
  `true`/`false` (reuse `ParseBool`), invalid-value warning — same pattern as every other
  boolean setting already covered there.
- **A pure, testable slice of the polling logic**: factor the "does this pending edit's parent
  directory match this panel, and what name should be re-selected" decision into a small pure
  function (e.g. `bool ShouldRefreshPanelForEdit(const Panel& panel, const
  std::filesystem::path& editedFile)`) that a test can call directly with real `Panel`
  instances and real temp-directory paths — the OS-level notification-handle plumbing around
  it stays untested, consistent with `main.cpp`'s established lack of direct coverage
  (`ACC-001`/`ACC-002`/`TST-005`/`UI-005`/`IS-0001` precedent), but the actual "which panel(s)
  should react" decision doesn't have to be.
- **Manual verification** (no interactive-console capture available in this environment,
  same limitation noted in IS-0001's plan): press `F4` on a text file, confirm MyCommander's
  panels are immediately interactive again while the editor window is open; edit and save in
  the external editor; confirm the file's size/modified-time column updates and the entry
  stays selected within roughly the polling cadence (~16 ms plus notification latency, so
  effectively "next frame"); close the editor and confirm no leaked, still-signaling watch
  remains (e.g. via Task Manager's handle count, or simply repeating the cycle many times and
  watching for any slowdown). Repeat with `waitForEditorToClose = true` and confirm the
  original blocking behavior is unchanged.

## Risk / scope notes

- Real new failure surface: `FindFirstChangeNotificationW`/`FindNextChangeNotificationW` can
  themselves fail (e.g. an inaccessible/removed network directory) — `DoEdit` must treat that
  as "editor launched, but proceed without a watch" (log a warning, ERR-004 style) rather than
  refusing to launch the editor at all; a failed watch is a missed convenience, not a reason to
  block the user from editing.
- The console-subsystem-editor limitation (section 1) is a deliberate, documented scope
  boundary, not an oversight — restated here so it isn't rediscovered as a surprise.
- `DoOpenShell`/`RunShellCommand`/`DoExecute` (F9, the internal command line, VEE-002/VEE-008's
  "execute" action) are **out of scope** and unchanged — they deliberately want the shared
  console (`F9` opens an interactive shell in the same window on purpose;
  `RunShellCommand`/`PauseForOutput` show a command's output inline). IS-0002 is specifically
  about `F4`/`DoEdit`.
- Verified the requirement ID: `VEE-006` ("Editing shall initially invoke a configured
  external editor.", P1, IN-MVP, already Implemented). Its wording is agnostic to
  blocking/non-blocking, so no `Implemented`/Disposition change is needed — this is a behavior
  refinement within an already-implemented requirement, the same situation IS-0001 was in for
  FOP-004/FOP-005.

## Status

**Implemented** (2026-09-24). Followed the plan as written, with one deliberate deviation:

- `Process.h`/`Process.cpp`: added `RunDetached`/`DetachedProcessResult` exactly as planned.
- `Config.h`/`Config.cpp`: added `waitForEditorToClose` (default `false`), `ParseBool`-based
  parsing under the `waitforeditortoclose` key, and documented it in the default config file.
- `main.cpp`: added `PendingEdit`, `PollPendingEdits` (polled once per main-loop iteration,
  alongside the existing `PumpRefresh` calls), and `DoEdit` now branches on
  `config.waitForEditorToClose` — `true` keeps the original code path (`RunAndWait` +
  suspend/resume) byte-for-byte; `false` uses `RunDetached` + the new watch. Both `DoEdit` call
  sites (`F4` directly, and `Enter` when `enterFileAction = edit`) updated to pass `config`,
  `logger`, and the new `pendingEdits` vector.
- **Deviation from the plan**: did not factor a separate `ShouldRefreshPanelForEdit` pure
  predicate out for testing. Once written, the actual logic
  (`RefreshPanelsForEditedFile`) turned out to be a two-line path comparison — extracting a
  whole separate testable module for that would have been unwarranted abstraction for what it
  actually turned out to be. Left inline in `main.cpp`, consistent with that file's established
  lack of direct test coverage elsewhere.
- One implementation-time bug caught by the build, not the plan: `FindNextChangeNotificationW`
  doesn't exist — only `FindFirstChangeNotificationW` has an `A`/`W` suffix pair (it's the only
  one of the three that takes a string); `FindNextChangeNotification` and
  `FindCloseChangeNotification` are unsuffixed. Fixed before the first successful build.
- Added `ProcessTests.cpp` (new — `Process.cpp` had zero prior coverage, `RunAndWait` included):
  `QuoteCommandLineArgument` basics plus `RunDetached`'s success path (a real short-lived
  `cmd.exe /c exit 0`, confirming the returned handle genuinely signals on exit within a
  generous real timeout) and failure path (nonexistent executable). Added 3 `ConfigTests.cpp`
  cases for `waitForEditorToClose` parsing (default, explicit `true`, invalid-value warning).
  Full suite: 280 cases / 10,928 checks / 0 failed (Debug run twice for the timing-sensitive
  process test, Release once) — up from 272/10,913 before this fix.
- No automated coverage of `PollPendingEdits`/the file-watch machinery itself or of `DoEdit`'s
  two call sites, consistent with this project's established limit (`main.cpp` has no direct
  test coverage). Manual verification of `F4`'s non-blocking behavior and auto-refresh in the
  running app is still outstanding — no interactive-console capture is available in this
  environment, same limitation noted in IS-0001's plan.
- Hit an unrelated environment snag while verifying: a Release-build `MyCommander.exe` from an
  earlier manual test session was still running and had the Release `.exe` file locked,
  blocking the linker (`LNK1104`). Asked the user rather than terminating their process
  unilaterally; they closed it and the retry succeeded cleanly.
- Documented in `AIPrompt/my-commander.md`'s version-history paragraph (0.42 -> 0.43), README
  (a Status bullet, the config sample/prose, and the top-of-file version line kept in sync),
  and `3_Reports/ChangeLogs/changelog_2026-09-24.md`. Verified `VEE-006`'s wording needs no
  change (see Risk/scope notes above) — no `Implemented`/Disposition edit made.
