# Implementation Plan — FOP-017 (Multi-rename with patterns, counters, and preview)

Source: `AIPrompt/my-commander.md`, `FOP-017`:

> Multi-rename with patterns, counters, and preview may be provided later.

- Priority P2, currently `LATER` / `Not Started`.
- Listed in §29 ("Multi-rename and file splitting — specialized workflows").

## Implementation check (2026-10-07)

| Item | State |
|---|---|
| Group rename logic (patterns, counters, preview) | None in the code |
| **File → Group Rename** | A placeholder: "Not yet implemented" (`MenuBar.h:103`, MENU-003) |
| Single rename `F2` | `DoRename` → `FileOps::RenameItem` (FOP-003) |
| **Bug found:** case-only rename | `RenameItem` refuses `report.txt` → `Report.TXT`. `fs::exists(dst)` is true on a case-insensitive file system, so it reports "already exists". This affects plain `F2` today and would break a group rename's case options. |
| Name validation | `ValidateEntryName` rejects empty, `.`/`..` and `\ / : * ? " < > \|`. It doesn't catch Windows' reserved names (`CON`, `NUL`, `COM1`…) or a trailing dot/space, which Windows silently strips. |

## Summary

A Total Commander-style **group rename** for the selection (or cursor item) in the active panel:

1. **`Ctrl+M`** (TC's multi-rename key) or **File → Group Rename** opens an options form:
   - name mask and extension mask, with tokens like `[N]`, `[C]`, `[YMD]`;
   - search and replace;
   - case conversion;
   - counter start / step / digits.
2. **A preview list** shows every `old name → new name`, and marks any problem before anything is renamed:
   - an invalid name;
   - two items ending up with the same name;
   - a clash with a file that isn't being renamed.
3. **`Enter`** renames everything, with the usual animated progress, per-item Retry/Skip, and Esc. **`F2`** goes
   back to the options; **`Esc`** cancels without touching anything.

Renames that swap or chain names (`a → b` while `b → c`) are ordered, or routed through temporary names, so
they never collide midway. A failure partway puts the affected item back under its original name.

## Design

### D1 — Mask language (a pure, tested subset of Total Commander's)

The **name mask** builds the part before the last dot; the **extension mask** builds the part after it. If the
extension mask comes out empty, the new name has no dot.

| Token | Meaning | `Holiday Photo.JPG`, counter 7, 3 digits |
|---|---|---|
| `[N]` | original name without extension | `Holiday Photo` |
| `[N2-5]` | characters 2..5 of it (1-based, inclusive; a range past the end is clipped) | `olid` |
| `[N3]` | just character 3 | `l` |
| `[N3-]` | from character 3 to the end | `liday Photo` |
| `[E]`, `[E1-2]` … | the same, on the original extension (no dot) | `JPG`, `JP` |
| `[C]` | counter, zero-padded to the form's *digits* | `007` |
| `[YMD]` | last-write date, `YYYYMMDD` (local time) | `20261007` |
| `[hms]` | last-write time, `HHMMSS` | `153012` |
| `[P]` | name of the folder it's in | `Pictures` |
| `[[` / `]]` | a literal `[` / `]` | |

- Anything else in brackets (`[X]`, `[N5-2]`, an unclosed `[`) is a **mask error**. It's shown at the top of the
  preview, and Enter is disabled.
- Text outside brackets is copied as is.
- The default masks `[N]` and `[E]` reproduce the original name, so the form can be opened and inspected safely.

### D2 — Then search & replace, then case

The steps run in this order, on the generated full name:

1. **Search for / Replace with:** plain text, every occurrence, case-insensitive (`ToLower`-compared, like the
   app's other matching). An empty "Search for" means no replacement. Regex is out of scope (open question 3).
2. **Case:**
   - *unchanged*;
   - *lower case*;
   - *UPPER CASE*;
   - *First letter capital* (lower-case everything, then capitalize the first character);
   - *Each Word Capital* (after start, space, `_`, `-`, `.`).

### D3 — The options form (`ShowFormDialog`, FEA-0007)

| Field | Kind | Default |
|---|---|---|
| Name mask | Text | `[N]` |
| Extension mask | Text | `[E]` |
| Search for | Text | (empty) |
| Replace with | Text | (empty) |
| Case | Choice: unchanged / lower / UPPER / First letter / Each Word | unchanged |
| Counter start | Number | 1 |
| Counter step | Number | 1 |
| Counter digits | Number (1–9) | 1 |

- These are the existing text-field and number-field types. Text fields have IS-0016's visible, scrolling cursor.
- The last-used options are remembered **for the session only**, not persisted. This keeps `mycommander.ini` free
  of operation state (ERR-006 spirit).

### D4 — Preview and validation (`ShowListChoiceWithKeys`, NAV-007)

- **Title:** `Group rename: 12 items, 11 to rename, 1 unchanged`. With problems:
  `Group rename: 2 problems — fix the options (F2)`.
- **Rows:** `old name  →  new name`, aligned with `TextWidth.h`. Problem rows are prefixed with `!` and the
  reason. Unchanged rows say `(unchanged)`.
- **Keys:**
  - **Enter** renames. If there's any problem, it shows a message listing the first few instead;
  - **F2** goes back to the form with the values kept;
  - **Esc** cancels.
- **Statuses**, from the pure planner:

| Status | When |
|---|---|
| `Ok` | new name differs and is valid |
| `Unchanged` | new name == old name, exactly (skipped silently) |
| `CaseOnly` | differs only in letter case (allowed; see D6) |
| `Invalid` | empty, `.`/`..`, a forbidden character, a reserved device name (`CON`, `PRN`, `AUX`, `NUL`, `COM1`–`COM9`, `LPT1`–`LPT9`, with or without an extension), a trailing `.` or space, or over 255 UTF-16 units |
| `DuplicateTarget` | two items in the batch get the same name (case-insensitive) |
| `ClashesWithExisting` | the new name equals, case-insensitively, a file or folder in the directory that is **not** itself being renamed away |

A name that *another item in the batch* currently has is **not** a clash if that item is being renamed to
something else. That's the swap or chain case, handled by D5.

### D5 — Execution order (pure, tested)

`OrderRenames(plan)` produces a list of `(from, to)` steps:

- **Direct steps** come in an order where no step's target is still occupied by a not-yet-moved source. This is a
  dependency order: rename `b → c` before `a → b`.
- **Cycles** (`a ↔ b`, `a → b → c → a`) are broken by first renaming one member to a unique temporary name in the
  same folder, `~mcren.<n>.tmp`, picked so it doesn't exist, then finishing the chain.
- **Running it:** a new `FileOps::RenameItems(directory, steps, onItemStart, onError)` goes item by item through
  the existing `RunWithRetry`. That gives per-item **Retry / Skip / Skip all / Cancel** (FOP-010) and the
  `ProgressUi` animated dialog with Esc (PER-002), the same as copy and delete.
- **Rollback:** if an item that went to a temporary name can't reach its final name (Skip or Cancel), it's renamed
  **back to its original name**. Nobody is ever left with a `~mcren` file. If even that fails, the outcome names
  both paths (ERR-001).

### D6 — Case-only renames (fixes the `F2` bug too)

- In `RenameItem`: when `oldName` and `newName` differ only in case, *or* `PathsEquivalent(src, dst)` (the same
  file object), skip the "already exists" check. `MoveFileExW` performs the case change.
- `ClashesWithExisting` in D4 ignores the item's own current name.
- New `FileOpsTests`: `F2`-style `a.txt` → `A.TXT` succeeds, and the directory listing shows `A.TXT`.

### D7 — Wiring and safety

- **Key `Ctrl+M`:** free today (no `case 'M'` with Ctrl in `main.cpp`, not in `BuiltinShortcutInventory`); it gets
  added to the inventory.
- **Menu:** File → **Group Rename** becomes real. `MenuItemKind::SimulatedKey`, `Ctrl+M`, MENU-002. The MENU-003
  "Not yet implemented" stub list loses it; `MenuBarTests` is updated.
- **Refused:**
  - on panel content (ZIP/tree), with a message, like queued operations;
  - when the cursor is on `..` with nothing selected;
  - on paths a background job is using (`RefuseIfBusyWithJob`, FOP-014), checked for both the old and the new
    names.
- **Afterwards:** the panel refreshes, the other panel is synced (`SyncOtherPanelIfSamePath`), and the cursor goes
  to the first renamed item (IS-0001 convention). `ReportOutcome` summarizes failures, as copy and delete do.
- **Reparse points:** renaming a junction or link renames the link itself. That's safe, and the same as `F2`.

## New / changed code

| File | Change |
|---|---|
| `Source/MyCommander/MultiRename.{h,cpp}` (new, pure) | Mask parser and expander (D1), search/replace and case (D2), `BuildRenamePlan` (D4 statuses), `ValidateNewFileName` (reserved names, trailing dot/space, length), `OrderRenames` with temp names for cycles (D5) |
| `Source/MyCommander/FileOps.{h,cpp}` | `RenameItems` (item-by-item, `RunWithRetry`, rollback of temp names); the `RenameItem` case-only fix (D6) |
| `Source/MyCommander/main.cpp` | `DoGroupRename`: form → preview loop → run; `Ctrl+M`; session-remembered options |
| `Source/MyCommander/MenuBar.h` | Group Rename becomes a real item |
| `Source/MyCommander/PluginManagerLogic.cpp` | `Ctrl+M` in `BuiltinShortcutInventory` |
| `Source/MyCommander/Strings.h` | Form labels, preview titles, status texts |
| `Source/MyCommander/Help.cpp`, `README.md`, `USERGUIDE.md` | Key, tokens table, examples |
| `Source/MyCommanderTests/MultiRenameTests.cpp` (new), `FileOpsTests.cpp`, `MenuBarTests.cpp`, `PluginManagerTests.cpp` | Tests below |
| Both `.vcxproj` (+ filters) | New files |
| `AIPrompt/my-commander.md`, `CLAUDE.md`, `Version.h`, changelog | Spec row LATER → IN-MVP / Implemented, §29 note, history; module bullet; 1.17 → 1.18 |

## Tests

**`MultiRenameTests.cpp`** (pure):

- **Mask expansion** (`Holiday Photo.JPG`):
  - `[N]`, `[N2-5]`, `[N3]`, `[N3-]`, `[E]`, `[E1-2]`;
  - an out-of-range clip;
  - `[C]` with start/step/digits (1, 2, 3, and a counter that outgrows its digits: `9` → `10`, not truncated);
  - `[YMD]`/`[hms]` from a fixed FILETIME;
  - `[P]`;
  - `[[`/`]]`;
  - literal text;
  - an empty extension mask gives no dot;
  - a name with no extension or several dots (`archive.tar.gz`: `[N]` = `archive.tar`, `[E]` = `gz`).
- **Mask errors:** an unknown token, a reversed range, an unclosed `[`. Each gives an error, with the position.
- **Search/replace:** case-insensitive, all occurrences; replace with empty; an empty search does nothing.
- **Case modes:** all five, including Unicode (`árvíztűrő` → `Árvíztűrő` / `ÁRVÍZTŰRŐ`).
- **`ValidateNewFileName`:**
  - `CON`, `con.txt`, `COM1.log`, `NUL` are Invalid;
  - `CONSOLE.txt` is fine;
  - `name.` and `name ` (trailing) are Invalid;
  - a forbidden character is Invalid;
  - 256 characters is Invalid.
- **`BuildRenamePlan` statuses:** Ok, Unchanged, CaseOnly, DuplicateTarget (two items both → `x.txt`),
  ClashesWithExisting (a non-selected `x.txt` exists), and a swap that is **not** a clash.
- **`OrderRenames`:**
  - the chain `a → b`, `b → c` orders `b → c` first;
  - the swap `a ↔ b` uses one temp name and three steps;
  - a 3-cycle;
  - independent renames keep the selection order;
  - a temp name avoids existing `~mcren.N.tmp` names.

**`FileOpsTests.cpp`** (real `TempDir`):

- `RenameItem` case-only `a.txt` → `A.TXT` succeeds; the listing shows `A.TXT` (D6 regression);
- `RenameItems` swap of two files: contents end up swapped;
- a locked file in a batch: Skip renames the others, the locked one keeps its name, no `~mcren` file is left;
- Cancel midway: items already done stay renamed, an item at a temp name is rolled back, nothing else is touched;
- `onItemStart` is called once per user-visible item.

**`MenuBarTests` / `PluginManagerTests`:** Group Rename is a `SimulatedKey` `Ctrl+M`; `Ctrl+M` is built-in.

**Manual:**

- select photos, `Ctrl+M`, mask `[YMD]_[C]`, digits 3;
- the preview shows `20261007_001.jpg`…;
- Enter renames them, with progress;
- swap two names via masks;
- a case-only change (`[N]` + UPPER);
- an invalid mask is shown in the preview and Enter is refused;
- `Esc` from the preview changes nothing.

## Phasing

1. **Phase 1:** `MultiRename` pure module and its tests; `ValidateNewFileName`.
2. **Phase 2:**
   - `RenameItem` case-only fix (with test);
   - `RenameItems` with ordering, temp names and rollback (with tests).
3. **Phase 3:**
   - UI: form, preview loop, `Ctrl+M`, menu, inventory, busy guard, after-rename cursor;
   - docs, spec, version, changelog.

## Risks and notes

- **Main risk: leaving names half-swapped.** It's handled by the pure `OrderRenames` (tested) plus rollback, and
  the preview shows exactly what will happen before anything does.
- **Case-only fix in `RenameItem`:** it changes `F2` behavior only for a case-only rename, which was failing
  before. The "never silently overwrite" guarantee is kept, because the target *is* the source.
- **Not undoable once run.** The preview is the safeguard. "Undo last group rename" is a follow-up (open
  question 4).
- **Out of scope:**
  - regular expressions;
  - renaming across folders;
  - ID3/EXIF tokens;
  - saving masks as named presets;
  - renaming inside ZIP archives (plugin panels are refused).

## Open questions

1. **Key:** `Ctrl+M` (Total Commander's), OK?
2. **Token set:** the D1 subset (`[N]`, ranges, `[E]`, `[C]`, `[YMD]`, `[hms]`, `[P]`), enough to start?
3. **Search & replace:** plain text, case-insensitive (planned), or regular expressions as well?
4. **Undo:** skip for now (planned, the preview is the safeguard), or keep the last batch in memory and offer
   "Undo last group rename"?

## Status

Implemented 2026-10-07, all three phases (version 1.18.0), with the defaults for the open questions:
`Ctrl+M`, the D1 token set, plain-text case-insensitive search/replace, no undo. Notes from implementation:

- **Rename-step type:** `RenameStep` lives in `FileOps.h`, so the engine doesn't depend on the mask module.
  `MultiRename.h` includes `FileOps.h` and produces those steps.
- **Mask errors** are checked per item, but stop at the first one. The whole plan then reports that one mask
  error, rather than repeating it on every row.
- **Number fields are digits-only** (`ApplyFormFieldKey`), so a negative counter step can't be typed. The form
  allows a step of 1 or more and a start of 0 or more. The engine itself (`FormatRenameCounter`) supports
  negative values and is tested with them.
- **Clash checks** use the real folder listing (`directory_iterator`), not the panel's entries, so hidden or
  filtered names are caught too.
