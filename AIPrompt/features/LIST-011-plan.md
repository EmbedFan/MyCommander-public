# Implementation Plan — LIST-011 (sort by attributes, and type as a named sort)

Source: a new requirement, proposed here. Builds on the existing sort keys (NAV-005, LIST-002/003).

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| LIST-011 (proposed) | The panel shall be sortable by file attributes, and the type sort shall be reachable by a named key and menu item. | P3 | IN-MVP | Not Started |

## Summary

Four sort keys exist: name, extension, size and modification time (Ctrl+F3–Ctrl+F6, LIST-002/003). Two gaps:

- **Type.** The panel's *Type* column shows the extension (`FormatType`), and the sort already called "extension"
  (Ctrl+F4) orders by it. So a separate "type" sort would duplicate it. **Decision here:** no new type sort. The existing
  sort is relabelled in the menu and the help as "Extension (type)", so the two names match what the user sees.
- **Attributes.** Sorting by attributes groups read-only, hidden, system and archive files together — useful when cleaning
  up. This is new.

What this plan adds:

- **Sort by attributes** (`Ctrl+F9`, both checked free): orders by the attribute bits, read-only first, then hidden, system,
  archive, reparse; within a group by name. Descending reverses the bit order, as the other sorts do.
- **Sort by attributes** and its label in the **View → Order by** menu, next to Name and Size.
- The existing "Extension (type)" label in the menu and F1 rows.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| `SortKey` enum and `CompareByKey` | `Panel.h`, `Panel.cpp` (LIST-002/003) | Adding `Attributes` |
| Attribute bits per entry | `Panel.h` `Entry::attributes` | The sort key |
| Sort menu items and keys | `MenuBar.h` View menu, `KeyMap.cpp` (Ctrl+F3–F6) | Menu item, key |
| `SortIndicator` in the header | `main.cpp` (Ctrl+F3–F6 header marker) | Showing the active sort |
| Session persistence of sort | `Session.cpp` | Not persisted today; unchanged |

## Design

### D1 — The sort key (`Panel.cpp`)

- `SortKey::Attributes` added after `ModifiedTime`. `CompareByKey` gets a case: compare a rank built from the attribute
  bits in a fixed order (read-only, hidden, system, archive, reparse point), then the name, the same tie rule as the other
  keys (`_wcsicmp`), so the order is always fully determined.
- The directories-first grouping and `..` handling are unchanged (they wrap `CompareByKey`, as now).

### D2 — Keys, menu, indicator

- `Ctrl+F9` → `sort-attributes` (`SortAttributes` command). Menu item **View → Order by Attributes**.
- The header's sort marker shows `A` for attributes, following `SortIndicator`.
- Pressing `Ctrl+F9` again reverses, as the other sort keys do (NAV-005).

### D3 — Naming the existing sort

- Menu item **Order by Extension** → **Order by Extension (type)**, and the F1 row says the same. No behaviour change, no
  key change.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/Panel.{h,cpp}` | `SortKey::Attributes`, rank comparison |
| `Source/MyCommander/main.cpp` | `DoSort` case, `SortIndicator` marker |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp` | `SortAttributes` command, `Ctrl+F9` |
| `Source/MyCommander/MenuBar.h` | Menu item; relabelled extension item |
| `Source/MyCommander/Help.cpp` | F1 row; relabelled extension row |
| `Source/MyCommanderTests/PanelTests.cpp`, `KeyMapTests.cpp` | Tests below |

## Tests

- **Rank (pure):** read-only before hidden; each bit on its own; a file with several bits sorts by the first bit in the
  fixed order; equal ranks fall back to name.
- **`Panel::SetSort(Attributes, desc)` on a `TempDir`:** the order matches the bits set with `SetFileAttributesW`;
  directories still grouped first; `..` still first; the cursor stays on its entry.
- **Key:** `Ctrl+F9` resolves to `SortAttributes`; it is in the built-in inventory (the consistency tests).
- **Manual:** a folder with mixed attributes; sort, reverse, check against Explorer's attribute columns.

## Documentation updates

- `AIPrompt/my-commander.md`: new row **LIST-011**; a note on LIST-002/003 that the extension sort is the type sort;
  version bump; history paragraph.
- `README.md`: keybindings table row (`Ctrl+F9`); the sort list in the Sorting section; the relabelled extension item.
- `USERGUIDE.md`: keybindings row; the sort section.
- `Source/MyCommander/Help.cpp`: F1 row for `Ctrl+F9`; relabelled extension row.
- `CLAUDE.md`: `Panel` description gains the attribute sort.
- `AIPrompt/ManualTests.md`: new MT entry.
- `AIPrompt/features/LIST-011-plan.md`: status and differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.

## Phasing

1. `SortKey::Attributes` and tests. 2. Key, menu, indicator. 3. Relabelling and docs.

## Risks

- **Expectation of a separate type sort:** a user may look for one. The relabelled menu item explains it; a true
  file-type sort (by a category instead of the extension) would need a mapping table, a separate decision.
- **Session persistence:** the sort is not saved between runs today (per panel, in memory). Adding it would touch the
  session store (NAV-008); out of scope here.

## Open questions

1. **Attribute order:** read-only, hidden, system, archive, reparse (recommended), or another order?
2. **Key:** `Ctrl+F9` (recommended, checked free).

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.46.0, version 2.10.0): `SortKey::Attributes`, `Ctrl+F9`,
View > Order by Attributes / Order by Extension (type), MT-063.

Open questions: attribute order read-only, hidden, system, archive, reparse; key `Ctrl+F9`.

Differences from the plan:

- The View menu had no extension item, so "Order by Extension (type)" was added rather than relabelled; the F1 row was
  relabelled "by extension (the type)".
- The header marker reads `Attr` (consistent with `Name`/`Ext`/`Size`/`Date`), not a single letter.
- `Ctrl+F9` is added to the plugin-conflict inventory next to plain `F9`.
