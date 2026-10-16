# Implementation Plan — LIST-012 (export the listing to a text or CSV file)

Source: a new requirement, proposed here. Builds on the panel's entries (name, size, time, attributes), the colour labels
(LIST-009) and the path rules of `PathUtil.h`.

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| LIST-012 (proposed) | The user shall be able to export the active panel's listing, or its selection, to a UTF-8 text or CSV file. | P3 | IN-MVP | Not Started |

## Summary

Reports, audits and hand-offs need a list of files. The panel already has the data; this writes it out.

- **Command:** **Commands → Export listing**. A form: *Format* (CSV or tab-separated text), *What* (the whole folder, or
  only the selection), *Include subfolders* (off; on exports the whole subtree through the search walk), *Output file*
  (default `listing-<folder>-<yyyymmdd-hhmm>.csv` in the user's Documents folder, typed path accepted).
- **Columns:** name (relative path when subfolders are included), type (extension, or `<DIR>`), size in bytes, modified
  (ISO `YYYY-MM-DD HH:MM:SS`, local), attributes (the R H S A letters), label (LIST-009 number or empty), full path.
- **Encoding:** UTF-8 with a byte-order mark for CSV, so Excel opens Unicode names correctly; plain UTF-8 without a BOM for
  text.
- **Safety:** the output file is never inside the folder being exported (an export must not change what it lists), and an
  existing output file is confirmed before it is overwritten.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| Panel entries with size, time, attributes | `Panel.h` `Entry` | The rows |
| Subtree walk | `Search.cpp` `SearchFiles` (SRC-001) | Subfolder export |
| Colour labels | `Labels.h` `LabelStore` (LIST-009) | The label column |
| Atomic UTF-8 file write | `Utf8TextFile.h` `WriteUtf8TextFileAtomic` | Writing the file (BOM option added) |
| Text prompt for the output path | `Dialog.h` `ShowTextPrompt` | Typing a path |
| Confirmation for an overwrite | `Dialog.h` `ShowChoiceDialog` | Overwrite question |
| Attribute letters | `main.cpp` `FormatAttributes` | The attribute column |

## Design

### D1 — Rows and encoding (pure, `ListingExport.{h,cpp}`)

- `ListingRow { relativeName, typeText, sizeBytes, modifiedLocal, attributes, label, fullPath }` and
  `FormatListing(rows, format)` returning the text.
- CSV per RFC 4180: a field containing a comma, a quote, a line break or a leading/trailing space is quoted, and quotes
  are doubled; a field starting with `=`, `+`, `-` or `@` is prefixed with a single quote, so a spreadsheet does not
  evaluate a file name as a formula (CSV injection).
- Tab-separated text: tabs and line breaks in names are replaced with spaces.
- Line endings CRLF (Windows convention).

### D2 — Gathering rows

- Selection or whole folder: from the panel's `Entries()` (no disk call), excluding `..`.
- With subfolders: a walk through `SearchFiles` with no mask filter, folders included, relative names built from the walk
  root; the labels are looked up by full path.
- Provider and plugin panels: refused in v1 (their entries are not local files).

### D3 — Output (`main.cpp`)

- `DoExportListing`: the form; the output path resolved (relative to the panel's folder); refusal if the output path is
  inside the exported folder tree; overwrite confirmation; the write through `WriteUtf8TextFileAtomic` (with a new BOM
  option); the outcome shown with the row count and the file path.
- The output path is not logged with file content; only the path and the count (ERR-005).

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/ListingExport.{h,cpp}` (new) | Rows, CSV and text formatting, CSV-injection guard (pure) |
| `Source/MyCommander/Utf8TextFile.h` | BOM option on the atomic write (default unchanged) |
| `Source/MyCommander/main.cpp` | `DoExportListing`, gathering, output checks |
| `Source/MyCommander/Strings.h` | Form labels, messages |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp`, `MenuBar.h`, `Help.cpp` | Command (menu only), help row |
| `Source/MyCommanderTests/ListingExportTests.cpp` (new), `KeyMapTests.cpp` | Tests below |
| Both `.vcxproj` files | New files |

## Tests

- **CSV (pure):** quoting of commas, quotes, line breaks and leading spaces; doubled quotes; the formula guard on `=`, `+`,
  `-`, `@` names; a name with a non-ASCII character; the header row.
- **Text (pure):** tabs and line breaks in names replaced; CRLF endings.
- **Rows:** type text (`<DIR>` for folders, extension otherwise); the time format; attribute letters; a label number or
  empty.
- **Gathering on `TempDir`:** selection-only export matches the selection; whole-folder export excludes `..`; subfolder
  export lists the nested files with relative names.
- **Output rules:** an output path inside the exported tree is refused (tested through the path helper).
- **Manual:** export a folder with a comma and an apostrophe in names, open the CSV in Excel, check the Unicode names and
  the formula guard.

## Documentation updates

- `AIPrompt/my-commander.md`: new row **LIST-012**; version bump; history paragraph.
- `README.md`: a "Export listing (LIST-012)" section with the columns and the encoding note; the Commands menu list.
- `USERGUIDE.md`: the same section in plain words, including the CSV-injection guard.
- `Source/MyCommander/Help.cpp`: F1 row (menu item only).
- `CLAUDE.md`: `ListingExport` in the source layout; the BOM option on `Utf8TextFile.h`; Testing list.
- `AIPrompt/ManualTests.md`: new MT entry.
- `AIPrompt/features/LIST-012-plan.md`: status and differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.

## Phasing

1. `ListingExport` formatting and tests (pure). 2. Row gathering and the output rules. 3. Form, command, menu, docs.

## Risks

- **CSV injection:** handled by the formula guard; a spreadsheet user may still see a leading apostrophe on such names,
  which is the intended trade-off (documented).
- **Large folders:** a 100 000-entry export is written row by row into the formatted text; memory is one string per export.
  A streamed writer is an option if this proves too large (open question 2).

## Open questions

1. **Default format:** CSV (recommended) or text.
2. **Streaming:** in-memory first (recommended), streamed later if needed.

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.44.0, version 2.8.0): `ListingExport.{h,cpp}`,
`ListingExportTests.cpp`, `Utf8TextFile.h` BOM option, `Commands > Export listing`, MT-061.

Open questions: CSV is the default format; in-memory formatting (cap 200,000 rows).

Differences from the plan:

- No F1 row and no key map entry: like Secure delete and Restore from Recycle Bin, the command is menu-only, and the
  help screen lists keys only.
- With "Selection only" and subfolders on, each selected folder is walked and its rows follow the selected entries.
- "Selection only" is the default choice when something is selected; the output file name follows the chosen format
  when the offered default was not edited.
- Attribute and modified columns are not given the formula guard (they are generated, and `----` must stay as is).
- The `SearchFiles` walk reports `\\?\` long-path names; the export strips the prefix so paths read normally.
