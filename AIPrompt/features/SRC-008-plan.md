# Implementation Plan — SRC-008 (Text-file difference viewer)

Source: `AIPrompt/my-commander.md` §16:

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| SRC-008 | A text-file difference viewer may be integrated later. | P3 | LATER | Not Started |

Neighbours:

- SRC-006: compare folders, `Shift+F2`;
- SRC-007: synchronize, `Ctrl+Shift+F2`;
- VEE-003/004: the built-in viewer, DEC-003;
- SRC-004/005: content search, binary detection and encodings.

## Implementation check (1.26.0)

Nothing compares the *contents* of two text files today. What exists and is reusable:

| What | Where | Reuse |
|---|---|---|
| File → "Compare contents" (C) | `MenuBar.h:104`, a **stub** since FEA-0005 | Becomes the real entry point |
| Folder-compare results list: F3 / Shift+F3 view the left / right file of the cursor item | `main.cpp` `DoCompareDirectories` | Gets a "show differences" key for an item present on both sides |
| Streaming line reader with UTF-8 / ANSI / UTF-16 (BOM) detection; CRLF/LF stripped | `TextFile.h` `ForEachTextLine` | Reads both files' lines |
| Binary detection | `ContentSearch.h` `LooksBinary` | Binary files get a byte comparison instead |
| Byte-by-byte equality with cancel and progress | `DirCompare.h` `FilesHaveEqualContent` | The model for "where do two binary files first differ" |
| Full-screen scrolling viewer: Console drawing, horizontal offset, search, status line | `Viewer.cpp` | The model for the diff screen's loop (a separate module, not a mode of the viewer) |
| Refusing plugin/tree panels for a two-panel operation | `main.cpp` `RefuseCompareOfPanels` | Reused |
| Themes with a contrast test (UI-010) | `Theme.h`, the `ConsoleAttrContrastRatio` tests | Three new colours: removed, added, changed |
| Rebindable command table (KEY-005: every command needs a default key) | `KeyMap.cpp` `Commands()` | A new `compare-files` command |

## Summary

A built-in **side-by-side difference viewer** for two text files:

- **Layout:** left file | right file, aligned line by line, with line numbers. Removed, added and changed lines
  are coloured, and the changed characters inside a changed line are highlighted. A *unified* (one-column) view
  is one key away, and is used automatically on narrow windows.
- **Moving around:** jump to the next/previous difference; scroll both sides together vertically and
  horizontally.
- **Options:** ignore case, and ignore whitespace (none / trailing / all), toggled live.
- **Binary files:** a byte comparison that reports the first differing offset.
- **Opening it:**
  - File → "Compare contents";
  - its own key;
  - the folder-compare results list, on an item that exists on both sides.

It only shows differences. **Editing or merging** (copying a block to the other side) is a natural follow-up,
but is not part of this requirement (proposed SRC-008a, LATER).

## Design

### D1 — Which two files (pure `ChooseFilesToCompare`)

| Situation | Compared |
|---|---|
| Exactly **two files selected** in the active panel | Those two (in panel order) |
| Otherwise, the active panel's **selected file, or the file under the cursor**, and the other panel has a **file with the same name** | That pair: the left-panel file on the left, the right-panel file on the right |
| Otherwise, the active panel's file and the **file under the other panel's cursor** | That pair (the Total Commander rule) |
| Folders, `..`, nothing usable, or a plugin/tree panel | A message saying what to select (`RefuseCompareOfPanels` for content panels) |

The rule is a pure function of names, cursor and selection, so it's unit-tested without a console.

### D2 — The diff engine (new pure module `TextDiff.{h,cpp}`, its own `TextDiffTests.cpp`)

```cpp
struct DiffOptions { bool ignoreCase = false; WhitespaceMode whitespace = WhitespaceMode::Exact; };  // Exact/Trailing/All
enum class RowKind { Same, Removed, Added, Changed };
struct DiffRow { RowKind kind; std::optional<size_t> leftLine, rightLine; };   // 0-based line numbers
struct DiffResult { DiffStatus status; std::vector<DiffRow> rows; size_t removed, added, changed; };  // Ok/Cancelled/TooLarge
DiffResult ComputeLineDiff(const std::vector<std::wstring>& left, const std::vector<std::wstring>& right,
                           const DiffOptions&, const CancelPoll&, const ProgressFn&);
std::vector<std::pair<size_t,size_t>> ChangedSpans(const std::wstring& a, const std::wstring& b, const DiffOptions&); // intra-line
size_t NextDifference(const std::vector<DiffRow>&, size_t from); size_t PreviousDifference(...);
```

- **Algorithm:** Myers' O((N+M)·D) diff in its **linear-space** form (middle snake, divide and conquer).
  Before it runs, the common prefix and suffix are trimmed and lines are compared by a hash of their
  *normalized* text (case-folded through `TextCase.h`, whitespace per the option). Equal hashes are confirmed
  by a text comparison.
- **Pairing:** each run of removed + added lines is paired off into **Changed** rows (left line *i* beside
  right line *i*), and the remainder stays Removed or Added. This gives the classic side-by-side alignment.
- **Bounded:**
  - files over **200,000 lines or 64 MB** each are refused with a message, falling back to the byte comparison;
  - a difference count over **20,000 edits** stops with `TooLarge` ("the files are too different to align");
  - cancel is polled while computing (Esc), and progress is shown for long runs (PER-002 style).
- **Intra-line highlight:** a small character-level diff of each Changed pair (the same algorithm on
  characters), capped at 2,000 characters per line; longer lines are highlighted as a whole.
- **Binary:** `LooksBinary` on either file's start → pure `FirstDifferenceOffset(a, b)` (streamed, 64 KB chunks,
  cancel) → "identical" or "differ at byte N (0x…)", plus the two sizes.

### D3 — The diff screen (new `DiffViewer.{h,cpp}`, Console-based like `Viewer.cpp`)

```
 Compare: C:\work\a\config.ini  |  D:\backup\a\config.ini            3 changes: -2 +5 ~4   [ignore: none]
   12 timeout = 20               |   12 timeout = 20
   13 retries = 3                ~   13 retries = 5                    <- changed: "3"/"5" highlighted
   14 log = off                  <                                    <- removed (left only)
                                 >   14 proxy = auto                  <- added (right only)
 F7/Shift+F7 next/prev difference  U unified  I ignore case  W whitespace  Esc close
```

- **Rows:** a fixed-width line-number gutter on each side; a middle marker column (`~` changed, `<` removed,
  `>` added) so the meaning survives monochrome and high-contrast themes (UI-010 / ACC — not colour alone).
- **Keys:**
  - ↑/↓, PgUp/PgDn, Home/End: scroll both sides together; ←/→: horizontal scroll;
  - **F7 / Shift+F7** (also `n` / `p`): next / previous difference, centred;
  - **U**: unified view (`-`/`+` prefixes, one column);
  - **I**: ignore case; **W**: cycle whitespace (exact → trailing → all), recomputed with progress;
  - **F3 / Shift+F3**: open the left / right file in the normal viewer at the current line;
  - **Esc / F10**: close.
- **Unified view by default** when the window is narrower than 100 columns.
- **Text width:** all text goes through `TextWidth.h` (UI-011). Tabs are expanded (the viewer's tab width).
  Full-width characters are padded correctly in both columns.
- **The header:** both paths (cut from the left), each file's encoding if they differ (e.g. "UTF-16 | UTF-8"),
  the counts, and the active options. **Identical** files open a message ("The files are identical" — "…when
  ignoring whitespace" if an option made them so) instead of an all-white screen.
- **Colours:** three new theme attributes, `attrDiffRemoved`, `attrDiffAdded`, `attrDiffChanged` (plus the
  intra-line highlight using `attrSelected`), in both the default and the high-contrast palette, added to the
  existing contrast test.

### D4 — Entry points and keys

- **File → "Compare contents" (C):** the stub becomes a real item for the new command.
- **Its own key — recommended `Alt+F3`** ("view the differences", next to F3 view / Shift+F3 choose a viewer):
  - a new KEY-005 command `compare-files`, rebindable;
  - it's free today (checked against `BuiltinShortcutInventory`), and once added, plugins can't claim it.
- **The folder-compare results list:** `Alt+F3` on an item that exists on both sides as files opens the diff of
  that pair (F3 / Shift+F3 keep viewing one side). The results hint line gains it.
- **F1 help, README, USERGUIDE, the hint bar** (the File/Commands context) and the menu's key column follow the
  user's binding automatically (KEY-005).

### D5 — An external diff tool (optional, recommended)

A new `Config` key, `diffProgram` (CFG, DEC-007 text), e.g.:

```
diffProgram = "C:\Program Files\WinMerge\WinMergeU.exe" "%1" "%2"
```

- When it is set, the command starts that program detached (`Process.h` `RunDetachedCommandLine`, SEC-004
  quoting, the same `%1`/`%2` expansion as file actions) instead of the built-in viewer.
- `Shift` together with the command forces the built-in viewer.
- The default stays empty, so the built-in viewer is used.

## Requirements (spec §16 row update + a follow-up)

| ID | Change |
|---|---|
| SRC-008 | LATER → IN-MVP; wording: "A text-file difference viewer shows two files side by side (or unified) with line, character and whitespace/case-insensitive comparison; binary files report the first differing byte. File → Compare contents, `Alt+F3`, and the folder-compare results list open it." → Implemented |
| SRC-008a (new, LATER) | Merging: copy a difference block to the other side, edit, save — useful but not required |

The §29 note is updated as well ("SRC-008 implemented in 1.27.0").

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/TextDiff.{h,cpp}` (new) + `Source/MyCommanderTests/TextDiffTests.cpp` (new) | The engine, intra-line spans, `FirstDifferenceOffset`, `ChooseFilesToCompare` |
| `Source/MyCommander/DiffViewer.{h,cpp}` (new) | The diff screen |
| `Source/MyCommander/main.cpp` | `DoCompareFiles` (selection rule → read lines with progress → binary fallback → viewer or external tool); the key case; the folder-compare results key |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp` | `compare-files` command, `Alt+F3` |
| `Source/MyCommander/MenuBar.h` | "Compare contents" stub → real |
| `Source/MyCommander/Theme.h` | Three diff attributes in both palettes |
| `Source/MyCommander/Config.{h,cpp}` (D5) | `diffProgram` (commented in the default file) |
| `Source/MyCommander/Strings.h`, `Help.cpp`, `HintBar` strings | Texts, F1 group "Compare files", the results hint |
| Both `.vcxproj` files | New sources |
| `README.md`, `USERGUIDE.md` (new section "Comparing two text files"), `CLAUDE.md`, `AIPrompt/my-commander.md`, `Version.h` (1.27.0), changelog | Docs, spec, version |

## Tests

- **`TextDiffTests.cpp`:**
  - empty vs empty, empty vs some;
  - identical;
  - insert or delete at the start, middle and end;
  - a single changed line;
  - several hunks;
  - pairing of unequal removed/added runs into Changed + remainder;
  - ignore case (including non-ASCII, via `TextCase.h`), ignore trailing / all whitespace;
  - CRLF vs LF files compare equal (through `ForEachTextLine`);
  - Unicode lines;
  - a 200,000-line file with one change, finished well within a second thanks to the prefix/suffix trim;
  - the `TooLarge` cap and cancel;
  - `NextDifference` / `PreviousDifference`;
  - `ChangedSpans` (one word, the line cap);
  - `FirstDifferenceOffset` (equal, differ at 0, differ at the end, different lengths) on `TempDir` files;
  - `ChooseFilesToCompare` for every row of D1.
- **Existing test files:**
  - `KeyMapTests`: the `compare-files` default and its inventory entry;
  - `MenuBarTests`: "Compare contents" is real and simulates `Alt+F3`;
  - the Theme contrast test: the new attributes;
  - `ConfigTests`: `diffProgram`, if D5 is chosen.
- **Manual:**
  - two versions of a source file, in both views;
  - the options;
  - a UTF-16 file against a UTF-8 file;
  - a binary pair;
  - a 100 MB file (refused cleanly);
  - Esc during a big diff;
  - from the folder-compare results;
  - a narrow window;
  - the high-contrast theme.

## Phasing

1. `TextDiff` engine, `ChooseFilesToCompare` and `FirstDifferenceOffset` with tests (no UI).
2. `DiffViewer` screen, `DoCompareFiles`, the command / `Alt+F3` / menu item, the folder-compare results key,
   and the theme attributes.
3. The external tool (D5, if chosen), docs, F1 help, spec, version 1.27.0, changelog.

## Risks and notes

- **Performance on pathological inputs.** Myers is fast when the files are similar (the normal case) and slow
  when they are completely different. The edit cap, progress, Esc and the line/size limits keep it bounded.
  Linear-space Myers keeps memory proportional to the file sizes.
- **Memory.** Both files' lines are held in memory (capped at 64 MB each, plus the row table). The diff of two
  huge logs is out of scope; the byte comparison still answers "are they equal, and where do they first
  differ".
- **Encodings.** Each file is detected separately, so a UTF-16 and a UTF-8 copy of the same text compare
  **equal**, which is intended. The header shows when the encodings differ.
- **Not colour alone.** The marker column keeps the meaning visible in the high-contrast and basic-symbol
  modes (ACC-006).

## Open questions

1. **Key:** `Alt+F3` (free today, rebindable)? Or another key?
2. **Which files (D1):** two selected files in one panel → those; else the active file with the same name in
   the other panel; else the other panel's cursor file? Or something else?
3. **External diff tool (D5):** include the optional `diffProgram` setting (recommended)? Or keep it built-in
   only?
4. **Merging (copy a block to the other side and save):** leave it as a new LATER requirement SRC-008a
   (recommended)? Or include it now?

## Status

Planned 2026-10-09. **Implemented 2026-10-09** (1.27.0), all three phases.

Decisions:

- Open questions 1, 3 and 4 were decided as recommended: `Alt+F3`, the optional `diffProgram` setting, and merging
  as SRC-008a (LATER).
- Question 2 was decided by the user as: **two files selected in the active panel, or one file selected in each
  panel**. Otherwise the plan's fallback applies: the active panel's file and the same name in the other panel,
  else the other panel's cursor file.

Small differences from the plan:

- **Colours.** Four theme colours instead of three, because the changed-character highlight got its own
  (`attrDiffHighlight`, black on yellow) rather than reusing `attrSelected`, which has no background.
- **Shift+Alt+F3** is the "always built-in" variant, and is reserved against plugins as well.
- **Esc during the first comparison** closes quietly. During a recomputation (I/W) it keeps the previous result,
  with a note.
