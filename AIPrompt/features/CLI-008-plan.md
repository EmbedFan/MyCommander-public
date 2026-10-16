# Implementation Plan — CLI-008 (reverse search in command history)

Source: a new requirement, proposed here. Extends CLI-004 (command history, Ctrl+Up/Ctrl+Down). Shell users know it as
Ctrl+R; here Ctrl+R is already the panel refresh (IS-0012), so the reverse search uses `Alt+R`, checked free.

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| CLI-008 (proposed) | The command line shall search its history incrementally: typed text narrows the list of past commands, and the chosen command is put back on the command line. | P3 | IN-MVP | Not Started |

## Summary

Ctrl+Up/Down walks the history one entry at a time. With a long history, finding "the `robocopy` line from last week"
takes many presses. Reverse search finds it by a fragment.

- **Key:** `Alt+R` opens the search inside the command line (checked free; the `Alt` letters used by menus are only
  F, S, C, N, V and E).
- **While searching:** the command line shows `(reverse-search) text_` and the most recent history entry containing the
  typed text. Typing narrows; `Alt+R` again steps to the next older match; `Backspace` removes a character.
- **Finish:**
  - `Enter` puts the matched command on the command line and leaves it there (not run; the user can edit it, then press
    `Enter` again to run, as with Ctrl+Up).
  - `Esc` cancels and restores the command line as it was before the search.
- **Matching:** case-insensitive substring, newest first, over the same in-memory history the Ctrl+Up/Down recall uses
  (CLI-004); the CLI-007 "leading space, never in history" rule is unchanged.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| Command history list (newest-last, capped at 100) | `CommandHistory.{h,cpp}`, `main.cpp` `commandHistory` | The search corpus |
| Recall state and the command line text/cursor | `main.cpp` `historyIndex`, `commandLine`, `commandLineCursor` | Where the result lands |
| Single-line editing for the search text | `Dialog.h` `LineEdit` / `ApplyLineEditKey` (UI-016) | The search text editor |
| Hint row | `HintBar.h` | The `(reverse-search)` prompt and hints |

## Design

### D1 — Matching (pure, `HistorySearch.{h,cpp}`)

- `FindMatch(history, text, startIndex)`: the newest entry (scanning from `startIndex` backward) whose text contains
  `text`, case-insensitively, using `TextCase.h`'s invariant folding (so a Hungarian `Ő` matches `ő`).
- Empty text matches the newest entry (the search opens showing the last command).
- Returns the index, or none. Stepping with `Alt+R` again is `FindMatch(history, text, index - 1)`.

### D2 — Interaction (`main.cpp`)

- A small state object: active flag, search text (a `LineEdit`), current match index, the saved command line and cursor
  for `Esc`.
- While active, key handling is routed to the search before the normal command-line handling: printable keys extend the
  text, `Backspace` shortens it, `Alt+R` steps, `Enter` accepts, `Esc` cancels. Other keys cancel the search and are then
  handled normally (so arrows and F-keys keep working as before).
- The panel and the menu are not affected; the search lives only on the command line row.

### D3 — Limits

- Only the in-memory history the session has (which, with `persistCommandHistory`, includes the saved one).
- The search never runs a command and never reads anything from disk while searching.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/HistorySearch.{h,cpp}` (new) | `FindMatch` (pure) |
| `Source/MyCommander/main.cpp` | Search state, key routing, `(reverse-search)` line |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp`, `HintBar.h`, `Help.cpp` | Command `ReverseSearch` (`Alt+R`), hint, help row |
| `Source/MyCommanderTests/HistorySearchTests.cpp` (new), `KeyMapTests.cpp` | Tests below |
| Both `.vcxproj` files | New source and test files |

## Tests

- **`FindMatch` (pure):** newest-first order; case-insensitive match; Unicode case folding (`Ő`/`ő`); empty text returns
  the newest entry; stepping past the oldest returns none; an entry starting with a space (never stored, CLI-007) is not
  a match because it is not in the list.
- **Key:** `Alt+R` → `ReverseSearch` in the default table and the inventory.
- **Manual:** run three commands, `Alt+R`, type a fragment, step, accept with Enter, confirm it is on the line and not
  run; `Esc` restores the line; a fragment with no match shows `failing` in the prompt.

## Documentation updates

- `AIPrompt/my-commander.md`: new row **CLI-008**; version bump; history paragraph; CLI-004's note about recall methods.
- `README.md`: keybindings row (`Alt+R`); a "Reverse history search (CLI-008)" paragraph in the command-line section.
- `USERGUIDE.md`: keybindings row and the same paragraph.
- `Source/MyCommander/HintBar.h`: the reverse-search prompt and its hint line.
- `Source/MyCommander/Help.cpp`: F1 row.
- `CLAUDE.md`: `HistorySearch` in the source layout; Testing list.
- `AIPrompt/ManualTests.md`: new MT entry.
- `AIPrompt/features/CLI-008-plan.md`: status and differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.

## Phasing

1. `FindMatch` and tests (pure). 2. Search state and key routing. 3. Hint, help, docs.

## Risks

- **Key routing mistakes:** the search swallows keys while active. The rule "any key that is not part of the search
  cancels it and then acts normally" keeps the rest of the app reachable; a test covers the routing decision as a pure
  function.
- **Accidental run:** accepting never runs the command; the user presses `Enter` again.

## Open questions

1. **Accept behaviour:** place on the line, not run (recommended, matches shell reverse search).
2. **Key:** `Alt+R` (recommended, checked free).

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.45.0, version 2.9.0): `HistorySearch.{h,cpp}`,
`HistorySearchTests.cpp`, `Alt+R`, Commands > History search, MT-062.

Open questions: accept places the command on the line without running it; key `Alt+R`.

Differences from the plan:

- No new `HintBar.h` context: while the search is open the status row carries its hints (`gStatusOverride`).
- A `Commands > History search` menu item (mnemonic I) was added, as every built-in shortcut has one.
- After `Enter` the history position is set to the matched entry, so `Ctrl+Up`/`Ctrl+Down` continue from it.
- The search does not open while a plugin panel is installed or when the history is empty.
