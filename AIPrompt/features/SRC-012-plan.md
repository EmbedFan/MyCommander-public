# Implementation Plan — SRC-012 (find entries by attribute)

Source: a new requirement, proposed here. Depends on SRC-010 phase 1 (the extended `SearchResult` and `EntryFilter`).

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| SRC-012 (proposed) | The application shall list the files and folders below a chosen folder that have chosen Windows attributes (read-only, hidden, system, archive, reparse point, compressed, encrypted), matching all or any of the chosen ones. | P3 | IN-MVP | Not Started |

## Summary

Read-only files that block a sync, hidden files left by a tool, reparse points that point nowhere, compressed or
encrypted files that slow a backup. Explorer cannot filter by these at all. This adds **Commands → Find by attribute**.

- **Form:** folder (default: the active panel's folder); checkboxes **Read-only**, **Hidden**, **System**, **Archive**,
  **Reparse point**, **Compressed**, **Encrypted**; *Match* as a choice: **all checked attributes** (default) or **any**;
  *Include subfolders* (on); *What to list*: files, folders or both (default both); name mask (`*`).
- **Result:** the matching entries, by path, each row showing its attribute letters (R H S A L C E, the same letters the
  panel's attribute column uses where they exist) and its size.
- **Actions:** `Enter` shows the entry in the active panel, `F3` views a file. Read-only: attribute changes are done with
  the existing Ctrl+A on the panel, after the user has seen the list.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| Attribute bits from the listing | `Panel.h` `Entry::attributes`, `PathUtil.h` `GetAttributesOrInvalid` | The attribute test |
| Attribute letters for the column | `main.cpp` `FormatAttributes` (CON-005) | The row's letters |
| Extended `SearchResult` and `EntryFilter` | SRC-010 D1 | The filter, no extra calls |
| Results screen | `main.cpp` `DoSearch` results loop | The list |

## Design

### D1 — The attribute predicate (pure, `Search.cpp`)

- `AttributeMatch(attributes, wanted, mode)`: `wanted` is a bit mask of the seven attributes; `mode` is *all* or *any*.
  An empty `wanted` matches nothing (the form refuses an empty choice rather than listing the whole tree).
- The letters for a row come from one pure function (`AttributeLetters(attributes)`), tested with the same bits.
- Compressed and encrypted are the Win32 `FILE_ATTRIBUTE_COMPRESSED` and `FILE_ATTRIBUTE_ENCRYPTED` bits from the same
  listing; no per-file `GetCompressedFileSize` call.

### D2 — The walk

- `SearchByAttribute(root, wanted, mode, options, cancelPoll, onProgress)` is `SearchFiles` with the attribute predicate
  as its `EntryFilter`.
- **Walk rule (decided here, stated in the help text):** the walk enters every folder **except reparse points**, including
  hidden and system folders, because an attribute search exists to find hidden and system items. A reparse point is
  reported when it matches, but is never entered (SEC-006). The panel's own "show hidden" setting does not apply.
- Unreadable folders are counted and skipped, not reported.
- The result cap (5 000) and `truncated` flag as for every search.

### D3 — Form, results, command

- `DoFindByAttribute` in `main.cpp`: the checkboxes and the match choice in one `ShowFormDialog`; an empty choice re-shows
  the form; the shared results loop with the attribute letters in the row.
- `CommandId::FindByAttribute` / `find-by-attribute`, menu item under **Commands** (menu only by default; open question 2).

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/Search.{h,cpp}` | `AttributeMatch`, `AttributeLetters`, `SearchByAttribute` |
| `Source/MyCommander/Strings.h` | Form labels, match choices, errors |
| `Source/MyCommander/main.cpp` | `DoFindByAttribute`, results rows |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp`, `MenuBar.h`, `Help.cpp` | Command and menu item |
| `Source/MyCommanderTests/SearchTests.cpp` | Tests below |
| `Source/MyCommanderTests/KeyMapTests.cpp` | Inventory check |

## Tests

- **`AttributeMatch` (pure):** all-mode requires every wanted bit; any-mode requires one; an empty mask matches nothing;
  each of the seven bits on its own.
- **`AttributeLetters` (pure):** the letters for each bit and for none.
- **`SearchByAttribute` on real `TempDir` trees:**
  - a read-only file and a hidden file are found by their own bit, and by "all" when both are set;
  - a hidden folder's contents are found (the walk enters hidden folders);
  - a junction (`test::CreateJunction`) is reported when *Reparse point* is chosen and never entered;
  - an empty choice is refused by the form helper (tested directly);
  - cancel and cap.
- **Manual:** a tree with a read-only file, a hidden folder, a junction and an archive-flagged file; run each choice and
  compare with Explorer's properties.

## Documentation updates

- `AIPrompt/my-commander.md`: new row **SRC-012**; version bump; history paragraph.
- `README.md`: a "Find by attribute (SRC-012)" section stating the walk rule (hidden folders are entered, reparse points
  are not); the Commands menu list.
- `USERGUIDE.md`: a short section with the same walk rule in plain words.
- `Source/MyCommander/Help.cpp`: none (menu only).
- `CLAUDE.md`: `Search.{h,cpp}` description (`SearchByAttribute`); Testing list.
- `AIPrompt/ManualTests.md`: new MT entry.
- `AIPrompt/features/SRC-012-plan.md`: status and differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.

## Phasing

1. `AttributeMatch` and `AttributeLetters` with tests (pure). 2. `SearchByAttribute` (needs SRC-010 phase 1).
3. Form, results, command, menu, docs.

## Risks

- **Entering hidden folders is a deliberate difference from the panel:** a user may expect the panel's hidden setting to
  apply. The form's help line and the README say so; the alternative (skip hidden folders) would hide the very items the
  attribute search is for.
- **Encrypted and compressed bits on a network share** may not be reported by the server. Such files simply do not match;
  the list does not claim otherwise.

## Open questions

1. **Match default:** all checked (recommended) or any?
2. **Key:** menu only (recommended for now).

## Status

Planned 2026-10-15. **Postponed 2026-10-16** by the user's decision; not started. The plan stays valid. Before
implementing: SRC-010 is done but added only `SearchResult::sizeBytes`, so this feature (or SRC-011, whichever comes first)
adds `lastWriteUtc`, `attributes` and the `EntryFilter` predicate described in SRC-010's plan.
