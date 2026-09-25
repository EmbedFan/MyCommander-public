# Terminal File Commander — Software Requirements Specification

**Document status:** Initial planning draft  
**Version:** 0.52 (2026-09-20: added an "Implemented" field to every requirement row —
Implemented/Partial/Not Started/N/A, evaluated against `Source/MyCommander/` as of this date;
0.6 marks SRC-001/002/003 Implemented after the quick-filter/recursive-search slice; 0.7 marks
CLI-002/003/005/006 Implemented and CLI-004/007 Partial after the command-line/shell slice —
CLI-004/007 stay Partial because command history is in-memory/session-only until the CFG-*
configuration system exists to persist or configure it; 0.8 marks CON-005 Implemented after
adding attribute display/toggling and fixing the UNC-share-root navigation dead end; 0.9 marks
UI-004 Implemented after adding the type/modification-time columns, and corrects UI-007 to
Implemented — it had gone stale at "Not Started" since the 0.7 command-line slice actually
implemented it; 0.10 (2026-09-21) decides DEC-007 (a minimal hand-rolled "key = value" config
file, no third-party parser) and marks CFG-001/002/004/005 and FOP-008 Implemented after adding
the Config module and wiring its two delete-confirmation safety rules into F8/Shift+F8 — note
CLI-004/007's command-history persistence still isn't part of this config file, so they remain
Partial as noted above; 0.11 (2026-09-21) marks FOP-010 Implemented after adding Retry — the
last of overwrite/skip/rename/retry/cancel/apply-to-all it was missing — to the copy/move/
delete engine via a shared retry helper and an "Operation failed" dialog offering Retry/Skip/
Skip all/Cancel for a failure that isn't a name collision; 0.12 (2026-09-21) marks VEE-002
Implemented after adding `Config::enterFileAction` (view/edit/execute, default view) and wiring
it into the bare-Enter-on-a-file case — F3/F4 still always mean View/Edit regardless of this
setting — and marks VEE-008 Implemented (was N/A) now that "execute" gives the app an actual
run-a-file path: `DoExecute`, reached only via Enter and only once the user has explicitly
opted into that setting, launches the file through its own Windows association; 0.13
(2026-09-21) marks KEY-001 Implemented — was Partial, but on inspection the gap note was stale
against the P0 feature set now built this session (search, command line, drive selection all
exist and are keyboard-only); more fundamentally, `Console`'s `SetConsoleMode` call has never
enabled `ENABLE_MOUSE_INPUT`, so `ReadConsoleInputW` structurally cannot hand any code path a
mouse event — every existing and future feature is keyboard-only by construction, not by each
feature separately remembering not to depend on the mouse; documented this invariant with a
comment on that call and in the README; 0.14 (2026-09-21) marks ERR-001 Implemented after
auditing every file-system error message for "affected path and operation" and fixing the ones
that fell short — `RenameItem`/`CreateNewDirectory`'s Win32-failure paths were bare system
messages with no filename, and every message inside a *recursive* copy/move/delete
(`CopySingleFile`, `CopyDirectoryRecursive`, `RemoveTreeSafely`, `MoveItems`' cross-volume
fallback) only ever surfaced the top-level item name, not the specific nested file that actually
failed several directories deep; 0.15 (2026-09-21) marks ERR-003 Implemented — the last multi-
item operation without Retry/Skip/Skip all/Cancel (FOP-010) was `ToggleAttributes`, which just
recorded a failure and moved on; unified its bespoke `AttributeToggleOutcome` result type into
the same `OperationOutcome` shape Copy/Move/Delete already use (removing a near-duplicate
struct) so it could reuse `RunWithRetry` and `main.cpp`'s existing `ReportOutcome`/`AskFileError`
directly, rather than adding a second, parallel implementation of the same mechanism; 0.16
(2026-09-21) marks ERR-006 Implemented after auditing every startup code path for anything that
could read back and act on state left by an abnormal termination — found none: `Config` (loaded
unconditionally on every startup) only ever holds user-set preferences, never panel paths or
operation state; there is no session file and no operation journal (ERR-007 is explicitly
deferred), so no file operation can ever be queued, journaled, or auto-resumed, meaning a crash
structurally cannot cause one to be silently repeated; documented this invariant on `Config` and
at the `wmain` startup site as a constraint future NAV-008 (session/panel-path restore) work
must preserve, not a gap this change needed to fill by building something new; 0.17
(2026-09-21) marks PER-002 Implemented after closing the actual responsiveness asymmetry: unlike
Copy/Move, permanent `DeleteItems` and `ToggleAttributes` had no `cancelPoll`/`onItemStart` at
all — a large permanent delete or attribute-toggle selection ran to completion with a frozen
screen and no way to interrupt it. Added the same live-progress-plus-Esc-cancel plumbing they
were missing (including a `cancelPoll` check in `RemoveTreeSafely` before descending into each
entry, so a deep recursive delete is interruptible mid-tree, not just between top-level items);
`ToggleAttributes` only needed `onItemStart` (each item is two near-instant Win32 calls, nothing
to poll mid-item). Deliberately did **not** pursue full concurrency (background worker threads
letting panel navigation continue during a running operation) — that's a substantial
architecture change none of DEC-002/DEC-005 call for, isn't how Total Commander or Explorer's
own modal copy dialogs behave either, and the requirement's practical bar — the app never
appears frozen, and input (chiefly Esc) is always honored promptly — is now met for every
multi-item operation, not just Copy/Move; 0.18 (2026-09-21) marks TST-001 Implemented — path
handling (`PathUtilTests.cpp`), sorting (`PanelTests.cpp`'s NAV-005/LIST-002 tests, added
earlier this session), conflict resolution and file-operation planning (`FileOpsTests.cpp`) all
already had dedicated coverage; the one gap was `Panel`'s selection state (`SEL-001/002/004/005`
— `ToggleCursorSelection`/`SelectAll`/`ClearSelection`/`InvertSelection`/`SelectedCount`/
`SelectedSizeBytes`/`SelectionOrCursor`), which had no tests at all until now — added 11; 0.19
(2026-09-21) marks TST-003 Implemented after auditing all 8 named categories: empty files,
read-only files, reparse points, and (as of earlier this session) simulated access failures
already had extensive coverage; Unicode filenames (as opposed to Unicode file *content*, already
covered), paths beyond `MAX_PATH`, large files, and hidden files being copied/deleted (as
opposed to toggling the attribute, already covered) had none. Added 6 `FileOpsTests.cpp` cases;
"large" deliberately means a few MB exercising the real chunked `CopyFileExW` path, not
multi-gigabyte — that scale is TST-006's job with generated/sparse data, not a per-test-run
concern here; 0.20 (2026-09-21) adds UI-014, requiring panel separators/borders and dialog
boxes to use the IBM PC/CP437 Unicode box-drawing character set (see
`AIPrompt/ascii_pc_frame_characters.md` for the reference glyph table) rather than plain ASCII,
cross-referenced with ACC-006 so the existing `+`/`-`/`|` style remains the sanctioned
basic-symbol fallback rather than being replaced outright — a code audit at doc-time found two
gaps: `Dialog.cpp`'s `DrawBox` currently draws only the plain-ASCII form (never the box-drawing
form), and `main.cpp`'s panel-separator fill character is `L'\xB3'`, which is the raw Unicode
code point U+00B3 (SUPERSCRIPT THREE, "³"), not U+2502 (│, BOX DRAWINGS LIGHT VERTICAL) — the
glyph CP437 byte 0xB3/179 actually maps to; both are noted here for a future implementation
pass and UI-014 is filed as Not Started, not Implemented, since this task was documentation-only)  
0.21 (2026-09-21) marks UI-014 Implemented: both dual-pane borders/separator and modal-dialog frames now use the
Unicode CP437-compatible single-line box-drawing glyphs by default. `useBasicSymbols = true` in the per-user
configuration selects the documented ASCII `+`/`-`/`|` fallback for fonts or terminals that cannot render them;
the parser and default configuration file are covered by automated tests.
0.22 (2026-09-21) marks UI-003 Implemented: every panel now shows a dedicated volume-information line beneath
its path, with its current volume root, label when available, drive type, and free/total space when queryable.
The information is refreshed along with the panel listing and supports normal drive roots and UNC share roots.
0.23 (2026-09-21) marks UI-008 Implemented: the bottom hint bar is now chosen from the active state rather than
being static. It distinguishes command-line editing, inaccessible locations, filtered listings, selections,
directories, files, and empty listings, and is covered by pure `HintBarTests.cpp` tests.
0.24 (2026-09-21) marks NAV-006 Implemented: each panel has an independent, session-only location history.
`Alt+Left` and `Alt+Right` navigate backward and forward; navigating to a new location clears only that panel's
forward history. The history is deliberately not persisted, consistent with ERR-006/NAV-008.
0.25 (2026-09-21) marks NAV-008 Implemented: a separate, atomic-replacement session file records only the left
and right panel locations, and only after a normal input-loop exit. The next no-argument launch restores those
locations; explicit command-line paths take precedence. No operation, command, selection, or other actionable
state is persisted, preserving ERR-006's guarantee that a crash can never replay work.
0.26 (2026-09-21) marks NAV-009 Implemented: the panel now explicitly identifies every Win32 reparse point as
`LINK`/`<REPARSE>` and refuses to navigate into it, including navigation requested by search results. Combined
with the existing SEC-006 guards in recursive copy, search, and permanent delete, a junction or symbolic link
cannot create an accidental traversal loop while remaining visible and actionable as a link.
0.27 (2026-09-21) marks NAV-010 Implemented: each panel hides Windows hidden and system entries by default;
`Ctrl+H` independently reveals or hides them and marks the active visibility mode in the panel header. The
visibility toggle is a session-only browsing control, not a filesystem attribute change, and is covered by
focused hidden/system listing tests.
0.28 (2026-09-21) marks LIST-004 Implemented: `groupDirectoriesFirst = true` is a persistent configuration
preference that preserves the traditional directories-before-files group for every sort key. Setting it to
`false` sorts all entries together; `..` remains first in either mode. Parsing and panel-order tests cover both
configurations.
0.29 (2026-09-21) marks LIST-008 Implemented: application panels enumerate directories in bounded batches on the
input loop, yielding every 16 ms while idle so keyboard input and resize events remain responsive. A loading
marker is displayed until filtering and sorting complete; a focused 300-entry test proves that loading progresses
across batches rather than completing in one blocking refresh.
0.30 (2026-09-21) marks SEL-003 Implemented: `Panel::SelectByMask(pattern, select)` sets or clears the selection
mark on every entry matching a wildcard mask (PathUtil.h's existing `WildcardMatch`, shared with the SRC-003
quick filter and SRC-001 recursive search), leaving non-matching entries' selection untouched. Wired to
`Numpad +`/`Numpad -`, which now prompt for a mask (default `*`, i.e. everything) instead of unconditionally
selecting/clearing everything; `Ctrl+Numpad +`/`Ctrl+Numpad -` keep the prior unconditional select-all/clear
behavior for users who want it without a prompt. `Numpad *` (invert selection) is unchanged.
0.31 (2026-09-21) marks FOP-005 Implemented: `FileOps.h/.cpp`'s `CreateNewFile(parentDir, name)` creates a new,
empty file via `CreateFileW`'s `CREATE_NEW` disposition, which atomically refuses to overwrite an existing entry
of the same name (no separate exists-check/create race). Bound to `Shift+F7` (`F7` alone remains FOP-004's
create-directory), prompting for a name the same way `DoMakeDirectory` does.
0.32 (2026-09-21) marks FOP-009 Implemented — the only missing piece (per the last system-plan snapshot) was a
byte-level transferred-bytes/speed readout; source, destination, per-item progress, current item, and errors
were already shown. Added `FileOps.h`'s `ByteProgressHandler` and threaded it through `CopyItems`/`MoveItems`,
`CopySingleFile`, and `CopyDirectoryRecursive`, driven by `CopyFileExW`/`MoveFileWithProgressW`'s own progress
routine (renamed the shared context/callback from `CopyCancelCtx`/`CopyProgressRoutine` to
`TransferCancelCtx`/`TransferProgressRoutine` since it now serves both). `main.cpp`'s `ProgressUi` tracks a
smoothed transfer rate across consecutive samples and shows "current bytes/total bytes  speed/s" alongside the
existing item name/count once a file is large enough for the OS to report incremental progress on it. Switching
Move's file fast path from `MoveFileExW` to `MoveFileWithProgressW` (same flags/semantics) also gives a
cross-volume file move the same Esc-responsiveness a Copy already had, at no cost to the existing same-volume
rename fast path.
0.33 (2026-09-21) refines FOP-009 (still Implemented — this is a presentation change, not a new gap): the
transferred-bytes/speed readout added in 0.32 originally overlaid a single line at the bottom of the screen;
moved it into a proper centered modal-style box (title "Copying"/"Moving", `Source:`/`Destination:` lines,
`Item N of M: name`, and the bytes/speed line once available, plus `Esc to cancel`), matching the app's other
dialogs (`ShowChoiceDialog`/`ShowMessage`/`ShowTextPrompt`) instead of standing out as a one-off status-bar
overlay. Added `Dialog.h`'s `ShowProgress` (draws the box and returns immediately, unlike the other three, which
block until dismissed — the progress display must keep redrawing on every tick while the caller's own loop polls
for Esc). `Delete`/`ToggleAttributes`' progress dialogs share the same box, labeled `Location:` since they have
no separate destination.
0.34 (2026-09-23) marks PER-005 Implemented after fixing a real, previously-unbounded cost rather than just
observing that `CopyFileExW`/`MoveFileWithProgressW`'s own progress callback "should be cheap": that OS callback
fires at a rate driven entirely by the OS's internal I/O buffer size (potentially very frequent — small buffers,
network shares), and every single call unconditionally ran the caller's `onByteProgress` plus a
`PeekConsoleInputW`/`ReadConsoleInputW` cancel-poll syscall pair, so an unthrottled callback rate could itself
measurably slow a fast transfer regardless of how cheap any one caller's own redraw throttling (already present
in `main.cpp`'s `ProgressUi`, but that only throttled the *draw*, not the syscalls before it) tried to be. Added
a rate cap directly in `FileOps.cpp`'s shared `TransferProgressRoutine`/`TransferCancelCtx` (~20 Hz,
`kProgressPollIntervalSeconds`), always still firing on the very first sample and on completion, so this holds
for any caller — not just `main.cpp`'s UI — and Esc-cancel latency (PER-002) stays far below any perceptible
threshold. Added `Copy_SlowByteProgressCallback_DoesNotSignificantlyIncreaseTransferTime_PER005` to
`FileOpsTests.cpp`, which gives `onByteProgress` an artificial per-call sleep (standing in for a caller doing
real work, e.g. a console redraw) and asserts both the call count and the added wall-clock time stay bounded on
a ~20 MB copy — a regression that reintroduced the unthrottled rate would fail this test by ballooning both.
0.35 (2026-09-23) marks ACC-001 Implemented after closing the two remaining color-only status gaps a prior
system-plan snapshot had already flagged ("directories and selection are still primarily color-coded in the
listing, though the status bar also states the selection count as text"): directory status in the main panel
listing already had a text marker (`<DIR>` in the size column, from an earlier LIST-006/UI-004 pass), but
selection did not — a selected entry was distinguished only by `kAttrSelected`'s color. Added a leading `"* "`/
`"  "` marker (plain ASCII, shown regardless of ACC-006's basic-symbol mode) in `main.cpp`'s `DrawPanel`, folded
into the existing narrow-terminal column-width budget so it doesn't regress UI-004's graceful column-dropping.
Audited every other `kAttr*`-driven distinction in the app for the same gap and found one more: the Find Files
(Alt+F7) results picker distinguished a directory result from a file result by `kAttrDir`'s color alone, with
no equivalent of the main panel's `<DIR>` marker — fixed with a trailing `\` on directory results, the ordinary
Windows convention for a directory path. Dialogs, the status/hint bars, and the Help/Viewer screens were
already text-driven regardless of color (severity/state is always stated in the message text, not colored
differently by state), so no further gaps found there.
0.36 (2026-09-23) marks ACC-002 Implemented after a real contrast audit, replacing the prior "default console
colors used; not audited for contrast" note with actual WCAG 2.x math rather than eyeballing it. Added
`ColorContrast.h` (pure, header-only relative-luminance/contrast-ratio functions against the console's real
default 16-color palette, unit-tested independently of the app's own color choices) and consolidated every
color constant previously duplicated across `main.cpp`/`Dialog.cpp`/`Help.cpp`/`Viewer.cpp` into one shared
`Theme.h` (each file's own call sites are unchanged — same unqualified names, just defined once). The audit
found the exact same low-contrast pattern independently copy-pasted into three files: `kAttrStatusBar` (the
bottom status/hint bar, light gray on green) computed to ~2.8:1, below WCAG AA's 4.5:1 minimum for normal
text — fixed by adding `FOREGROUND_INTENSITY` (bright white instead of light gray), raising it to ~5.1:1
without changing the green-background look. Also found one combination Theme.h's own table can't see because
it's formed dynamically at the call site: a broken/inaccessible entry (`kAttrBroken`) under the cursor in the
active panel combines with the ordinary cursor highlight (`BACKGROUND_BLUE`) to ~4.0:1 — fixed in `main.cpp`'s
`DrawPanel` by special-casing that one combination to the already-accessible white-on-dark-red pairing
`kAttrBrokenBanner` uses (~11.0:1), which keeps the "broken" signal at least as strong. Added
`ColorContrastTests.cpp`: correctness tests for the luminance/contrast math itself against known WCAG
reference values, a table-driven test asserting every one of Theme.h's 14 defined attributes individually
meets the 4.5:1 minimum (so a future low-contrast addition fails the build instead of going unaudited a
fourth time), and a test covering the cursor-row color-masking combinations main.cpp actually forms at
runtime.
0.37 (2026-09-23) marks ACC-003 Implemented after auditing every user-facing string literal in the app (not
just the ones already suspected) and moving the ones genuinely interleaved with control-flow logic into a new
`Strings.h` — the same "one header owns the display copy, separate from the logic deciding when to show it"
shape `HintBar.h` (context -> hint line) and `Theme.h` (color constants) already used successfully; `Strings.h`
does the equivalent job for everything else, mixing plain constants (titles/labels/messages with no
interpolated data) and small `Format*` functions (for sentences that interpolate a path/name/count, so the
English word order stays in one place rather than being reassembled at each call site). Migrated: `main.cpp`
(the majority — dialog titles/prompts/labels, status/progress/error text, drive/sort/volume labels), `FileOps.cpp`
(the fixed wording inside its composed error messages — paths and system messages stay as parameters),
`Panel.cpp` (status-bar messages), `Config.cpp` (the config-file parse-warning sentence templates — the
config keys/values interpolated into them stay excluded, see below), `Dialog.cpp`, `Viewer.cpp`, and `Help.cpp`'s
fixed header/status chrome. Deliberately left alone, with the reasoning recorded in `Strings.h`'s own header
comment: `HintBar.h`'s hint-line table and `Help.cpp`'s `Sections()` keyboard-shortcut table (both already
self-contained, already-separated data, not entangled with logic — moving their entries would have fragmented
two already-correct tables rather than consolidating anything); Config/Session/CommandHistory's own file-format
keys and values (data-format identifiers a user reads/writes verbatim, not prose — the same category as a JSON
key name); `Log.cpp`'s log level names and every `logger.Log(...)` call site's message text (a diagnostic/support
artifact, conventionally kept in English/technical form regardless of UI language, the same as a stack trace);
`Elevation.cpp`'s `--elevated-*` IPC flags between the app and its own elevated re-invocation; and Win32 API
parameters that happen to be string literals (`ShellExecute` verbs, `%EDITOR%`/`%COMSPEC%` variable names,
`notepad.exe`/`cmd.exe` fallback executable names) — none of these are language-dependent text a user reads and
interprets. Runtime locale switching (actually loading a non-English catalog) is explicitly out of scope here;
that's what ACC-005 (LATER) would build on top of this separation, not something this change needed to build.
0.38 (2026-09-23) marks EXT-001 Implemented after auditing actual module boundaries for the same gap CON-004's
note already named ("`Panel` and `FileOps` still call `std::filesystem` directly rather than through a
swappable file-system abstraction") — but interpreted EXT-001's literal text ("should have defined boundaries")
as the achievable bar it actually is, not as "build a runtime-swappable provider interface now." A full
`IFileSystemProvider` abstraction with no second concrete implementation to prove it against would be exactly
the premature-abstraction EXT-002's own postponement note already warns against ("should follow stabilization
of the local provider interface") — building the interface before there's a second provider to abstract over
tends to guess wrong and need rework later, not stabilize anything. Audited every module for a genuine boundary
violation instead of a hypothetical one: found exactly one — `main.cpp` (Core UI) called
`std::filesystem::exists`/`is_directory`/`create_directories` directly in `DoCopyOrMove` to check and prepare a
Copy/Move destination, the one place the UI layer reached past `FileOps` (file-operation services) into the
file system itself. Every other boundary already holds: `FileOps` has zero dependency on `Console`/`Panel`/
`Dialog`; `Console` is the sole owner of console Win32 calls (`WriteConsoleOutputW`/`SetConsoleMode`/etc.) —
nothing outside `Console.cpp` calls them; `Process`/`Elevation` are the sole owners of process-spawning/elevated
re-invocation.
Added `FileOps.h`'s `CheckCopyMoveDestination`/`CreateDestinationDirectory` — the file-system touches move here,
while the UI decision for each outcome (ask the user, show which error) stays in `main.cpp`, matching how every
other `FileOps` entry point never makes a UI choice itself. `main.cpp` no longer includes any direct
`std::filesystem` *operation* (only the ordinary `std::filesystem::path` value type, which isn't a file-system
touch), and its now-unused `ToLongPath`/`ToWideMessage` `using` declarations were removed. Added 5 new
`FileOpsTests.cpp` cases covering both functions' three/two outcomes each.
0.39 (2026-09-23) marks TST-005 Implemented — was Not Started, since `main.cpp`'s whole UI/input-loop layer has
never had test coverage in this project (it isn't even part of `MyCommanderTests.vcxproj`'s compile list),
unlike `Panel`/`FileOps`/`Search`/`TextFile`, which are all deliberately `Console`-independent and unit-tested.
Rather than building a full mock-console harness to drive `wmain`'s real input loop end-to-end (a much larger,
riskier undertaking with limited payoff, since most of that loop is Win32 I/O rather than decision logic),
pulled the actual *decision* logic for navigation and for the two general-purpose dialog primitives out of their
Console-coupled input loops into pure, directly-testable functions — the same "separate the logic from the I/O"
move `TextFile`/`Search` already made successfully for their own areas. Added `Navigation.h/.cpp`:
`ClassifyNavigationKey` reproduces the exact key-dispatch decisions `wmain`'s switch makes for Tab/panel-history/
command-history-browsing-vs-cursor-movement/Backspace (including the original's non-obvious edge case —
Ctrl+Up with no command history to browse is a no-op, it does not fall back to moving the cursor), and `wmain`'s
switch cases now call it and act on the result rather than re-implementing the same conditionals inline. Added
`Dialog.h`'s `MatchDialogHotkey` (`ShowChoiceDialog`'s case-insensitive hotkey match) and `ApplyTextPromptKey`
(`ShowTextPrompt`'s per-keystroke text-editing decision: Esc/Enter/Backspace/printable-char-append-with-240-cap),
both used by these "critical dialogs" throughout the app (every conflict/error/confirmation/rename/create prompt).
Split their definitions into a new `DialogInput.cpp` (declared in the same `Dialog.h`, called by `Dialog.cpp`)
rather than leaving them in `Dialog.cpp` itself, so the test project can compile and test them directly without
pulling in `Dialog.cpp`'s (and therefore `Console.cpp`'s) real Win32 console dependency — the same reasoning
already applied to `Navigation.cpp`. Added `NavigationTests.cpp` (15 cases covering every classified key and its
guard conditions) and `DialogInputTests.cpp` (14 cases covering both functions' full behavior) — 29 new tests
total, all exercising the exact same decision logic the real UI runs, repeatably and without a live console.
0.40 (2026-09-23) marks UI-005 Implemented at the user's explicit request, ahead of its original `LATER`
disposition (moved to `IN-MVP` accordingly, since leaving a shipped feature marked `LATER` would be
self-contradictory; removed from Section 24's postponement-candidates table, which now lists only UI-010 under
"Custom themes"). Added `Config`'s `visibleColumns` (an ordered subset of `{Type, Attributes, ModifiedTime,
Size}`; Name is always shown first and isn't itself a column choice) and `typeColumnWidth`/`mtimeColumnWidth`/
`sizeColumnWidth` (Attributes stays a fixed 4 — one slot per R/H/S/A flag, not meaningfully resizable). Each
width is validated against a hard minimum below which its own content would truncate (type >= 6, mtime >= 16
for the fixed "YYYY-MM-DD HH:MM" format, size >= 10) and rejected (CFG-005 warning, default kept) rather than
silently clamped. `main.cpp`'s `DrawPanel` generalizes UI-004's old hardcoded two-tier full/basic column drop
into an N-tier one (`FitColumnsToWidth`) that drops one column at a time from the end of the configured
`visibleColumns` order under space pressure — the user's chosen order doubles as a drop priority. Documented
behavior change for the unchanged default order: size is now the first column dropped on a narrow terminal
(then modification time, then attributes, leaving type until only name fits), rather than the old hardcoded
"type and modification time drop together, attributes and size survive" split — a deliberate refinement (more
granular, transparently driven by configuration) rather than an oversight, since the old two-tier behavior was
never itself a documented guarantee beyond README prose. `Config` is loaded once at `wmain` startup and never
reloaded during a session (no in-app settings UI, matching every other setting), so the resolved column layout
is captured into module-level state at that point rather than threaded through `DrawFrame`'s ~20 call sites.
Added 9 `ConfigTests.cpp` cases covering parsing, defaults, case-insensitivity, duplicate collapsing, and each
width's minimum-rejection path; the `DrawPanel` layout math itself has no test coverage, consistent with
`main.cpp`'s drawing code never having any in this project (see ACC-001/ACC-002/TST-005's precedent).
0.41 (2026-09-23) marks UI-010 Implemented, at the user's explicit request, ahead of its original `LATER`
disposition — moved to `IN-MVP` accordingly (same reasoning as UI-005's 0.40 entry) and removed from Section
24's postponement-candidates table entirely, since it was the row's only remaining ID (the "Custom themes" row
itself is now gone, not just trimmed). Built directly on ACC-002's contrast-audit infrastructure: `Theme.h`
gained a `ThemePalette` struct bundling every theme-dependent color as one unit, `kDefaultThemePalette`
(unchanged values from ACC-002) and `kHighContrastThemePalette` (a genuine second theme, not just a
relabeling — pushes every pairing that only just cleared WCAG AA under the default theme to the maximum
achievable ~21:1: plain text and the inactive header go from light-gray-on-black to white-on-black, and the
status bar drops its green background entirely, since green's own luminance caps that pairing at ~5:1
regardless of foreground choice). Landmark backgrounds that were already excellent (dialog chrome's dark blue
~16:1, the broken-location banner's dark red ~11:1) are kept unchanged in both themes, since they're already
well past any accessibility guideline and the color itself carries real meaning; `attrBroken` stays red-on-black
(~5.3:1, the physical ceiling for a red-only foreground against black) in both themes too, since its real
accessibility guarantee is the `<BROKEN>` text tag ACC-001 already added, not its color — the same reasoning
ACC-001 established generally, applied consistently here rather than re-litigated.
Every draw call site keeps reading the exact same `kAttrNormal`/`kAttrDir`/etc. names as before (now mutable
module state rather than true `constexpr`, reassigned once by the new `ApplyThemePalette` from `wmain` right
after `Config` loads — the same one-time-at-startup treatment UI-005's column settings already got) rather than
renaming all ~90 call sites across `main.cpp`/`Dialog.cpp`/`Help.cpp`/`Viewer.cpp` to a `g`-prefixed name; the
resulting `k`-prefix/mutability mismatch is a deliberate, documented trade-off (see `Theme.h`'s own comment),
not an oversight. Added `Config::colorTheme` (`default`/`highcontrast`, same `LoadConfigFrom`/CFG-005 warning
convention as every other enum setting) and documented it in the default config file.
Rewrote `ColorContrastTests.cpp` to audit both named palettes directly (`kDefaultThemePalette`/
`kHighContrastThemePalette`, as data — 14 fields each) rather than only whichever theme happened to be
currently live, fixing a `constexpr`-array compile break the mutability change caused along the way; added a
test asserting the high-contrast theme's three actually-changed fields each land near the 21:1 maximum and
exceed the default theme's own ratio for the same field, so the theme is provably a real improvement, not a
relabeling. Added 5 `ConfigTests.cpp` cases for `colorTheme` parsing.
0.42 (2026-09-24) fixes IS-0001 (`AIPrompt/issue_fix_requests.md`; plan in
`AIPrompt/fixes/IS-0001-fixplan.md`): `F7`/`Shift+F7` (FOP-004/FOP-005, both already
Implemented — this is a bug fix within an already-shipped feature, not a status change) now
select the newly created directory/file once the panel refreshes, instead of leaving the
cursor wherever it happened to be beforehand. Root cause had two layers: `main.cpp`'s
`DoMakeDirectory`/`DoCreateNewFile` called the no-argument `Panel::Refresh()` (which only ever
preserves the *existing* cursor position) instead of `NavigateTo(path, name)` (which can
select a specific name); and even fixing just that call site wouldn't have worked, because
`Panel::SetLocation`'s `selectName` lookup searched `entries_` immediately after starting a
refresh, which for both of the app's real panels (`incrementalLoading=true`, LIST-008) is
still empty at that point — the listing is filled in over later `PumpRefresh()` calls. Added
`Panel`'s `pendingSelectName_`, consumed by `BeginRefresh()`/`Refresh()`'s synchronous branch
and applied by the existing `FinishRefresh()` cursor-restore logic once a listing is actually
complete. This same latent gap silently affected `DoSearch`'s jump-to-a-result (`Alt+F7` ->
`Enter`, SRC-002) whenever the destination needed a real listing pass, fixed as a side effect
of the shared root cause rather than as separate scope. Not caught by the existing suite
because `PanelTests.cpp` only exercised the non-incremental `Panel` constructor, where the old,
broken `selectName` lookup happened to work by accident (the listing was already complete
by the time it ran) — added 4 new `PanelTests.cpp` cases that explicitly use
`incrementalLoading=true` to exercise the path that was actually broken, including one
confirming the ordinary "preserve wherever the cursor already was" behavior (LIST-005) still
works once `pendingSelectName_` is introduced as a separate, higher-priority mechanism.
0.43 (2026-09-24) implements IS-0002 (`AIPrompt/issue_fix_requests.md`; plan in
`AIPrompt/fixes/IS-0002-fixplan.md`): `F4` (`VEE-006`, already Implemented — a behavior
refinement, not a status change) now launches the external editor and returns immediately by
default, instead of blocking until it closes. Added `Process.h`'s `RunDetached` (`CreateProcessW`
without waiting, returning a process handle instead of an exit code) and `Config::
waitForEditorToClose` (default `false` — a deliberate default-*behavior* change, unlike every
other `Config` field added so far, since this matches Total Commander's own default and the
user explicitly asked for it; `true` restores the original blocking path unchanged, including
`Console::SuspendForChildProcess`/`ResumeAfterChildProcess`, for a console-subsystem `%EDITOR%`
sharing this console window, which the non-blocking path doesn't hand off to — an accepted,
documented limitation rather than solved here). Since the app no longer learns the file
changed from "the editor just exited," added a Windows file-change-notification watch
(`FindFirstChangeNotificationW`/`FindNextChangeNotification` on the file's parent directory,
tied to a `PendingEdit` per background edit) polled non-blockingly (`WaitForSingleObject(...,
0)`) once per main-loop iteration — the same ~16 ms cadence `LIST-008`'s incremental loading
already relies on, so no new wait/timing mechanism was needed. On a change, the edited file's
panel entry is refreshed and re-selected via `NavigateTo(path, name)`, directly reusing
IS-0001's `Panel::pendingSelectName_` fix. A watch's lifetime is tied to its editor process's
own handle (polled alongside the notification handle) so it can't accumulate unboundedly
across a session of editing many files in turn. Added `ProcessTests.cpp` (new file — `Process.cpp`
had zero prior test coverage) covering `RunDetached`'s success/failure paths against a real
short-lived process, and 3 `ConfigTests.cpp` cases for `waitForEditorToClose` parsing.
0.44 (2026-09-24) marks UI-012 Implemented at the user's explicit request, ahead of its
original `LATER` disposition (moved to `IN-MVP`, and removed entirely from Section 24's
postponement-candidates table, same treatment UI-005/UI-010 got). `Console`'s constructor now
enables `ENABLE_MOUSE_INPUT` (previously deliberately omitted — see the constructor's own
updated comment, and `CLAUDE.md`'s Source-layout entry for `Console`, both corrected to match).
KEY-001's invariant ("all MVP functions available without a mouse") is preserved not by
withholding mouse events anymore, but because every mouse-triggered action added here already
has an existing keyboard equivalent — mouse support is purely additive, matching UI-012's own
"without being required" wording. Scoped to exactly what the requirement names — the two main
file panels' selection, scrolling, and activation — not every screen: dialogs, the drive/search
pickers, the viewer, and the help screen stay keyboard-only, a deliberate, documented scope
boundary (the same kind of proportionality call as EXT-001's). Left-click moves the cursor to
the clicked entry and, if the click landed in the other panel, makes that panel active
(matching Total Commander's own click-to-activate convention); the mouse wheel moves the
active panel's cursor 3 rows per notch, regardless of which panel the wheel was over — the same
"always acts on the active panel" rule every other unmodified navigation key already follows;
a double-click additionally activates the entry, identical to pressing Enter on it.
Extracted `ActivateCursorEntry` (`main.cpp`) out of `VK_RETURN`'s own handler — the
directory-vs-file-default-action logic Enter already had — so a double-click reuses the exact
same code path rather than a second, parallel copy of the same switch. Added `Navigation.h`/
`.cpp`'s `HitTestPanel`/`PanelHit` (which panel and which visible row a mouse event's
console-cell coordinates landed on) as a pure function of width/visibleRows/mouse coordinates,
following the same testability reasoning `ClassifyNavigationKey` (TST-005) already established
— its geometry mirrors `DrawFrame`/`DrawPanel`'s own layout by hand, since neither currently
exposes it as reusable data. Added 9 `NavigationTests.cpp` cases for `HitTestPanel` (each panel,
each border, the rows above/below the visible area, the first/last valid row).
0.45 (2026-09-24) implements IS-0003 (`AIPrompt/issue_fix_requests.md`; plan in
`AIPrompt/fixes/IS-0003-fixplan.md`): three additive extensions to UI-012's mouse support
(already Implemented — behavior extensions, not a status change). (1) Drag-and-drop between
the two panels: dragging a selection (or the cursor entry, if nothing is selected) from one
panel and dropping it on the other copies it there by default, or moves it if Shift is held at
the moment of drop. Since Windows console mouse events carry no distinct "button released"
event, a release is inferred by noticing `FROM_LEFT_1ST_BUTTON_PRESSED` disappeared from
`dwButtonState` between one event and the next — `main.cpp`'s new `DragState` tracks
press-candidate/dragging/drop across consecutive `HandleMouseEvent` calls the same way
`PendingEdit` (IS-0002) already tracks state across main-loop iterations. Factored the
destination-check-through-`ReportOutcome` half of `DoCopyOrMove` out into a shared
`PerformCopyOrMove`, reused by both `DoCopyOrMove` (F5/F6, prompts for a destination) and the
drop handler (which already knows the destination — the panel dropped on — and skips the
prompt). A status-bar line ("Dragging N item(s) — release to copy, hold Shift to move") shows
while dragging, since the console has no drag "ghost" rendering otherwise. (2) Clickable hint
bar: `HintBar.h`'s `BuildHintBar` is now derived from a new `BuildHintSegments`, pairing each
hint's label with an optional `HintAction` (virtual-key code + modifiers) — every single-key
hint is clickable; a hint naming two different keys as a slash pair ("Ctrl+Up/Down History",
"Alt+Left/Right History", "Alt+F1/2 Drive") stays informational-only, since a click can't
unambiguously pick which key was meant. A click is turned into the real action via a new
`SimulateKeyPress` (`main.cpp`), which injects a synthetic key-down/key-up `INPUT_RECORD` pair
into the console's own input queue (`WriteConsoleInputW`) — picked up by the main loop's own
next `ReadConsoleInputW` call and dispatched through the existing ~600-line key-dispatch switch
unchanged, rather than a second, parallel copy of it. (3) Clickable dialog option buttons:
`ShowChoiceDialog` now also accepts a click on a rendered "[X]Label" option, selecting it the
same as its hotkey would; `ShowMessage` now dismisses on a click anywhere, matching its "any
key" dismissal. Added `Navigation.h`/`.cpp`'s `ClickableRegion`/`MatchClickRegion`, a generic
row/column-range hit-test shared by both the hint bar and the dialog options (rather than two
near-duplicate implementations), and `DialogInput.cpp`'s `BuildOptionRegions` (the pure column-
span layout of `ShowChoiceDialog`'s own options line). `ShowTextPrompt` (no buttons, just a
text field) and `ShowProgress` (non-interactive) are unchanged — explicitly out of scope, not
an oversight. Added `BuildHintSegments`/ambiguous-pair coverage to `HintBarTests.cpp`,
`MatchClickRegion` coverage to `NavigationTests.cpp`, and `BuildOptionRegions` coverage to
`DialogInputTests.cpp`.
0.46 (2026-09-25) implements IS-0004 (`AIPrompt/issue_fix_requests.md`; plan in
`AIPrompt/fixes/IS-0004-fixplan.md`): mouse-driven file selection, extending UI-012 (already
Implemented) and SEL-001/002 (already Implemented via keyboard) rather than completing a
still-open requirement. Right-click toggles the clicked entry's selection mark (new `Panel::
ToggleEntrySelected`); holding the right button and dragging extends the selection to every
further entry the mouse passes over, applying the drag's target state rather than re-toggling on
each visit (new `Panel::SetEntrySelected`, and `main.cpp`'s `SelectDragState`, mirroring
`DragState`'s (IS-0003) own cross-event press/move/release shape — the same reasoning applies,
since Windows console mouse events still carry no distinct "button released" event). Deliberately
the *right* button, not a second use of the left button, since a plain left-drag already means
copy/move (IS-0003) — using it for selection too would make left-drag ambiguous. A right-drag
stays confined to the panel it started in (selection is a per-panel concept, unlike copy/move's
deliberately cross-panel drag). Shift+left-click selects every entry between wherever the cursor
already was and the clicked entry (new `Panel::SelectRange`) — additive (entries outside the
range keep whatever selection they already had), matching `SelectByMask`'s existing convention,
rather than Explorer's "replace the whole selection" behavior; the range's other endpoint is
always the cursor's position at click time, not a separately remembered anchor that survives
across multiple Shift+clicks the way Explorer's does — a deliberate simplification needing no new
persisted `Panel` state, documented as a known difference rather than a bug. Every resulting
selection remains fully reachable from the keyboard alone (Insert/mask-select/select-all/invert),
so KEY-001's invariant is preserved the same way UI-012/IS-0003 already established it. Added 10
`PanelTests.cpp` cases (`_IS0004` suffix) covering `ToggleEntrySelected`/`SetEntrySelected`/
`SelectRange`, including the "never selects `..`" rule and the "leaves entries outside the range
untouched" behavior; no automated coverage of `HandleMouseEvent`'s new right-button dispatch
itself, consistent with `main.cpp`'s established limit.
0.47 (2026-09-25) implements IS-0004's addendum (`AIPrompt/issue_fix_requests.md`; plan in
`AIPrompt/fixes/IS-0004-fixplan.md`, Section 4): Shift+right-click now selects a range between
two right-clicked entries, the same outcome Shift+left-click already gave, entered via the right
button instead. Needed almost no new code: a right-click already moves the cursor to the clicked
entry before toggling it, so the "first" right-clicked entry is still sitting at `Cursor()` when
the second, Shift-held right-click arrives — exactly the anchor `Panel::SelectRange` (added for
Shift+left-click) already expects, so this reuses it unchanged rather than adding a new `Panel`
method. `main.cpp`'s right-button press branch now captures the cursor position before moving it,
and calls `SelectRange` instead of `ToggleEntrySelected` when Shift is held, returning without
starting a `SelectDragState` — a single discrete gesture, not draggable, matching Shift+left-
click's own shape. No new `PanelTests.cpp` cases were needed, since the addendum calls the
already-tested `SelectRange` unmodified; the full existing suite (315 cases) was re-run to
confirm no regression.
0.48 (2026-09-25) switches IS-0004's two range-select gestures (0.46's Ctrl... originally
Shift+left-click, and 0.47's Shift+right-click) from Shift to **Ctrl**, after the user reported
Shift+right-click "not sensed by the software at all," with no popup appearing either. Follow-up
established: Shift+left-click showed the identical symptom, and the user runs the classic
console host (`conhost.exe`, not Windows Terminal) — since both buttons are affected identically
and nothing visibly intercepts the click, classic console host is silently swallowing a
Shift-held mouse click before it ever reaches the app's input queue, which no `SetConsoleMode`
flag can override from inside the app. Per the user's explicit choice (offered three options:
Ctrl-only, Ctrl-plus-keep-Shift, or something else), both `main.cpp` `dwControlKeyState` checks
now test `LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED` instead of `SHIFT_PRESSED`; `Panel::SelectRange`
itself is unchanged, since it never knew which modifier its caller used. IS-0003's unrelated
Shift-to-mean-move-at-drop (a button *release* during an already-active drag, not a fresh click)
was deliberately left alone, since it hadn't been reported broken and is a different kind of
event. Updated README/this spec's own IS-0004 prose to say Ctrl; the fix plan
(`AIPrompt/fixes/IS-0004-fixplan.md`) now leads with this as its own addendum, documenting the
diagnosis. No `PanelTests.cpp` changes were needed (the switch is entirely which `main.cpp`
condition calls the already-tested `SelectRange`); full suite (315 cases) re-run, 0 regressions.
0.49 (2026-09-25) implements IS-0005 (`AIPrompt/issue_fix_requests.md`; plan in
`AIPrompt/fixes/IS-0005-fixplan.md`): `Esc` no longer quits the app. Removed the `else { running =
false; }` branch from `wmain`'s own `VK_ESCAPE` case (`main.cpp`) — `Esc` with an empty command
line is now a no-op at the top level; `Esc` with a non-empty command line still clears it,
unaffected. `F10` is now the sole top-level quit key (it already set the identical `running =
false` flag independently, so nothing else needed to change there). Every other `Esc` meaning
elsewhere in the app — cancelling a dialog (`ShowChoiceDialog`/`ShowTextPrompt`, UI-009), closing
the drive/search-results picker, the viewer, or the help screen (each has its own local exit flag,
unrelated to the app-level one), and interrupting an in-progress file operation (PER-002's
`PeekEscapePressed`/`PollCancel`) — is unaffected; none of those mean "quit the app," so the fix
plan explicitly scoped them out rather than leaving the boundary implicit. Updated `Help.cpp`'s
shortcut-reference table and README's keybindings table together (per `CLAUDE.md`'s own note that
the two must mirror each other) to drop the "otherwise quit" wording from `Esc`'s row. No
requirement ID's `Implemented`/Disposition field changes — this was never tracked under its own
spec row. Grepped the test suite for `VK_ESCAPE`: the only hit was `DialogInputTests.cpp`'s
unrelated `ApplyTextPromptKey_Escape_Cancels` (`ShowTextPrompt`'s own dialog-cancel behavior) —
`wmain`'s key-dispatch switch has never had test coverage, so there was never a test asserting
"Esc quits" to remove; the issue's "and tests too" turned out to be a no-op here, not an
oversight.
0.50 (2026-09-25) implements IS-0006 (`AIPrompt/issue_fix_requests.md`; plan in
`AIPrompt/fixes/IS-0006-fixplan.md`): a self-healing config file, an `F11` hot-key to edit it, and
live reload on save. (1) `Config.cpp`'s fully-commented default-file text is now a
`ConfigKeyBlocks()` table (one comment-plus-default block per recognized key), the single source
both `EnsureConfigFileExistsAt` (joins every block for a brand-new file) and the new
`AppendMissingConfigKeys` (appends only the ones missing from an *existing* file) build from.
`LoadConfigResult` gained `presentKeys`, tracking which recognized keys a file actually mentioned
(regardless of whether the value itself parsed — a typo'd value is a separate, already-handled
`warnings` case). Wired into `wmain` right after `Logger` is constructed: any key this version
knows about but an older file predates gets appended, with its own default and documentation
comment, and an Info-level log line — deliberately no popup, since an old file isn't a mistake the
user made. (2) `F11` (the next free function key after `F10`) opens the config file in the
external editor via a new `DoEditConfig`, mirroring `F4`'s `DoEdit` launch logic but always
detached (blocking would defeat point 3). Added to the `F1` shortcut reference and README's
keybindings table; deliberately *not* added to the bottom hint bar's per-context segments (a rare,
power-user action, not panel-contextual) — flagged as a judgment call, not a final decision. (3) A
new `ConfigWatch` (deliberately separate from IS-0002's `PendingEdit` — a config-file change means
"reload and re-apply settings," not "refresh a panel listing," a different reaction entirely,
mirroring why IS-0004 kept `SelectDragState` separate from `DragState`) watches the config file the
same way `PendingEdit` watches an edited panel file; `PollConfigWatch` reloads `Config` and applies
it live via a new `ApplyConfigLive` on a save or the editor's exit — traced every `Config` field's
call sites to find which were already "live" via by-const-reference reads at each call site
(most were) versus consumed only once at startup: `groupDirectoriesFirst`/`useBasicSymbols` now go
through `Panel`/`Console`'s own pre-existing setters, `visibleColumns`/the three column widths
reassign `main.cpp`'s module-level globals directly, `colorTheme` re-runs `ApplyThemePalette`, and
`logLevel` uses a new `Logger::SetLevel` (`Log.h`) added specifically for this, since `Logger` had
no way to change its threshold after construction. A reload's own parse warnings reuse the exact
same startup warning dialog. Deliberately does not re-run the missing-key backfill on this path —
startup-only, so deleting a key while live-editing isn't silently fought by the app between
keystrokes. Added `ConfigTests.cpp` cases for `presentKeys`/`AppendMissingConfigKeys` (including
the "existing lines/values stay untouched" and "no-op when nothing's missing" behaviors) and one
`LogTests.cpp` case for `Logger::SetLevel`; `ApplyConfigLive`/`PollConfigWatch`/`DoEditConfig`
themselves get no automated coverage, consistent with `main.cpp`'s established limit.
0.51 (2026-09-25) two corrections to IS-0006, both reported directly by the user testing it: (1)
plain `F11` never reaches the app on most terminal hosts (Windows Terminal included), which
reserve it as a window-fullscreen toggle — the same class of host interception IS-0004 hit with
Shift-held mouse clicks. Switched the hot-key to `Ctrl+F11` (`main.cpp`'s `case VK_F11:` now
gates on `ctrlHeld`); updated `Help.cpp`'s shortcut reference and README everywhere `F11` was
mentioned. (2) Every one of `Config.cpp`'s `ConfigKeyBlocks()` comment blocks now ends with its
own explicit `# Values: ...` line (e.g. `# Values: true / false (default: true)`) stating exactly
what's acceptable for that setting and its default — previously only the enum-valued settings
(`enterFileAction`, `logLevel`, `colorTheme`) spelled this out per-setting; boolean settings relied
on a single generic "Boolean values: true/false" line in the file's header, and
`mtimeColumnWidth`/`sizeColumnWidth` had no comment of their own at all (sharing
`typeColumnWidth`'s). This also makes `AppendMissingConfigKeys` more useful: a single backfilled
key's block is now self-contained even far from the file's own header. Updated README's sample
config block to match. No `Panel`/`Console`/parsing behavior changed — comments are inert to
`LoadConfigFrom`; the full test suite was re-run to confirm.
0.52 (2026-09-25) IS-0007: the bottom hint bar (`UI-008`) was hard-coded to exactly one console
row, silently truncating whichever commands didn't fit — reproducible even at the documented
60-column minimum (`UI-006`) for the two longest contexts (`Directory`/`EmptyListing`, ~155
columns of segments each). Added `HintBar.h`'s `LayOutHintBar`/`HintBarRowCount`, generalizing
the existing single-row segment-column walk (`MatchHintBarClick`'s own, IS-0003) into one that
wraps to a new row (capped at `maxRows = 3`) whenever the next segment wouldn't fit, rather than
truncating. `main.cpp`'s `DrawFrame` now reserves `2 + hintRows` bottom rows instead of a literal
`3` and draws each wrapped row; `ComputeVisibleRows` (panel paging, every mouse hit-test call
site) takes the same `HintContext` and must agree with `DrawFrame` on the row count, or clicks/
paging would target rows that don't match what's drawn — every call site in `main.cpp` was
updated together. `MatchHintBarClick` rewritten to match a click against whichever wrapped row it
landed on. Row count is recomputed on every `DrawFrame` call (not just on resize) — a `HintContext`
change (selecting an item, typing into the command line) re-lays-out the hint bar exactly like a
resize does, since both reach `DrawFrame` through the same path; this also satisfies the issue's
"when resized, reset the size" ask without separate resize-specific code. Added 7 `HintBarTests.cpp`
cases for the new pure layout functions (including the exact `Directory`-at-60-columns regression
case: every segment present, multiple rows used); `MatchHintBarClick`'s row-aware click matching
itself stays unverified by an automated test, consistent with `main.cpp`'s established limit for
key/mouse dispatch glue — checked live instead (resize correctly triggers the `UI-006` too-small
threshold, column-narrowing takes effect, no crash across repeated resizes; the wrapped hint rows
themselves couldn't be screen-captured in this environment, a `PrintWindow`-vs-Windows-Terminal
composited-content limitation hit repeatedly this session, not a functional gap).
0.53 (2026-09-25) IS-0007 addendum: user-reported (with a screenshot) blank/useless green row
sitting above the hint bar in the default "nothing selected" browsing state. Root cause was
pre-existing, not introduced by the row-wrapping fix above: `BuildStatusLine` returns an empty
string in that state, but `DrawFrame` always drew that empty string as its own reserved row using
the hint bar's own green background, so the two visually merged — only newly noticeable once the
hint bar itself grew past one row. Made the status row conditional: `DrawFrame` now computes
`hasStatusLine = !status.empty()` and only reserves/draws that row when true (reserved-row formula
`1 + (hasStatusLine ? 1 : 0) + hintRows`); `ComputeVisibleRows` gained the same parameter and
formula, kept in agreement with `DrawFrame` for the same reason `hintRows` itself had to be; every
call site in `HandleMouseEvent` and `wmain`'s key-dispatch loop was updated together. No
`Implemented`/Disposition change; full test suite re-run with no regression (329 cases, 11,508
checks).
0.54 (2026-09-25) KEY-004: added an "About" section at the top of the F1 help screen (product
name, version, description, company, copyright). `Help.cpp` reads these at runtime from the
running exe's own embedded Win32 `VERSIONINFO` resource (`GetFileVersionInfoW`/`VerQueryValueW`
against `MyCommander.rc`'s `StringFileInfo` block, `040904B0` — the same data Explorer's
Properties > Details tab and `--version` draw from) rather than duplicating those strings a
second time by hand; falls back to `Version.h`'s compile-time `MC_VERSION_STR` for the version
field only if the resource can't be read. The result is cached in a function-local static read
once per process, and reused as the first entry `Sections()` returns — no change to
`ShortcutItem`/`ShortcutSection`'s shape or to any other section. Confirmed the values read match
the exe's actual embedded resource via `Get-Item ... .VersionInfo`. No `Implemented`/Disposition
change (`KEY-004` was already Implemented — this adds to it, not a status change); full test
suite re-run with no regression (329 cases, 11,508 checks).
0.55 (2026-09-25) IS-0008: the F1 shortcut reference (`KEY-004`) ignored the mouse wheel entirely
— `Console.cpp` already enables `ENABLE_MOUSE_INPUT` for the whole app, so `MOUSE_EVENT` records
reached `Help.cpp`'s `ShowShortcutReference` input loop, but its own filter
(`record.EventType != KEY_EVENT`) discarded them unconditionally. Added a `MOUSE_EVENT`/
`MOUSE_WHEELED` branch ahead of that filter, scrolling `topLine` by 3 rows per notch — the same
sign convention and granularity `main.cpp`'s dual-pane wheel scrolling already uses (IS-0003) —
then reusing the screen's existing `clampTop`/`paint` machinery exactly as every keyboard scroll
case already does. Scope deliberately limited to the help view, matching the issue's wording;
`Viewer.cpp` and the drive/search-results pickers have the identical keyboard-only limitation but
weren't touched. No `Implemented`/Disposition change (`KEY-004` was already Implemented); full
test suite re-run with no regression (329 cases, 11,508 checks).
0.56 (2026-09-25) FEA-0006: added `MyCommanderSetup.exe`, a native C++ installer (`DEC-002`/
`DEC-009` — no third-party installer framework, no MSI database), a third project in
`MyCommander.sln`. New `## 23. Installation Requirements` section (`INST-001` through `INST-006`,
all Implemented) — every subsequent section renumbered `## 24.` onward. Checks Windows
version/disk space before installing anything; installs per-user to
`%LOCALAPPDATA%\Programs\MyCommander` (no elevation, `SEC-002`); creates a desktop shortcut
(`IShellLinkW`); registers with Windows' Apps & Features (`HKCU\...\Uninstall`) with a working
uninstall path. `MyCommander.vcxproj`/`MyCommanderTests.vcxproj` switched to static CRT linking
(`/MT`/`/MTd`) so the installed exe has no Visual C++ Redistributable dependency to check for.
Every `MyCommanderSetup` build force-rebuilds `MyCommander` as `Release|x64`
(`Tools/RebuildReleaseForInstaller.ps1`, `INST-006`) and embeds it as an `RCDATA` resource, so the
installer always ships a fresh build regardless of which configuration it's itself built in. New
`SetupLogic.{h,cpp}` (pure — install-path defaulting, version-support check, uninstall-registry
value construction) with 12 new `SetupLogicTests.cpp` cases; the OS-integration code
(`RtlGetVersion`, registry, `IShellLinkW`, resource extraction) has no automated coverage,
matching `Elevation.h`'s own `RunElevated` precedent — verified instead via a real install/
upgrade-detect/uninstall round trip on this machine, fully reversed by the same uninstall path
this feature adds. See `AIPrompt/features/FEA-0006-plan.md` for the full design and status.
**Primary platform:** Windows 10/11  
**Possible later platform:** Linux  
**Application type:** Text-based terminal file manager  

## 1. Purpose

This document defines the requirements for a keyboard-driven, text-based file explorer inspired by dual-pane file managers such as Total Commander.

The document is designed to support staged development. Every feature has a stable requirement ID, priority, and release disposition so that a feature can later be postponed without being removed or forgotten.

## 2. Product Vision

The application shall provide fast and safe file-management operations from a terminal. Its main interface shall contain two independently navigable file panels and shall favor keyboard operation, predictable behavior, and clear confirmation of risky actions.

The intended users are developers, system administrators, advanced Windows users, and users who prefer a terminal workflow.

## 3. Scope

### 3.1 In scope

- Local file-system navigation
- Dual-pane file browsing
- Copy, move, rename, create, and delete operations
- File viewing and editing through built-in or external tools
- Search and selection
- Keyboard shortcuts and command-line integration
- Safe error handling and recoverable operations where supported
- Configuration and persistent user preferences
- Extensible architecture for later plug-ins and remote file systems

### 3.2 Initially out of scope

- Graphical desktop user interface
- Mobile platforms
- Real-time multi-user collaboration
- Cloud storage implemented directly in the first release
- Full replacement for the Windows shell

## 4. Release Classification

Each requirement shall use one of these values in the **Disposition** field:

| Disposition | Meaning |
|---|---|
| `IN-MVP` | Planned for the first usable release |
| `POSTPONE` | Valid requirement intentionally moved to a later release |
| `LATER` | Candidate for a later release; not yet committed |
| `REJECTED` | Explicitly excluded, with a recorded reason |
| `DECIDE` | A decision is still required |

Priority and disposition are independent. A high-priority feature may still be postponed because of cost, risk, or dependencies.

| Priority | Meaning |
|---|---|
| `P0` | Essential; the product is not viable without it |
| `P1` | Important for a practical first version |
| `P2` | Useful enhancement |
| `P3` | Optional or specialized capability |

## 5. Assumptions and Constraints

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| CON-001 | The first implementation shall target Windows 10 and Windows 11 terminals. | P0 | IN-MVP | Partial |
| CON-002 | The user interface shall work in Windows Terminal and a standard console host. | P0 | IN-MVP | Partial |
| CON-003 | Core file operations shall not depend on a graphical desktop environment. | P0 | IN-MVP | Implemented |
| CON-004 | The internal design should isolate platform-specific file-system and terminal code. | P1 | IN-MVP | Partial |
| CON-005 | Long paths, Unicode filenames, drive letters, UNC paths, and Windows file attributes shall be supported. | P0 | IN-MVP | Implemented |
| CON-006 | Linux support shall be possible without redesigning the core application. | P2 | LATER | Not Started |

## 6. User Interface Requirements

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| UI-001 | The main screen shall show two file panels side by side. | P0 | IN-MVP | Implemented |
| UI-002 | Exactly one panel shall be active, with a visually distinct cursor and header. | P0 | IN-MVP | Implemented |
| UI-003 | Each panel shall show its current path and available drive or volume information. | P1 | IN-MVP | Implemented |
| UI-004 | A file row shall be able to show name, size, modification time, type, and attributes. | P0 | IN-MVP | Implemented |
| UI-005 | The user shall be able to configure visible columns and their widths. | P2 | IN-MVP | Implemented |
| UI-006 | The interface shall adapt to terminal resizing and define a documented minimum terminal size. | P0 | IN-MVP | Implemented |
| UI-007 | The application shall provide a command line at the bottom of the screen. | P1 | IN-MVP | Implemented |
| UI-008 | The bottom area shall show context-sensitive function-key actions. | P1 | IN-MVP | Implemented |
| UI-009 | Dialogs shall be usable entirely from the keyboard. | P0 | IN-MVP | Implemented |
| UI-010 | The user shall be able to choose a color theme, including a high-contrast theme. | P2 | IN-MVP | Implemented |
| UI-011 | The interface shall correctly render Unicode text supported by the active terminal and font. | P0 | IN-MVP | Implemented |
| UI-012 | Mouse selection, scrolling, and activation may be supported without being required. | P3 | IN-MVP | Implemented |
| UI-013 | A single-panel and a directory-tree layout may be provided in addition to dual-pane mode. | P3 | LATER | Not Started |
| UI-014 | Panel separators/borders and dialog boxes shall use the IBM PC/CP437 Unicode box-drawing character set (single-line `─│┌┐└┘├┤┬┴┼`, U+2500 range; see `AIPrompt/ascii_pc_frame_characters.md`) as the default frame style; the plain 7-bit ASCII fallback (`+`, `-`, `|`) remains available as ACC-006's basic-symbol mode for fonts/terminals lacking box-drawing glyphs. | P1 | IN-MVP | Implemented |

## 7. Navigation Requirements

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| NAV-001 | The user shall be able to enter directories and navigate to the parent directory. | P0 | IN-MVP | Implemented |
| NAV-002 | `Tab` shall switch the active panel. | P0 | IN-MVP | Implemented |
| NAV-003 | The user shall be able to type or paste an absolute or relative path. | P0 | IN-MVP | Implemented |
| NAV-004 | The application shall provide drive selection on Windows. | P0 | IN-MVP | Implemented |
| NAV-005 | Each panel shall retain independent path, cursor, sorting, and selection state. | P0 | IN-MVP | Implemented |
| NAV-006 | Navigation history shall support backward and forward movement. | P1 | IN-MVP | Implemented |
| NAV-007 | The user shall be able to create named favorite locations. | P2 | LATER | Not Started |
| NAV-008 | The application shall restore panel locations from the previous normal exit. | P1 | IN-MVP | Implemented |
| NAV-009 | Symbolic links, junctions, and reparse points shall be identified and handled without accidental traversal loops. | P1 | IN-MVP | Implemented |
| NAV-010 | Hidden and system files shall be optionally visible. | P1 | IN-MVP | Implemented |

## 8. File Listing and Sorting

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| LIST-001 | A panel shall list files and directories for the current location. | P0 | IN-MVP | Implemented |
| LIST-002 | Entries shall be sortable by name, extension/type, size, and modification time. | P0 | IN-MVP | Implemented |
| LIST-003 | Sorting shall support ascending and descending order. | P0 | IN-MVP | Implemented |
| LIST-004 | Directories may be grouped before files, controlled by configuration. | P1 | IN-MVP | Implemented |
| LIST-005 | The user shall be able to refresh either panel without losing a valid cursor position. | P0 | IN-MVP | Implemented |
| LIST-006 | The panel shall indicate inaccessible, broken, or unavailable locations clearly. | P0 | IN-MVP | Implemented |
| LIST-007 | Background monitoring may refresh a panel after external file-system changes. | P2 | LATER | Not Started |
| LIST-008 | Large directories shall be loaded without freezing input for an excessive period. | P1 | IN-MVP | Implemented |

## 9. Selection Requirements

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| SEL-001 | The user shall be able to select and unselect individual entries. | P0 | IN-MVP | Implemented |
| SEL-002 | The user shall be able to select all, invert the selection, and clear the selection. | P1 | IN-MVP | Implemented |
| SEL-003 | The user shall be able to select entries using wildcard masks. | P1 | IN-MVP | Implemented |
| SEL-004 | Operations shall act on selected entries, or on the cursor entry when nothing is selected. | P0 | IN-MVP | Implemented |
| SEL-005 | The interface shall display the selected item count and total selected size. | P1 | IN-MVP | Implemented |
| SEL-006 | Regular-expression and attribute-based selection may be added later. | P3 | LATER | Not Started |

## 10. File Operation Requirements

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| FOP-001 | The user shall be able to copy files and directories, normally to the opposite panel. | P0 | IN-MVP | Implemented |
| FOP-002 | The user shall be able to move files and directories. | P0 | IN-MVP | Implemented |
| FOP-003 | The user shall be able to rename one file or directory. | P0 | IN-MVP | Implemented |
| FOP-004 | The user shall be able to create a directory. | P0 | IN-MVP | Implemented |
| FOP-005 | The user shall be able to create an empty file. | P1 | IN-MVP | Implemented |
| FOP-006 | The user shall be able to delete selected entries. | P0 | IN-MVP | Implemented |
| FOP-007 | Deletion shall offer Recycle Bin use where supported and permanent deletion only as an explicit action. | P0 | IN-MVP | Implemented |
| FOP-008 | Destructive operations shall require confirmation according to configurable safety rules. | P0 | IN-MVP | Implemented |
| FOP-009 | Copy and move dialogs shall show source, destination, progress, current item, transferred bytes, speed, and errors. | P1 | IN-MVP | Implemented |
| FOP-010 | Conflict handling shall offer overwrite, skip, rename, retry, cancel, and apply-to-all choices where applicable. | P0 | IN-MVP | Implemented |
| FOP-011 | A failed multi-file operation shall report completed, skipped, and failed items. | P0 | IN-MVP | Implemented |
| FOP-012 | The user shall be able to cancel a long-running operation safely. | P0 | IN-MVP | Implemented |
| FOP-013 | Operations should preserve applicable timestamps and attributes. | P1 | IN-MVP | Implemented |
| FOP-014 | The application may support queued and background file operations. | P2 | LATER | Not Started |
| FOP-015 | The application may pause and resume supported long-running operations. | P2 | LATER | Not Started |
| FOP-016 | Secure file deletion shall not be part of the MVP. | P3 | POSTPONE | N/A |
| FOP-017 | Multi-rename with patterns, counters, and preview may be provided later. | P2 | LATER | Not Started |
| FOP-018 | File splitting and recombination may be provided later. | P3 | LATER | Not Started |

## 11. View, Edit, and Execute Requirements

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| VEE-001 | Pressing Enter on a directory shall open it. | P0 | IN-MVP | Implemented |
| VEE-002 | Pressing Enter on a file shall use a configurable default action. | P0 | IN-MVP | Implemented |
| VEE-003 | The application shall provide or invoke a text viewer for files. | P1 | IN-MVP | Implemented |
| VEE-004 | The viewer shall support text search, line navigation, encoding detection or selection, and large-file handling. | P1 | IN-MVP | Implemented |
| VEE-005 | Hexadecimal viewing may be supported. | P2 | LATER | Not Started |
| VEE-006 | Editing shall initially invoke a configured external editor. | P1 | IN-MVP | Implemented |
| VEE-007 | A built-in text editor may be developed later. | P2 | LATER | Not Started |
| VEE-008 | Executable files and scripts shall run only after an explicit user action. | P0 | IN-MVP | Implemented |
| VEE-009 | File associations shall be configurable by extension or pattern. | P2 | LATER | Not Started |

## 12. Search, Filter, and Comparison

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| SRC-001 | The user shall be able to search recursively by filename or wildcard. | P1 | IN-MVP | Implemented |
| SRC-002 | Search results shall be navigable and openable in a panel or viewer. | P1 | IN-MVP | Implemented |
| SRC-003 | The user shall be able to filter the visible panel by name or wildcard. | P1 | IN-MVP | Implemented |
| SRC-004 | Content search shall support plain text. | P2 | LATER | Not Started |
| SRC-005 | Content search may support regular expressions and encoding options. | P2 | LATER | Not Started |
| SRC-006 | The application may compare directories by name, size, timestamp, or checksum. | P2 | LATER | Not Started |
| SRC-007 | Directory synchronization shall require preview and explicit confirmation. | P3 | LATER | Not Started |
| SRC-008 | A text-file difference viewer may be integrated later. | P3 | LATER | Not Started |

## 13. Archive Requirements

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| ARC-001 | ZIP archives should be browsable as virtual directories. | P2 | LATER | Not Started |
| ARC-002 | The user should be able to create and extract ZIP archives. | P2 | LATER | Not Started |
| ARC-003 | Additional archive types shall use an extensible provider mechanism. | P3 | LATER | Not Started |
| ARC-004 | Password-protected archive support shall not expose passwords in logs or command histories. | P2 | LATER | Not Started |

## 14. Command-Line and Shell Integration

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| CLI-001 | The application shall accept zero, one, or two starting paths as command-line arguments. | P0 | IN-MVP | Implemented |
| CLI-002 | Commands entered in the internal command line shall run in the active panel directory. | P1 | IN-MVP | Implemented |
| CLI-003 | The user shall be able to insert the active filename, selected filenames, or active path into the command line. | P1 | IN-MVP | Implemented |
| CLI-004 | Command history shall be retained subject to configuration. | P1 | IN-MVP | Implemented |
| CLI-005 | The application shall provide an option to open a separate shell in the active directory. | P1 | IN-MVP | Implemented |
| CLI-006 | Shell metacharacters and quoting shall be handled predictably and documented. | P0 | IN-MVP | Implemented |
| CLI-007 | Potentially sensitive commands may be excluded from persistent history. | P1 | IN-MVP | Implemented |

## 15. Keyboard and Input Requirements

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| KEY-001 | All MVP functions shall be available without a mouse. | P0 | IN-MVP | Implemented |
| KEY-002 | Default shortcuts should follow familiar commander-style conventions where practical. | P1 | IN-MVP | Implemented |
| KEY-003 | `F3`, `F4`, `F5`, `F6`, `F7`, and `F8` should map to View, Edit, Copy, Move, New Directory, and Delete respectively. | P1 | IN-MVP | Implemented |
| KEY-004 | The application shall provide an in-application shortcut reference. | P1 | IN-MVP | Implemented |
| KEY-005 | Shortcut rebinding may be supported later through configuration. | P2 | LATER | Not Started |
| KEY-006 | Invalid or unavailable key combinations shall fail safely and provide feedback when needed. | P1 | IN-MVP | Implemented |

## 16. Configuration Requirements

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| CFG-001 | Configuration shall be stored in a documented, human-readable format. | P1 | IN-MVP | Implemented |
| CFG-002 | User configuration shall be stored in an appropriate per-user location. | P1 | IN-MVP | Implemented |
| CFG-003 | The application shall operate with sensible defaults when no configuration exists. | P0 | IN-MVP | Implemented |
| CFG-004 | Unknown configuration fields should not prevent startup unless they make safe operation impossible. | P1 | IN-MVP | Implemented |
| CFG-005 | Invalid settings shall be reported with the setting name and invalid value. | P1 | IN-MVP | Implemented |
| CFG-006 | Portable mode, with settings stored beside the executable, may be supported. | P2 | LATER | Not Started |
| CFG-007 | Settings import and export may be supported. | P3 | LATER | Not Started |

## 17. Error Handling, Logging, and Recovery

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| ERR-001 | File-system errors shall be translated into clear messages containing the affected path and operation. | P0 | IN-MVP | Implemented |
| ERR-002 | Expected errors such as access denied or a missing file shall not terminate the application. | P0 | IN-MVP | Implemented |
| ERR-003 | The user shall be offered retry, skip, skip all, or cancel where those actions are meaningful. | P0 | IN-MVP | Implemented |
| ERR-004 | Diagnostic logs shall be available and configurable by severity. | P1 | IN-MVP | Implemented |
| ERR-005 | Logs shall avoid storing passwords, credentials, and sensitive command content. | P0 | IN-MVP | Implemented |
| ERR-006 | After an abnormal termination, the application should restore only safe UI state, not automatically repeat file operations. | P0 | IN-MVP | Implemented |
| ERR-007 | Operation journaling and resumable recovery may be added later. | P2 | LATER | Not Started |

## 18. Security and Safety Requirements

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| SEC-001 | The application shall operate with the privileges of the current user by default. | P0 | IN-MVP | Implemented |
| SEC-002 | It shall not request administrator privileges for ordinary operations. | P0 | IN-MVP | Implemented |
| SEC-003 | Privilege elevation, if later supported, shall be limited to the specific operation requiring it. | P1 | IN-MVP | Implemented |
| SEC-004 | Paths and filenames shall be treated as data and safely quoted when passed to external programs. | P0 | IN-MVP | Implemented |
| SEC-005 | The application shall protect against accidental recursive copy or move into a source subtree. | P0 | IN-MVP | Implemented |
| SEC-006 | Reparse points and symbolic links shall not cause unintended recursive deletion or traversal. | P0 | IN-MVP | Implemented |
| SEC-007 | Before permanent deletion or overwrite, the dialog shall identify the target clearly. | P0 | IN-MVP | Implemented |
| SEC-008 | Plug-ins and scripts shall not be loaded automatically from untrusted directories. | P1 | LATER | N/A |

## 19. Performance Requirements

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| PER-001 | Startup should reach an interactive screen within 1 second on a typical local system, excluding unavailable network paths. | P1 | IN-MVP | Implemented |
| PER-002 | Input shall remain responsive during lengthy scans and file operations. | P0 | IN-MVP | Implemented |
| PER-003 | A directory containing at least 100,000 entries shall be handled without unbounded memory growth or application failure. | P1 | IN-MVP | Implemented |
| PER-004 | File copying shall use buffered or platform-optimized I/O suitable for large files. | P1 | IN-MVP | Implemented |
| PER-005 | Progress updates shall not significantly reduce transfer performance. | P1 | IN-MVP | Implemented |

Exact performance thresholds shall be refined after a prototype is benchmarked on agreed reference hardware.

## 20. Accessibility and Internationalization

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| ACC-001 | Status shall not be communicated by color alone. | P1 | IN-MVP | Implemented |
| ACC-002 | The default theme shall maintain readable contrast. | P1 | IN-MVP | Implemented |
| ACC-003 | User-facing text shall be separated from program logic to permit localization. | P1 | IN-MVP | Implemented |
| ACC-004 | English shall be the initial interface language. | P0 | IN-MVP | Implemented |
| ACC-005 | Hungarian localization should be supported after localization infrastructure exists. | P2 | LATER | Not Started |
| ACC-006 | The application shall avoid relying on terminal glyphs that are absent from common fonts; a basic-symbol mode should be available (see UI-014, which names the plain-ASCII `+`/`-`/`|` frame style as this basic-symbol mode). | P1 | IN-MVP | Partial |

## 21. Extensibility Requirements

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| EXT-001 | Core UI, file-operation services, platform integration, and file-system providers should have defined boundaries. | P1 | IN-MVP | Implemented |
| EXT-002 | Remote and virtual file systems should be representable through a common provider interface. | P2 | LATER | Not Started |
| EXT-003 | A public plug-in API may later support viewers, archive handlers, file-system providers, and commands. | P3 | LATER | Not Started |
| EXT-004 | SFTP support may be implemented as a remote file-system provider. | P3 | LATER | Not Started |
| EXT-005 | FTP support may be implemented as a remote file-system provider. | P3 | LATER | Not Started |
| EXT-006 | Cloud providers may be implemented through optional extensions. | P3 | LATER | Not Started |

## 22. Testing and Quality Requirements

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| TST-001 | Core path handling, selection, sorting, conflict resolution, and file-operation planning shall have automated unit tests. | P0 | IN-MVP | Implemented |
| TST-002 | Integration tests shall use temporary directories and verify file contents and metadata after operations. | P0 | IN-MVP | Implemented |
| TST-003 | Tests shall cover Unicode, long paths, empty files, large files, read-only files, hidden files, links/reparse points, and access failures. | P0 | IN-MVP | Implemented |
| TST-004 | Destructive tests shall never target arbitrary user directories. | P0 | IN-MVP | Implemented |
| TST-005 | Terminal UI behavior shall have repeatable tests for navigation and critical dialogs. | P1 | IN-MVP | Implemented |
| TST-006 | Performance tests shall cover large directories and multi-gigabyte transfers using safe generated data or sparse files. | P1 | IN-MVP | Partial |
| TST-007 | A release shall not proceed with known data-loss defects. | P0 | IN-MVP | Implemented |

## 23. Installation Requirements

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| INST-001 | The application shall be installable via a native C++ installer (`MyCommanderSetup.exe`), with no third-party installer framework (WiX/Inno/NSIS) or Windows Installer (MSI) database, consistent with DEC-002/DEC-009's no-third-party-tooling stance. | P2 | IN-MVP | Implemented |
| INST-002 | The installer shall verify the target machine can run the application (Windows version, 64-bit) before installing anything, and shall stop with a clear message rather than partially installing if a check fails. | P2 | IN-MVP | Implemented |
| INST-003 | The installer shall not require administrator privileges for an ordinary install, consistent with SEC-002's "no elevation for ordinary operation" — it installs per-user, under `%LOCALAPPDATA%\Programs\MyCommander`. | P2 | IN-MVP | Implemented |
| INST-004 | The installer shall create a desktop shortcut to the installed application. | P2 | IN-MVP | Implemented |
| INST-005 | The installer shall register the installed application with Windows ("Apps & Features") with a working uninstall path that removes the installed files, the desktop shortcut, and the registration itself. | P2 | IN-MVP | Implemented |
| INST-006 | Every build of the installer shall embed a freshly rebuilt Release build of the application, never a stale or Debug build, regardless of which configuration the installer itself is built in. | P3 | IN-MVP | Implemented |

## 24. Proposed MVP

The first usable release should include:

1. Windows terminal support and Unicode/long-path handling.
2. Two independent file panels with keyboard navigation.
3. Drive and path selection, history, sorting, and refresh.
4. File selection and selected-size summary.
5. Safe copy, move, rename, directory creation, and deletion.
6. Conflict resolution, cancellation, progress, and error reporting.
7. Text viewing and external-editor integration.
8. Filename search and panel filtering.
9. Internal command line and external shell launch.
10. Persistent configuration and previous panel locations.
11. Diagnostic logging and automated tests for critical file operations.

## 25. Recommended Postponement Candidates

These features are useful but can be postponed with limited impact on the core product:

| Candidate | Related IDs | Reason to postpone |
|---|---|---|
| Favorites | NAV-007 | History and direct path entry cover the initial need. |
| Background monitoring | LIST-007 | Manual refresh is sufficient initially. |
| Queued and resumable operations | FOP-014, FOP-015, ERR-007 | Adds concurrency and recovery complexity. |
| Multi-rename and file splitting | FOP-017, FOP-018 | Specialized workflows. |
| Hex viewer and built-in editor | VEE-005, VEE-007 | External tools provide an early solution. |
| Content search and comparison | SRC-004 to SRC-008 | Valuable but not required for basic file management. |
| Archive browsing | ARC-001 to ARC-004 | Requires virtual file-system and archive abstractions. |
| Shortcut customization | KEY-005 | A stable default key map is enough for MVP. |
| Portable mode | CFG-006 | Per-user configuration is sufficient initially. |
| Localization | ACC-005 | English can be used while strings remain localization-ready. |
| Plug-ins and remote systems | EXT-002 to EXT-006 | Should follow stabilization of the local provider interface. |

## 26. Postponement Register

When postponing a feature, keep its original requirement and add a row below. This preserves intent and provides an auditable reason for the change.

| Requirement ID | Feature | Previous disposition | New disposition | Target release | Reason | Dependencies/impact | Decision date |
|---|---|---|---|---|---|---|---|
| FOP-016 | Secure deletion | LATER | POSTPONE | Unscheduled | Platform-specific behavior and limited MVP value | None for core file operations | Initial draft |
| _Example: ARC-001_ | _Browse ZIP archives_ | _LATER_ | _POSTPONE_ | _v2_ | _Reduce first-release scope_ | _Archive provider postponed_ | _YYYY-MM-DD_ |

## 27. Open Decisions

| ID | Decision | Options | Recommended starting point | Status |
|---|---|---|---|---|
| DEC-001 | Implementation language | C++, Rust, C#, Python, other | Decide after a terminal UI prototype | DECIDED: native C++ (2026-09-20) |
| DEC-002 | Terminal UI framework | Framework-specific choices depend on language | Prototype resize, Unicode, and key handling first | DECIDED: raw Win32 Console API, double-buffered via WriteConsoleOutputW — no third-party TUI library (2026-09-20) |
| DEC-003 | Built-in viewer | Minimal built-in viewer or external tool only | Minimal built-in text viewer | DECIDED: minimal built-in text viewer (F3), no external tool dependency — scroll, tab expansion, UTF-8/UTF-16 BOM detection with ANSI fallback, forward substring search (2026-09-20) |
| DEC-004 | Command interpreter | `cmd.exe`, PowerShell, configurable | Configurable; PowerShell default on modern Windows | DECIDE |
| DEC-005 | Copy engine | Native platform API or custom buffered implementation | Abstract it and benchmark both | DECIDE |
| DEC-006 | Recycle Bin integration | Windows Shell API or permanent-delete-only MVP | Use Recycle Bin through platform adapter | DECIDE |
| DEC-007 | Configuration format | TOML, JSON, YAML, other | TOML or JSON | DECIDED: a minimal hand-rolled "key = value" plain-text format (`#`/`;` comments), consistent with DEC-002's no-third-party-library stance — no TOML/JSON/YAML parser dependency needed for the small, flat settings this app has (2026-09-21) |
| DEC-008 | Licensing | Proprietary or open source | Decide before accepting outside contributions | DECIDED: MIT (open source) (2026-09-20) |
| DEC-009 | Build system / toolchain | Native VS solution (.sln/.vcxproj), CMake | Prototype with the installed VS toolset first | DECIDED: native VS 2022 Professional solution (MSBuild, v143 toolset, C++20, Windows SDK 10.0.26100.0) (2026-09-20) |

## 28. Initial Acceptance Criteria

The MVP is acceptable when all of the following are true:

1. The application starts in a supported Windows terminal and renders two usable panels.
2. A user can navigate both panels independently using only the keyboard.
3. A user can copy, move, rename, create, and delete files and directories safely.
4. File conflicts and access errors do not crash the application and provide actionable choices.
5. Long-running operations show progress and can be cancelled without corrupting completed targets.
6. Unicode names, long paths, drive roots, UNC paths, and inaccessible entries are handled predictably.
7. A user can view a text file, invoke an external editor, search by filename, and run a command in the active directory.
8. Configuration and panel locations persist across normal restarts.
9. Automated tests cover all critical file-operation and path-handling behavior.
10. No known defect can cause silent data loss.

## 29. Change Procedure

For each planning revision:

1. Keep existing requirement IDs stable.
2. Change the requirement's **Disposition** to `POSTPONE` rather than deleting it.
3. Add a row to the Postponement Register.
4. Record the reason, expected impact, and intended release if known.
5. Review dependencies so an included feature does not depend on a postponed one.
6. Update the Proposed MVP and acceptance criteria if the scope changes.

This method allows later scope reduction while preserving a complete product roadmap.
