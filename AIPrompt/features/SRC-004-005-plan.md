# Implementation Plan — SRC-004 + SRC-005 (Content search: plain text, regular expressions, encodings)

Source: `AIPrompt/my-commander.md`:

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| SRC-004 | Content search shall support plain text. | P2 | LATER | Not Started |
| SRC-005 | Content search may support regular expressions and encoding options. | P2 | LATER | Not Started |

Both are also listed in §29 ("Content search and comparison — SRC-004 to SRC-008").

## Implementation check (2026-10-07)

There's no content search anywhere: nothing opens a file to look *inside* it.

| What exists | Where | Reusable for |
|---|---|---|
| `Alt+F7` recursive **name** search: a prompt, then a scrollable results list. Enter jumps there, F3 views. Esc cancels the walk. Results are capped at 5000 (`truncated`) | `Search.{h,cpp}` (`SearchFiles`), `main.cpp` `DoSearch` (SRC-001/002) | The walk (reparse points never descended, SEC-006; unreadable folders skipped), the cap, the results UI |
| Case-insensitive glob `WildcardMatch` | `PathUtil.h` | The file-name mask filter |
| A large-file text reader: bounded memory, BOM detection (UTF-8 / UTF-16LE), ANSI fallback per line | `TextFile.{h,cpp}` (VEE-004) | Decoding lines the same way the viewer does |
| The viewer's own `/` search (case-insensitive substring, `#hex` bytes) | `Viewer.cpp` | Opening a result at the matching line, with the term pre-loaded so `n` finds the next one |
| `ShowFormDialog`, Unicode case mapping (`LCMapStringEx`, in `MultiRename.cpp`) | `Dialog.h`, `MultiRename.cpp` | The options form; Unicode case-insensitive comparison |

## Summary

`Alt+F7` becomes **Find files**: one form that searches by **name** (as today) and, optionally, by **content**.
Every matching file is listed with its **first matching line**. `Enter` jumps there, as today. `F3` opens the
viewer **at that line**, with the search term ready for `n` (next match).

## Design

### D1 — The search form (replaces the single prompt)

| Field | Kind | Default | Notes |
|---|---|---|---|
| File name mask | Text | `*` | The same `WildcardMatch` as today, so plain `log` still means "contains log" |
| Containing text | Text | (empty) | Empty means a name-only search, exactly today's behavior. **SRC-004** |
| Regular expression | Checkbox | off | **SRC-005** |
| Match case | Checkbox | off | For plain text and regex |
| Encoding | Choice: **Auto** / UTF-8 / ANSI (system code page) / UTF-16LE / UTF-16BE | Auto | **SRC-005** |
| Skip binary files | Checkbox | on | See D4 |

- Values are remembered for the session; nothing is written to disk.
- With "Containing text" empty, the form behaves exactly like today's `Alt+F7`, so existing habits keep working.

### D2 — Matching (a new pure module `ContentSearch.{h,cpp}`)

```cpp
struct ContentQuery { std::wstring text; bool regex = false; bool matchCase = false; };
class ContentMatcher {                       // built once per search
public:
    static std::optional<ContentMatcher> Create(const ContentQuery&, std::wstring& error);  // bad regex -> error
    bool Matches(const std::wstring& line) const;
};
```

- **Plain text:** a substring match on each line.
  - Case-insensitive by default, via a **Unicode** lower-case mapping (`LCMapStringEx`, invariant locale), so
    `árvíztűrő` finds `ÁRVÍZTŰRŐ`.
  - The helper moves out of `MultiRename.cpp` into a small shared `TextCase.h` (`ToLowerInvariant` /
    `ToUpperInvariant`) used by both. A typed `#` has no special meaning here: the hex syntax belongs to the
    viewer only.
- **Regular expression:**
  - `std::wregex`, ECMAScript syntax. It's part of the C++ standard library, so not a third-party dependency
    (DEC-002 stays satisfied).
  - Case-insensitive uses `icase`, with the regex imbued with the **user's locale**, so accented letters match
    case-insensitively too.
  - An invalid pattern is reported **before** the walk starts, with the library's message, and the user returns
    to the form.
- **Line-based matching:** a match never spans a line break. That's simple and predictable, and the same as the
  viewer's search. This limitation is documented.
- **Long lines:** a line longer than 64 K characters is matched on its first 64 K only. This guards against
  pathological regex backtracking and memory use on minified or one-line files, and is documented.

### D3 — Reading file content (extends `TextFile`)

- **New streaming reader** in `TextFile.{h,cpp}`:

  ```cpp
  ForEachTextLine(path, TextEncodingChoice, callback(lineNumber, line) -> bool continue)
  ```

  - It's a single forward pass in fixed-size chunks, so **bounded memory regardless of file size**, as in VEE-004.
  - It shares the existing decoding rules: a BOM wins under Auto; otherwise UTF-8 with an ANSI fallback per line;
    or the explicitly chosen encoding.
  - `IndexTextFileForViewing` is unchanged.
- **`TextEncoding`** gains UTF-16BE, plus explicit UTF-8-only and ANSI-only modes for the *search* choice. The
  viewer's own auto-detection is unchanged.
- **The search stops reading a file at its first match**, which keeps it fast. The result records that line's
  number and text.

### D4 — Binary files

- **Detection:** with **Skip binary files** on (the default), a file is skipped as binary when its first 8 KB
  contain a NUL byte and it doesn't look like UTF-16 (no UTF-16 BOM, and the encoding choice isn't UTF-16).
  That's the usual heuristic, and it avoids "matches" in executables and images.
- **With the option off,** binary files are searched as ANSI text. That's useful for finding a string inside a
  DLL, the TC behavior.

### D5 — The walk, progress and cancel (extends `Search`)

```cpp
struct SearchOptions { std::wstring nameMask; std::optional<ContentMatcher> content; TextEncodingChoice encoding;
                       bool skipBinary = true; size_t maxResults = 5000; };
struct SearchResult  { std::filesystem::path fullPath; bool isDirectory; int lineNumber = 0; std::wstring lineText; };
SearchOutcome SearchFiles(const std::filesystem::path& root, const SearchOptions&, const SearchCancelPoll&,
                          const SearchProgress& onProgress = {});  // (filesSearched, currentPath)
```

- **Unchanged:** the existing name-only overload stays, delegating to this one. The SEC-006 reparse-point rule,
  skipping unreadable folders, and the 5000-result cap all still apply.
- **With content text set,** only **files** whose name matches the mask are opened; folders are walked but never
  listed as results.
- **Cancel:** Esc is polled per folder *and* every few thousand lines inside a big file, so cancelling is prompt
  even mid-file (PER-002).
- **Live progress line:** `Searching C:\src for "TODO"… 1 234 files, 17 found — current: …\big.log`, throttled to
  about 10 updates a second.
- **Unreadable files** (locked, permission denied) are counted and skipped. The results header mentions how many
  were skipped, so a missing match isn't a silent mystery.

### D6 — Results and opening them

- **Rows:** `path  :12:  …matching line text…`, with the line trimmed of leading whitespace and fitted to the
  width via `TextWidth.h` (UI-011).
- **Header:** `42 files containing "TODO" (3 unreadable skipped)`, and the truncation note when capped.
- **Keys:**
  - `Enter` jumps the panel to the file, with the cursor on it, as today (SRC-002);
  - **`F3`** opens the viewer **at the matching line**, with the term pre-loaded so `n` jumps to the next match.
    This is a small extension: `ShowTextFileViewer(console, file, ViewerStart{line, searchTerm})`, with the
    existing call unchanged.
- **Regex results in the viewer:** for a regex search, the viewer's own `n` stays plain-text, because the viewer
  has no regex mode. The pre-loaded term is then left empty and only the line is shown. This is documented.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/ContentSearch.{h,cpp}` (new, pure) | `ContentQuery`, `ContentMatcher` (plain/regex, case, invariant lower-casing), binary detection helper |
| `Source/MyCommander/TextCase.h` (new, header-only) | `ToLowerInvariant`/`ToUpperInvariant`, shared with `MultiRename.cpp` |
| `Source/MyCommander/TextFile.{h,cpp}` | `ForEachTextLine` streaming reader; encoding choice incl. UTF-16BE, forced UTF-8 / ANSI |
| `Source/MyCommander/Search.{h,cpp}` | `SearchOptions`, content-aware `SearchFiles` with progress; `SearchResult` line fields; unreadable count |
| `Source/MyCommander/Viewer.{h,cpp}` | Optional `ViewerStart{line, searchTerm}` |
| `Source/MyCommander/main.cpp` | `DoSearch`: form, live progress, the new result rows, F3 at the line |
| `Source/MyCommander/Strings.h`, `Help.cpp`, `README.md`, `USERGUIDE.md` | Texts and docs |
| `Source/MyCommanderTests/ContentSearchTests.cpp` (new), `SearchTests.cpp`, `TextFileTests.cpp` | Tests below |
| Both `.vcxproj` (+ filters) | New files |
| `AIPrompt/my-commander.md`, `CLAUDE.md`, `Version.h`, changelog | Spec rows LATER → IN-MVP / Implemented, §29 note, history; module bullet; 1.18 → 1.19 |

## Tests

**`ContentSearchTests.cpp`** (pure):

- **Plain matching:**
  - case-insensitive, including Unicode (`ÁRVÍZTŰRŐ` vs `árvíztűrő`);
  - Match case on/off;
  - empty text matches nothing, and the caller treats empty as name-only.
- **Regex:**
  - `TODO|FIXME`, `^\s*#include`, `\d{4}-\d{2}`;
  - `icase` with accented letters;
  - an invalid pattern (`(`, `[a-`) returns an error and no matcher.
- **Long lines:** a 100 K-character line matches within the first 64 K and doesn't hang or crash on `(a+)+b`.
- **Binary detection:**
  - a NUL in the first 8 KB means binary;
  - a UTF-16LE file with a BOM isn't binary;
  - plain text isn't binary.

**`TextFileTests.cpp`** (`TempDir`):

- **`ForEachTextLine` encodings:**
  - UTF-8 (with and without BOM);
  - UTF-16LE with BOM;
  - UTF-16BE (BOM and forced);
  - ANSI (forced): bytes decode by the system code page;
  - Auto picks the BOM.
- **Line numbers** are 1-based and correct across chunk boundaries (a file larger than the chunk size, with lines
  straddling it), and CRLF/LF are handled.
- **Early stop:** returning false from the callback stops reading.

**`SearchTests.cpp`** (`TempDir`):

- **Name-only:** behavior identical to before; the existing tests stay green.
- **Content found:**
  - the result has the right line number and text;
  - only the first match per file is reported;
  - folders are not results.
- **Mask and content combined:** `*.txt` with text skips a matching `.log`.
- **Binary handling:** a binary file is skipped by default and searched with the option off.
- **Unreadable files:** a locked or permission-denied file is counted, not fatal.
- **Cancel:** mid-walk cancel sets `cancelled`.
- **Cap:** the result cap sets `truncated`.
- **Reparse points:** a junction is still never descended (SEC-006); this is the existing test, now with content
  set.

**Manual:**

- `Alt+F7`, mask `*.cpp`, text `TODO`: results show `:line:` and the text; `F3` opens at the line and `n` finds
  the next one;
- regex `^\s*class ` with Match case;
- an invalid regex shows an error and returns to the form;
- a large log file can be cancelled with Esc mid-file;
- a UTF-16 file and an ANSI file are both found with Auto, and the explicit encodings change the result as
  expected.

## Phasing

1. **Phase 1:**
   - `TextCase.h` (and `MultiRename.cpp` switched to it, with its tests kept green);
   - `ContentSearch` matcher and binary detection, with tests.
2. **Phase 2:**
   - `TextFile::ForEachTextLine` and the encoding choices (tests);
   - `Search` content-aware walk, progress and unreadable count (tests).
3. **Phase 3:**
   - UI: the form, progress line, result rows, viewer start-at-line;
   - docs, spec, version, changelog.

## Risks and notes

- **Regex performance:** `std::wregex` is slow and backtracks. The 64 K line cap and Esc polling inside files keep
  a bad pattern from freezing the app. Plain text, the default, is fast.
- **Name-only searches are unchanged:** only the prompt becomes a form, whose first field takes the same input.
  It's one extra `Enter` press compared to today; the form remembers the last values.
- **Encoding guesses:** Auto can't tell ANSI from UTF-8 in every case without a BOM, the same per-line
  UTF-8-then-ANSI rule as the viewer. The explicit choices cover the rest.
- **Out of scope:**
  - matches spanning line breaks;
  - search and replace inside files;
  - searching inside ZIP archives (plugin content);
  - date and size filters;
  - "find duplicates";
  - feeding results to a panel as a list.

  These are natural follow-ups; SRC-006..008 stay as they are.

## Open questions

1. **Key and form:** keep `Alt+F7` and replace its single prompt with the form (planned), or leave `Alt+F7`
   name-only and give content search its own key?
2. **Results:** the **first** matching line per file (planned, fast), or *every* matching line, one row each?
3. **Binary files:** skip by default (planned)?
4. **Encodings:** Auto / UTF-8 / ANSI / UTF-16LE / UTF-16BE enough, or also a specific code page (e.g. Central
   European 1250)?

## Status

Implemented 2026-10-07, all three phases (version 1.19.0), with the defaults for the open questions:
`Alt+F7` replaced by the form; the first matching line per file; binary files skipped by default; Auto /
UTF-8 / ANSI / UTF-16LE / UTF-16BE.

Changes made while implementing, from measurement:

- **The regex runaway risk was real and is now bounded.** The first test of `(a+)+b` on a 29-character
  line ran for over 30 seconds, and a quadratic `.*ID` on a 60 K line looped for minutes. Esc can't
  interrupt one `std::regex_search` call. Now:
  - a regex looks at the first **1 024** characters of a line (plain text keeps 65 536);
  - MSVC's `_REGEX_MAX_COMPLEXITY_COUNT` is lowered to **200 000**, defined in `ContentSearch.cpp`, the
    only translation unit that includes `<regex>`; the header hides the regex type behind a small opaque
    struct;
  - measured in a Debug build: the pathological pattern gives up in about 2 s and the quadratic one takes
    about 0.8 s. Release is much faster;
  - Esc is checked after every line during a regex search (every 4 096 lines for plain text).
- **Auto encoding without a BOM** falls back to the **system ANSI code page** per line, so
  Hungarian/Central European ANSI files read correctly. The viewer's own fallback still widens bytes as
  Latin-1; it is unchanged here.
- **The viewer opens with the matching line at the top,** not with context above it, so `n` moves on to
  the *next* match. A term starting with `#` isn't pre-loaded, because the viewer would treat it as a hex
  search.
- **Progress shows files searched and the current path.** It has no "found" count; that appears in the
  results title.
