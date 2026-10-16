# Implementation Plan — LIST-009 (colour labels on files and folders)

Source: a new requirement, proposed here. Not in the spec, `additional_feature_requests.md` or any earlier plan.

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| LIST-009 (proposed) | The user shall be able to mark files and folders with one of a small set of colour labels, shown in the panel listing and persisted outside the files themselves, and to select entries by label. | P3 | LATER | Not Started |

## Summary

A lightweight way to mark "to review", "done", "send" without renaming, moving or touching the files: eight named colour
labels, applied from the keyboard to the selection, drawn in the listing, remembered between runs.

- **Apply:** `Ctrl+L` cycles the label of the selection (or the cursor entry): none → 1 → 2 → … → 8 → none.
- **Choose / clear / select:** `Ctrl+Shift+L` opens a chooser: *Set label 1…8*, *Clear label*, *Select all entries with
  this label in this folder*, *Remove labels of missing files*.
- **Display:** the entry's name is drawn in the label's colour, plus a one-cell marker so the label is never colour-only
  (ACC-002/ACC-006 spirit; the marker also survives the high-contrast and ASCII-frame modes).
- **Storage:** `%APPDATA%\MyCommander\labels.conf`, a hand-editable text file keyed by full path. **Nothing is written
  into, beside or onto the labelled files** (no alternate data stream, no sidecar file in their folder).

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| A small hand-editable store, saved immediately on change, parse errors warned and skipped | `Favorites.{h,cpp}`, `Utf8TextFile.h` | The file format and persistence |
| Per-entry colour choices by type/attributes | `main.cpp` `DrawPanel`, `Theme.h` palette | Where the label colour is applied |
| A theme palette whose every field is contrast-tested for each named theme | `Theme.h`, `ColorContrast.h`, `ColorContrastTests.cpp` (ACC-002/UI-010) | The eight label colours must pass the same test |
| Selection by predicate | `Panel::SelectByMask`, `SetEntrySelected` (SEL-003/IS-0004) | "Select all with this label" |
| The key table plus the inventory test | `KeyMap.cpp`, `PluginManagerLogic.cpp` | The two new commands |
| Rename / move / delete engines with per-item outcomes | `FileOps.cpp`, `main.cpp` `ReportOutcome` | Keeping labels attached when the app itself renames/moves, and dropping them on delete |

## Design

### D1 — `LabelStore` (pure, `Source/MyCommander/Labels.{h,cpp}`)

- `int LabelFor(path)`, `Set(path, label)` (0 clears), `Move(oldPath, newPath)` (also moves labels of everything beneath a
  renamed folder), `RemoveUnder(path)` (on delete), `Entries()`.
- Comparison: lexical, case-insensitive, separators and trailing separator normalized; **never touches the
  filesystem** (an offline share can't stall drawing or loading).
- Capped at 5,000 labels; setting one beyond the cap is refused with a message.
- `ParseLabelsFile` / `FormatLabelsFile`: UTF-8 lines `label = path`, label `1`…`8`, first `=` splits, a bad line is
  warned and skipped, unknown/duplicate paths collapse (last wins).

### D2 — Display

- `DrawPanel` asks the store for the entry's label (`O(1)` map lookup keyed by the normalized full path; the folder's
  prefix is computed once per draw, not per row).
- Eight new palette fields (`kAttrLabel1..8`) in `Theme.h`, each defined for the **default** and **high-contrast**
  themes; the existing contrast test enumerates every field of every theme, so a failing colour is caught at build time.
- The cursor row and a selected row keep their own attributes (cursor and selection stay visible); the label colour
  shows on non-cursor rows and, for the cursor row, through the marker cell only.
- **Marker:** one cell, `■` (`#` in ASCII-frame mode), in the label's colour. Placement is the first thing to settle in
  phase 1 by reading `DrawPanel`: it should reuse an existing separator cell between the name and the next column rather
  than shifting any column; on a very narrow terminal the marker is dropped before any column is, never the name.

### D3 — Commands

- `CommandId::CycleLabel` (`K('L', C)`) and `CommandId::LabelMenu` (`K('L', C, S)`); inventory entries and test.
- **Cycle:** applies the *next label of the cursor entry* to every selected entry (so a mixed selection becomes uniform).
  Labelling `..` is refused.
- **Chooser** (`ShowListChoice`): *Label 1 … 8* (with colour names), *Clear label*, *Select entries with this label*,
  *Remove labels of missing files*. "Remove labels of missing files" checks existence only for the labels **under the
  current folder** (one `GetFileAttributes` each, cancelable), never for the whole store, so an offline share elsewhere
  can't block.
- Works in a plain local panel; refused with a message in a plugin/provider panel (labels are about local paths).

### D4 — Staying attached

- **Copy:** labels are not copied (the copy is a new file).
- **Move / rename** done by the application (F6, F2, group rename, drag): `LabelStore::Move` is called with each
  successful old→new pair, from the shared outcome reporting, so a labelled file keeps its label.
- **Delete** (Recycle Bin or permanent): `RemoveUnder` for each successfully deleted path.
- **Changes outside the application** (Explorer renames, other tools): the label is orphaned and harmless; the chooser's
  cleanup removes them on demand. No background process re-reads the store.

### D5 — Config

- `labelColors = ...` is **not** a setting in the first version (the eight colours are theme fields). One preference:
  `showLabels = true | false` (default `true`) hides the colour and marker without deleting any label.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/Labels.{h,cpp}` (new) | Store, normalization, file format |
| `Source/MyCommander/Theme.h`, `ColorContrast.h` tests | Eight label attributes in both themes |
| `Source/MyCommander/Config.{h,cpp}` | `showLabels` |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp`, `MenuBar.h`, `HintBar.h`, `Help.cpp` | Commands, menu items (Selection menu), help |
| `Source/MyCommander/main.cpp` | Draw, the commands, move/delete hooks, load/save |
| `Source/MyCommanderTests/LabelsTests.cpp` (new), `ColorContrastTests.cpp`, `ConfigTests.cpp`, `KeyMapTests.cpp` | Tests below |
| both `.vcxproj`, `README.md`, `USERGUIDE.md`, `CLAUDE.md`, spec (new LIST-009 row), `ManualTests.md`, changelog, `Version.h` | Docs |

## Tests

- **`LabelStore`:**
  - set, read back, clear; case- and separator-insensitive lookup;
  - `Move` of a file and of a folder carries labels beneath it; `RemoveUnder` drops a subtree;
  - the 5,000 cap refuses the next label;
  - parse/format round trip, Unicode paths, CRLF, bad lines warned and skipped, duplicate paths (last wins), out-of-range
    label values rejected.
- **Theme:** every label attribute of every named theme meets the existing WCAG AA contrast test against the panel
  background.
- **Selection by label:** `Panel` selection helper chooses exactly the entries whose label matches (a pure function
  taking the entries and a lookup).
- **Config:** `showLabels` default and parsing.
- **Manual:** label a mix of files and folders, restart, confirm persistence; rename and move them in the app and
  confirm the label follows; delete one and confirm it is gone; label in high-contrast mode and ASCII-frame mode;
  confirm the cursor row stays readable; label 200 files in one folder and scroll (no slowdown).

## Phasing

1. `LabelStore` and its tests. 2. Theme fields, drawing and the marker. 3. Commands, chooser, move/delete hooks, config.
4. Docs.

## Risks

- **State outside the files:** labels do not follow files moved by other tools. Documented limit; the chooser's cleanup and
  the orphan-is-harmless rule bound the damage.
- **Colour-only meaning:** the marker cell is mandatory, and the chooser lists labels by *name and number*, not by colour
  alone.
- **Drawing cost:** a lookup per visible row only (the existing `DrawPanel` already renders only the visible range, PER-003).
- **Theme contrast:** eight distinguishable colours that all pass AA on both themes is tight on a 16-colour console.
  If some cannot, the fallback is fewer labels (6) rather than a failing test; settle in phase 2.

## Open questions

1. **Eight labels or six?** Recommended: eight if all pass the contrast test in both themes, else six.
2. **Should labels follow files on copy-as / archive extraction?** Recommended: no.
3. **A "show only labelled" panel filter?** Recommended: later, as a quick-filter extension.

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.37.0) with the recommended answers, using the fallback in
open question 1: **six labels, not eight**.

Differences from the plan:

- **Six labels.** On the black panel background only six console foregrounds clear the 4.5:1 bar and stay distinct:
  bright red, dark yellow (amber), bright green, bright cyan, bright magenta and dark gray. Bright blue (~2.4:1) and the
  other dark colours fail, so they are not offered. All six pass the existing `ColorContrastTests` audit in both themes.
- **The marker is the label's number** in the second cell of the existing 2-cell selection-marker prefix (`* ` / `  `),
  not a glyph in a separator: no column moves, it works in ASCII-frame mode, and it says *which* label, not only that
  there is one. The cursor row, selected entries and broken entries keep their own colours; the number still shows.
- **`LabelStore` is a sorted map** keyed by the lexical path key (a lookup per visible row is O(log n); a folder's
  subtree is one contiguous range for move/remove).
- **Move/delete hooks** probe the disk only for items that carry a label and are post-hoc (the item is gone from the
  source and present at the destination), so they cover F6, F8, background jobs and the empty-folder deletion with one
  helper; the sync tool's own deletions and renames done outside the application leave orphans (documented).
- **Menu:** `Selection > Cycle colour label` and `Selection > Colour labels...`; `showLabels` hides without deleting.
