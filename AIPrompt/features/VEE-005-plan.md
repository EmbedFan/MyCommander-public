# Implementation Plan — VEE-005 (Hexadecimal viewing)

Source: `AIPrompt/my-commander.md`, `VEE-005` — "Hexadecimal viewing may be supported." P2, was `LATER` /
`Not Started`, listed in the Postponement Register (§30) alongside `VEE-007`.

## Revision history

- **Revision 1 (initial plan):** widths 8/16/32/64, `W` cycled through them, pan horizontal.
- **Revision 2 (user feedback, implemented):** the count of items per row is **8, 16, 32, or fit** to the
  terminal width. 64 is removed. `W` cycles through all four settings. "Bit" in the original request means
  the number of items (bytes) per row, which settles Open item 1.
- **Revision 3 (user feedback, implemented):** hex view gets a real cursor — a single byte, highlighted in
  both the hex digits and the ASCII column — separate from the view's scroll position. `Up`/`Down`/`Left`/
  `Right`/`Page Up`/`Page Down`/`Home`/`End` move the cursor; manual panning (Revision 1/2's `Left`/`Right`)
  is gone, replaced by the view auto-scrolling/panning to keep the cursor visible. The status line's offset
  is the cursor's position, not the first visible byte. See the revised D1/D3/D4/D7/D8 below.

## User decisions (confirmed)

- `VEE-005` moves to `IN-MVP`.
- Hex view is a key on the default viewer (F3 / Enter-on-file via `internal:viewer`), not a separate command.
- Toggle key `H`.
- Items per row: **8, 16, 32, or fit**. Default **8** each time the viewer opens.
- The switch between them is a keyboard key (`W`), and the current setting is always visible on screen.
- Search must work in **both** ASCII and hex mode.

## Summary

The built-in viewer gains a hex mode toggled with `H`. In hex mode the screen shows an offset column, hex
bytes, and an ASCII column. The number of items per row is 8, 16, or 32, or **fit**, which chooses the
largest of those that fits the terminal width. The byte position is kept across toggles. Search (`/`, `n`)
works in both modes for ASCII text and for hex byte patterns.

`VEE-004` (text search, line navigation, encoding, large-file handling) stays Implemented; text-mode behavior
is unchanged except for the added hex-pattern search syntax.

## What exists today

| Piece | Location | Relevance |
|---|---|---|
| Viewer loop, keys, rendering | `Viewer.cpp` (`ShowTextFileViewer`) | Host. Keys in use: arrows, PgUp/PgDn, Home/End, Esc/F3/F10, `/`, `n`. |
| Line index (byte offsets) | `TextFile.h` (`IndexTextFileForViewing`, `lineStarts`/`lineEnds`) | Byte ↔ line mapping for the toggle and for jumping search hits in text mode. |
| Search prompt | `Dialog.cpp` (`ShowTextPrompt`) | Reused for the `/` prompt. Has a real cursor (IS-0015 Part 3). |
| Status / hint strings | `Strings.h` | Localization-ready strings (ACC-005). |
| Viewer docs | `Help.cpp`, `README.md`, `USERGUIDE.md` | Updated together. |

## Design

### D1 — Keys inside the viewer

| Key | Text mode | Hex mode |
|---|---|---|
| `H` | Switch to hex. | Switch to text. |
| `W` | (no effect) | Cycle items per row: 8 → 16 → 32 → fit → 8. |
| `↑` / `↓` | Scroll one line. | Move the cursor one row. |
| `←` / `→` | (no effect) | Move the cursor one byte. |
| `PgUp` / `PgDn` | One screen. | Move the cursor one screen of rows. |
| `Home` / `End` | First / last line. | Move the cursor to the file's first / last byte. |
| `/` | Search prompt. | Search prompt. |
| `n` | Repeat search. | Repeat search. |
| `Esc` / `F3` / `F10` | Close viewer. | Close viewer. |

**Revision 3:** hex mode has a cursor (D4), and `↑`/`↓`/`←`/`→`/`PgUp`/`PgDn`/`Home`/`End` all move it; the
view scrolls/pans on its own to keep it visible (D3). Revision 1/2's manual `←`/`→` pan is gone.

`W` is a single cycle key, so it fits the existing one-letter viewer keys. Number keys were rejected because
"8/16/32/fit" as digits is not intuitive. Alternative: `Numpad +`/`-`. Confirm if preferred.

The setting is kept for the viewer session only, not persisted to config. Default on each viewer open is 8.

The current setting is shown in the status line at all times in hex mode, e.g. `8 items/row` or
`fit: 16 items/row`, so a user switching with `W` can always see the result.

### D2 — Pure module `HexDump.{h,cpp}`

`Console`-free, unit-testable (CLAUDE.md pure-logic precedent):

```cpp
namespace mc {
// The user-selectable item counts per row, in cycle order.
inline constexpr size_t kHexItemCounts[] = {8, 16, 32};
// Setting value meaning "fit to the terminal width". Not a real item count.
inline constexpr size_t kHexFitSetting = 0;
// Cycles the setting: 8 -> 16 -> 32 -> fit -> 8. Any unknown value returns 8.
size_t NextHexItemSetting(size_t setting);
// The actual items per row for a setting. Fit returns the largest of kHexItemCounts
// whose full row fits `consoleWidth`, or 8 if none does. A fixed count is returned as-is.
size_t ResolveHexItems(size_t setting, size_t consoleWidth);
// Columns a full row occupies: offset(16) + 2 + items*3 + 1 + items (ASCII).
size_t HexRowColumns(size_t bytesPerRow);
// One row: "0000000000001230  48 65 6C ... |Hello ...|". ASCII column: 0x20–0x7E as-is, else '.'.
std::wstring FormatHexRow(uint64_t offset, const uint8_t* bytes, size_t count, size_t bytesPerRow,
                          size_t firstColumn, size_t maxColumns);
uint64_t HexRowStart(uint64_t byteOffset, size_t bytesPerRow);
uint64_t HexMaxAnchor(uint64_t fileSize, size_t bytesPerRow, size_t visibleRows);
size_t HexMaxPan(size_t bytesPerRow, size_t visibleColumns);
// Line whose [lineStart, nextLineStart) contains byteOffset (binary search over lineStarts).
size_t LineIndexForByte(const std::vector<uint64_t>& lineStarts, uint64_t byteOffset);
// Hex pattern parsing and the chunked byte search engine (see D6).
}
```

`NextHexItemSetting` replaces Revision 1's `NextHexWidth`, and `kHexFitSetting` makes "fit" explicit in the
data rather than a magic number in the viewer.

Columns per row: 8 items → 16+2+24+1+8 = 51; 16 → 83; 32 → 147.

### D3 — Terminal width vs. row width (Revision 3: auto-scroll/pan follows the cursor)

- **Fit** never needs panning: `ResolveHexItems` only picks a count whose row fits the console width. Below
  51 columns no count fits, and fit falls back to 8, which then pans. The minimum terminal size is 60
  columns (UI-006), so fit always resolves to a count that fits in the supported range.
- **Fixed 16 and 32** can be wider than the terminal (16 needs 83 columns, 32 needs 147). There is no manual
  pan key any more (Revision 1/2 had `←`/`→` pan by 8 columns); instead, `ensureCursorVisible` pans just far
  enough that both the cursor's hex pair and its ASCII character are on screen, clamped to
  `[0, HexMaxPan(items, visibleColumns)]`. Moving the cursor across a wide row scrolls it into view a column
  at a time, the way moving past the right edge of a text editor does.
- The status line shows the visible column range regardless, e.g. `cols 1-80 of 147`, so a user can tell a
  row is wider than the screen even before panning.
- `W` resets pan to 0 and re-anchors the view on the cursor's row, so the cursor's own column decides the pan
  needed afterward (via `ensureCursorVisible`), not a stale pan value from the previous setting.
- Rejected: hiding counts that don't fit (the user wants the fixed counts selectable), and wrapping rows
  (breaks one-row-per-offset).

### D4 — Byte-position anchoring and the cursor (Revision 3)

- **`hexAnchor`** is the first visible byte, purely a rendering detail of which rows are on screen. Always
  row-aligned to the resolved items-per-row.
- **`hexCursor`** (new in Revision 3) is the single byte the cursor is on — the thing the user is actually
  pointing at. It is independent of `hexAnchor`; `ensureCursorVisible` is what keeps the two consistent,
  moving `hexAnchor` (and the pan) by the minimum needed to keep `hexCursor` on screen after every cursor
  move, resize, or setting change — vertically via `HexRowStart`/`HexMaxAnchor`, horizontally via the same
  pan logic as D3.
- Text → hex (`H`): `hexCursor = lineStarts[topLine]`, `hexAnchor` starts equal to it then gets row-aligned by
  `ensureCursorVisible`.
- Hex → text (`H`): `topLine = LineIndexForByte(lineStarts, hexCursor)` — the cursor's line, not whatever
  happened to be scrolled to the top.
- A search hit sets both: `hexCursor = hit`, `hexAnchor = HexRowStart(hit, items)` (so the match's row is at
  the top, as before), then `ensureCursorVisible` for the pan.
- Changing the setting (`W`) keeps `hexCursor`'s byte value unchanged; `hexAnchor` and the pan are
  recomputed around it. Fit re-resolves on every paint, so a terminal resize keeps the cursor visible too.

### D5 — Bounded memory

- Total rows = `ceil(fileSize / items)`; `fileSize` from `std::filesystem::file_size`.
- Paint reads only the visible window: seek to the anchor, then at most `visibleRows * items` bytes into a
  fixed buffer. A multi-GB file costs one screen of bytes per paint.
- Reuses the viewer's already-open binary `std::ifstream`.

### D6 — Search (ASCII and hex, both modes)

**Query syntax** in the `/` prompt:

- Plain text → ASCII search, case-insensitive for `A–Z` only. In hex mode its UTF-8 bytes are searched.
- Leading `#` → hex byte pattern: `#4D 5A` or `#4D5A`. An odd digit count, or any non-hex character, is
  rejected with a message, and the viewer position is unchanged.
- The prompt's title shows the syntax: `Find: text, or #hex bytes (e.g. #4D 5A)`. The title is fixed
  because `ShowTextPrompt` has no per-keystroke title hook (Phase 2 deviation, kept).

**Engine (pure, in `HexDump.cpp`):** `FindBytePattern(in, fileSize, pattern, fromByte, caseFoldAscii)`
scans forward in 64 KiB chunks with an overlap of `pattern.size()-1` bytes, so matches across a chunk
boundary are found. It searches match starts in `[fromByte, end)` and then wraps to `[0, fromByte)`, the
same wrap as the text `findNext`. Memory is bounded by the chunk size.

**Behavior by mode:**

- **Hex mode:** the match offset sets both `hexCursor` and `hexAnchor` (D4), so the match row is at the top
  and the cursor sits on the match's first byte. Its bytes are highlighted (D7).
- **Text mode, ASCII query:** keep the existing line-by-line decoded search (VEE-004), so UTF-16 and
  full-width text still match. No regression.
- **Text mode, hex query:** byte search. On a hit, `topLine = LineIndexForByte(...)`. Hex patterns can't be
  expressed line-wise.
- **Hex mode, ASCII query:** byte search over raw bytes, case-insensitive for `A–Z`. The user is looking at
  raw bytes, so a match they cannot see in the ASCII column would be confusing.

`n` repeats the last query from the current position, starting after the cursor (hex mode) or the current line (text mode).

### D7 — Highlighting the match and the cursor (hex mode)

After a hex-mode hit, the matched bytes are drawn with `kAttrHeaderActive` (the existing "focused" highlight,
as in IS-0015 Part 3). Only bytes inside the visible window are highlighted. A match spanning two rows
highlights both. The highlight is cleared on the next keypress.

**Revision 3:** the cursor itself is always drawn too, in both the hex pair and the ASCII character for
`hexCursor`'s byte — this is the literal answer to "the cursor can be in the hex part or the ascii part":
both representations of the same byte are highlighted together, not two independently-movable cursors. It
uses `Theme.h`'s `kAttrCursor` (added for IS-0016's line-edit cursors), kept visually distinct from a search
hit's `kAttrHeaderActive`. Drawn after the hit highlight, so where the two coincide the cursor is the one
visible on top.

### D8 — Status line and hints

- Text mode: unchanged, hint gains `H hex`.
- Hex mode status: `Offset 0x00001230 / 0x0000ABCD  16 items/row  cols 1-80 of 83`. **Revision 3:** `Offset`
  is `hexCursor`'s position, not `hexAnchor`'s — "the offset shall be shown calculated [from] the current
  position of the cursor." The items part reads `8 items/row`, `16 items/row`, `32 items/row`, or
  `fit: 16 items/row` (fit with the resolved count).
- Hex mode hint: `W items  Arrows: Move cursor  / search  n next  H text` (Revision 3: no separate pan hint).
- All strings in `Strings.h`.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/HexDump.{h,cpp}` (new) | Item setting, fit resolution, row formatting, anchor/scroll math, byte search engine. Pure. |
| `Source/MyCommander/Viewer.cpp` | Setting, anchor, cursor, pan state; `H`/`W`/arrow keys; hex render branch; byte-search dispatch; hit and cursor highlight; `ensureCursorVisible`. |
| `Source/MyCommander/Strings.h` | Hex status/hint, search prompt title, invalid-hex-pattern message. |
| `Source/MyCommander/Help.cpp` | Built-in viewer (F3) section: `H`, `W`, cursor keys, search syntax. Mirrors README. |
| `Source/MyCommanderTests/HexDumpTests.cpp` (new) | Unit tests (below). |
| `Source/MyCommanderTests/MyCommanderTests.vcxproj` | Add `HexDumpTests.cpp` and `..\MyCommander\HexDump.cpp`. |
| `Source/MyCommander/MyCommander.vcxproj` (+ `.filters`) | Add `HexDump.cpp`/`HexDump.h`. |
| `README.md` | Viewer row, hex-view subsection. |
| `USERGUIDE.md` | Viewing section. |
| `AIPrompt/my-commander.md` | `VEE-005` wording and status; Postponement Register split; version history; header sync. |
| `Source/MyCommander/Version.h` | MINOR bump. |
| `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md` | Entry per phase, real timestamp. |

## Test plan

Unit (`HexDumpTests.cpp`, pure):

1. `NextHexItemSetting`: 8→16, 16→32, 32→fit, fit→8; unknown input → 8.
2. `ResolveHexItems`: fit at 60 columns → 8; at 90 → 16; at 150 → 32; at 50 (below any fit) → 8; a fixed
   count returns itself regardless of width (32 at 60 → 32).
3. `HexRowColumns`: 51 / 83 / 147 for 8 / 16 / 32.
4. `FormatHexRow`: full 8-item row, exact string.
5. `FormatHexRow`: partial final row keeps the ASCII column aligned.
6. `FormatHexRow`: 0x00, 0x0A, 0x7F, 0xFF → `.`; printable ASCII as itself.
7. `FormatHexRow`: offset above 4 GB formats as 16 hex digits.
8. `FormatHexRow` with `firstColumn`/`maxColumns`: pan slice is correct and never exceeds `maxColumns`;
   a pan past the end is empty.
9. `HexRowStart`: rounds down for 8, 16, 32.
10. `HexMaxAnchor`: empty or short file → 0; last full screen is row-aligned.
11. `HexMaxPan`: zero when the row fits; remainder otherwise (51 at 30 → 21; 147 at 80 → 67).
12. `LineIndexForByte`: line start, mid-line, last line, CRLF ending, empty index → 0, byte before first line → 0.
13. Anchor round trip: text line N → byte → hex row → text line N returns N, for each count.
14. Setting change keeps the first visible byte once re-aligned to the new count.
19. (Revision 3) `ensureCursorVisible`'s row/pan math is exercised through `Viewer.cpp`'s own cursor-move
    paths rather than as a separate pure function — consistent with `clampHex`/`anchorFromTopLine` already
    being untested Viewer-local lambdas (VEE-003's precedent: `Viewer.cpp` has no dedicated test file).
15. `FindBytePattern`: match at offset 0, at end, across a 64 KiB chunk boundary, not found, and wrap from a
    position past the last match.
16. ASCII case-folding: `"abc"` matches `"ABC"`; non-letters unchanged.
17. Hex pattern parsing: `"4D 5A"`, `"4d5a"`, `"4D5"` (odd, rejected), `"4G"` (rejected), empty and whitespace-only (rejected).
18. Zero-length file: no division by zero; search returns not-found.

Manual (no interactive capture in this project; same limitation as prior viewer work):

- Toggle `H` on text and binary files; position preserved both ways.
- Cycle `W` through 8, 16, 32, fit at 60, 80, and 160 columns; confirm the status shows the active setting and
  the resolved count for fit; confirm the cursor stays on the same byte.
- Move the cursor with all eight movement keys at 16 and 32 items/row on an 80-column terminal; confirm the
  view auto-scrolls/pans to keep the cursor visible, and the status offset tracks the cursor, not the anchor.
- Search a hit in hex mode, then move the cursor onto a highlighted byte; confirm the cursor is visible (drawn
  on top of the hit highlight).
- Search: ASCII in text mode (incl. UTF-16), hex pattern in text mode, ASCII and hex in hex mode; a match
  crossing a row boundary highlights on both rows.
- Multi-GB file: scrolling responsive, memory flat.
- `Esc`/`F3`/`F10` close from either mode.

## Risks and notes

- **32 items at 80 columns** shows only part of each row until the cursor is moved into the hidden part,
  which auto-pans. The status line shows the visible range so the user isn't confused. Fit avoids this
  entirely for users who don't want to move the cursor sideways just to see the rest of a row.
- **Text-mode hex search** scans raw bytes, so on a UTF-16 file the hex pattern must be the UTF-16 bytes.
  The hint explains this; no automatic encoding translation is attempted.
- **Chunked search cost** on a multi-GB file is one full read per search. Acceptable for an explicit action.
- **Index build on open** is the existing one-pass line scan; hex mode adds no second one.
- **Help/README/USERGUIDE sync** required in the same change as every keybinding change.

## Phasing

- **Phase 1 (done):** `HexDump` pure module, byte search engine, `HexDumpTests.cpp`.
- **Phase 2 (done, revised in Revisions 2 and 3):** viewer integration with the 8/16/32/fit setting,
  `ResolveHexItems`, the status-line item text, and (Revision 3) the `hexCursor`/`ensureCursorVisible` cursor.
- **Phase 3 (done, revised in Revisions 2 and 3):** Help, README, USERGUIDE, spec, version (now 1.13.0),
  changelog, all describing 8/16/32/fit, no 64 option, and the cursor.
- **Revision 2 (implemented):** 8/16/32/fit, no 64.
- **Revision 3 (implemented):** the cursor, auto-scroll/pan, and offset-of-cursor described above.

## Open items

1. **Fit meaning.** Read as "the largest of 8/16/32 that fits the terminal width". If you meant any count that
   fits (e.g. 24 items), say so and fit becomes a computed count with no fixed set.
2. **Width key `W`** as a single cycle key. Alternative: `Numpad +`/`-`.
3. **Highlight color.** The search hit still reuses `kAttrHeaderActive`; the cursor (Revision 3) uses the
   separate `kAttrCursor`. A dedicated search-hit color could go in `Theme.h` instead of reusing
   `kAttrHeaderActive`, which would also need the ACC-002 contrast audit.
4. **Home/End in hex mode (Revision 3)** move the cursor to the file's first/last byte, continuing the
   pre-cursor behavior (jump the view to the start/end of the file). An alternative, not implemented: jump to
   the start/end of the cursor's *current row* instead, with `Ctrl+Home`/`Ctrl+End` for the whole file — more
   like a text editor's Home/End. Say so if that's preferred.
5. **"It can be in the hex part or the ascii part"** is implemented as one cursor highlighted in both
   representations of the same byte simultaneously (D7), not two independently-positionable cursors or a
   Tab-switchable focus between the two panes. Say so if a Tab-switchable focus (e.g. for a future editing
   mode where typing behaves differently in each pane) was intended instead.
