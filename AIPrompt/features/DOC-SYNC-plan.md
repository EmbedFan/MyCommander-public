# Implementation Plan — DOC-SYNC (bring the project documents up to date)

Source: CLAUDE.md "Requirements and planning docs", plus the review of 2026-10-12. No requirement ID: this is project
hygiene. It changes documents only, never code.

## Implementation check (2.0.0.368)

| Document | State | Problem |
|---|---|---|
| `AIPrompt/system-plan_2026-09-20-173701.md`, `..._191601.md` | Generated 2026-09-20 from spec version 0.5 | Part A reproduces spec 0.5; the spec is now at the 1.27.0/2.0.0 history. Part B's readiness matrix predates menus, plugins, ZIP/FTP/SFTP, compare/sync, keys, single-panel and the diff viewer. CLAUDE.md says: regenerate, never patch |
| `AIPrompt/ManualTests.md` (MT-001…MT-024, the blank template) and `ManualTests_001.md` (the same plan as a partly filled run record: MT-001…003 marked Pass) | 2026-09-23, the 0.1.x MVP | No tests at all for anything added in 1.0–2.0 |
| Spec §31 Open Decisions | DEC-004/005/006 still `DECIDE` | The code made these choices long ago: `cmd.exe /c` (COMSPEC) for the command line and F9; `CopyFileExW`/`MoveFileWithProgressW` with progress and cancel callbacks; `SHFileOperationW` with `FOF_ALLOWUNDO` (Recycle Bin), with permanent delete as a separate path |

## Design

### D1 — Record DEC-004/005/006 (first: the system plan reproduces them)

Rewrite the three rows' **Status** column in place. A decision row records the decision itself, as DEC-010…015
already do. Add one history paragraph saying "decisions recorded from existing code, no behaviour change".

| ID | Recorded decision (as implemented) |
|---|---|
| DEC-004 Command interpreter | `%COMSPEC%` (fallback `cmd.exe`) with `/c`. The command-line text is passed as one quoted argument, CLI-002/005; F9 opens `%COMSPEC%` in the active folder. PowerShell is not the default; configurability stays a possible later change (would need a new CFG key). |
| DEC-005 Copy engine | Native platform API: `CopyFileExW` per file, and `MoveFileWithProgressW` for same-volume moves (the fast path). Progress and cancel go through `LPPROGRESS_ROUTINE` and a cancel flag (PER-002, FOP-012). Directory trees are walked by `FileOps`. Attributes and timestamps are preserved (FOP-013). |
| DEC-006 Recycle Bin integration | Windows Shell API: `SHFileOperationW` + `FOF_ALLOWUNDO` per item, with per-item progress and retry (IS-0018). Permanent delete (`Shift+F8`) is a separate `FileOps` path. |

### D2 — Regenerate the system plan

- **Output:** a new `AIPrompt/system-plan_<YYYY-MM-DD-HHMMSS>.md`. The timestamp comes from
  `Tools/get_current_timestamp.py`. The old snapshots are kept (they are history), and their headers are left as
  they are.
- **Part A:** the spec, verbatim, at its current version (after D1).
- **Part B:** a readiness matrix of **every requirement ID**, built by evaluating the code, not by copying Part A.
  For each ID it gives the state (Implemented / Partial / Not Started / N/A / POSTPONE), the evidence (modules, tests,
  plugin) and a note. Rows where Part B disagrees with the spec's Implemented column are listed at the top, as
  findings.
  - **Reviewed in particular:** the IN-MVP Partials CON-001/002/004, ACC-006 and TST-006. For each, state exactly what
    is missing.
- **Header:** the same table as the previous snapshot (Description / Created at / File version / Created by).
  There is no staleness note yet.
- **Method:** a script lists every `| XXX-nnn |` row from the spec. Each ID is evaluated by grep and by the
  test-name tags (`_SRC006` …). Unclear IDs are read in the code by hand. The script is temporary and isn't checked in.

### D3 — Manual test plan v2

- **Files:** `AIPrompt/ManualTests.md` (the blank template) gets the new tests, and its header gains "Covers
  2.0.0.x". `ManualTests_001.md` is a run record of the old plan (MT-001…003 Pass, the rest unfilled). It stays as
  it is, as history; new runs are recorded in copies named `ManualTests_<nnn>.md` (open question 2).
- **Format:** keep the existing `MT-nnn` format (goal, setup, steps, expected result, record Pass/Fail/Blocked/Not
  run). Add these tests:

| ID | Area | Feature references |
|---|---|---|
| MT-025 | Menu bar: keyboard and mouse, mnemonics, key column, stubs say "Not yet implemented" | FEA-0005, MENU-* |
| MT-026 | File properties | FEA-0003 |
| MT-027 | Plugins: discovery, disabledPluginIds, help pages, crash isolation (ExamplePlugin) | FEA-0002 |
| MT-028 | ZIP: pack/unpack, browse, copy in/out, delete, mkdir, save on leave, packages | FEA-0007, ZIP-* |
| MT-029 | Line editor cursor everywhere; Shift+Up/Down selection; panel sync | IS-0015/16 |
| MT-030 | Copy as (Shift+F5) | FOP-019 |
| MT-031 | File actions and Assign File to App | FEA-0009 |
| MT-032 | Hex view | VEE-005 |
| MT-033 | Directory tree and preview | UI-013 B |
| MT-034 | Background jobs: queue, overlap waits, refusal, quit with jobs | FOP-014 |
| MT-035 | Hotlist | NAV-007 |
| MT-036 | Group rename incl. chains and cycles | FOP-017 |
| MT-037 | Content / regex search | SRC-004/005 |
| MT-038 | Compare folders | SRC-006 |
| MT-039 | Synchronize folders (preview, overrides, confirmation) | SRC-007 |
| MT-040 | Key bindings (keys.conf, live reload, relabelled hints/menus/F1) | KEY-005 |
| MT-041 | FTP (against a local test server, e.g. FileZilla Server or `python -m pyftpdlib`) | EXT-005 |
| MT-042 | SFTP: password/key/agent, host-key prompt, changed key refused, transfers | EXT-004 |
| MT-043 | Viewer plugins | EXT-003 |
| MT-044 | Single-panel layout | UI-013 A |
| MT-045 | Difference viewer and diffProgram | SRC-008 |
| MT-046 | Installer bundles the plugins | INST-007 |
| MT-047 | Regression sweep of the 2.0.0 cppcheck fixes (panel layout, file actions, keys, jobs, ZIP, remote panels) | `sec_fixes_report_2026-10-12_0940.md` |

- **Fixture section:** extended with a ZIP and a JAR, an FTP and SFTP test server, a `keys.conf` sample and two
  text-file versions for the diff.

## Files touched

| File | Change |
|---|---|
| `AIPrompt/my-commander.md` | DEC-004/005/006 rows and a history paragraph |
| `AIPrompt/system-plan_<timestamp>.md` (new) | The regenerated plan |
| `AIPrompt/ManualTests.md` | MT-025…MT-047, fixtures, header |
| `AIPrompt/ManualTests_001.md` | Unchanged (the 2026-09-23 run record) |
| `3_Reports/ChangeLogs/changelog_<date>.md` | One entry (CLAUDE.md requires one for spec and system-plan changes) |

## Phasing

1. D1, decisions (small; the system plan reproduces them).
2. D2, system plan.
3. D3, manual tests.

## Open questions

1. Should DEC-004 keep "cmd.exe only" as the decision, or should configurability (PowerShell) be recorded as a
   later option with a new CFG requirement?
2. Should run records continue as copies (`ManualTests_<nnn>.md`) of the template (recommended)? Or should results be recorded in a separate table?
3. Should the old system-plan snapshots be kept (recommended) or removed?

## Status

Planned 2026-10-12. **Implemented 2026-10-13** in commit `8eb7959` ("DOC-SYNC: record open decisions, regenerate system
plan, extend manual test plan"), all three parts:

- **D1:** `my-commander.md`'s Open Decisions table marks DEC-004, DEC-005 and DEC-006 DECIDED; the file's version header
  was brought up to date (1.28.0 at the time) with a history paragraph.
- **D2:** the system plan was regenerated as `AIPrompt/system-plan_2026-10-13-175053.md` (Part A reproduces the spec,
  Part B is an independent readiness matrix), superseding the two 2026-09-20 snapshots.
- **D3:** `ManualTests.md` gained the new fixture item and MT-025 to MT-047.

Not done and not part of this plan: the system plan has not been regenerated since. Its Part B is stale for every
requirement implemented after spec 1.28.0 (the spec is at 1.49.0), as CLAUDE.md describes; regenerating it is a
separate task.
