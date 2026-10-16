# Implementation Plan — NAV-018 (path autocomplete in the command line)

Source: a new requirement, proposed here. Builds on the command line's path-navigation rule (NAV-003,
`ResolveExistingDirectory`) and its editor (`LineEdit`/`ApplyLineEditKey`, UI-016).

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| NAV-018 (proposed) | The command line shall complete a typed folder or file path from the entries that exist, extending the text to the common prefix and cycling through the candidates on repeated presses. | P3 | IN-MVP | Not Started |

## Summary

Typing `C:\Users\att` and then the rest of a long path is slow and error-prone. Shells complete paths on a key; the
command line should too.

- **Key:** `Ctrl+Space` completes. **Plain `Tab` keeps its role** (it switches panels, as today), so the command line does
  not change what Tab does. `Ctrl+Space` is checked free in the key table. Open question 1 covers the IME caveat.
- **Behaviour:**
  - the path being typed is split into its folder part and its name part (the text after the last `\` or `/`);
  - the matching entries of that folder whose names start with the name part (case-insensitive) are the candidates;
  - one candidate: the text is completed to it, plus a `\` if it is a folder;
  - several candidates: the text is extended to their longest common prefix; a second press shows the candidates in a
    one-line list in the hint row, and further presses cycle through them.
- **Scope:** paths only. Command names are not completed (the command line runs `cmd` commands; a command list is a
  separate, larger question).

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| Path resolution of a typed folder | `PathUtil.h` `ResolveExistingDirectory` (NAV-003) | Finding the folder part |
| Line editor with cursor and insertion | `Dialog.h` `LineEdit` / `ApplyLineEditKey` (UI-016) | Replacing the typed text |
| The command line's text and cursor state | `main.cpp` `commandLine`, `commandLineCursor` (IS-0015) | Where the completion lands |
| Listing a folder | `FindFirstFileExW` / `Panel` listing | Candidate names |
| Case-insensitive comparison | `CompareStringOrdinal`, `TextCase.h` | Matching |

## Design

### D1 — Candidate computation (pure, `PathCompletion.{h,cpp}`)

- `SplitTypedPath(text)` → (folder part, name part). The folder part is resolved with `ResolveExistingDirectory`
  relative to the active panel (so a relative typed name works); a folder that does not exist yields no candidates.
- `CandidatesFor(folder entries, namePart)`: names starting with `namePart`, case-insensitive, folders marked, sorted
  case-insensitively; hidden and system entries included only when the typed name starts with a dot or the panel shows
  hidden entries (default off, matching the panel).
- `CommonPrefix(candidates)`: the longest common prefix, compared case-insensitively but keeping the spelling of the first
  candidate.
- `NextCandidate(index, count)`: the cycle for repeated presses.
- All pure and tested without a console.

### D2 — The command line (`main.cpp`)

- `Ctrl+Space` (new command `CompletePath`, `complete-path`): computes the candidates from the current text; applies
  one of the three outcomes (single, common prefix, cycling). State kept between presses: the candidate list and the
  index, cleared by any other key.
- The hint row shows the candidate list (truncated with `…`, width-aware via `TextWidth.h`) while cycling.
- With an empty command line, `Ctrl+Space` does nothing.

### D3 — Limits

- Provider panels (archives, servers): no completion in v1; the command line is for local paths. Stated in the help.
- Very large folders: the listing is done once per press, with the same bounded enumeration the panel uses; a folder of
  100 000 entries is listed by the panel anyway, so the cost is the existing one.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/PathCompletion.{h,cpp}` (new) | `SplitTypedPath`, `CandidatesFor`, `CommonPrefix`, cycle helper |
| `Source/MyCommander/main.cpp` | `Ctrl+Space` handling, candidate state, hint row list |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp`, `HintBar.h`, `Help.cpp` | Command, key, hint, help row |
| `Source/MyCommanderTests/PathCompletionTests.cpp` (new), `KeyMapTests.cpp` | Tests below |
| Both `.vcxproj` files | New source and test files |

## Tests

- **`SplitTypedPath`:** `C:\a\b` → (`C:\a\`, `b`); `C:\a\` → (`C:\a\`, ``); `b` (relative) → (active folder, `b`);
  quoted paths; `..` segments; a folder part that does not exist.
- **`CandidatesFor`:** prefix match, case-insensitive; folders marked; hidden excluded by default and included when the
  typed name starts with a dot; order.
- **`CommonPrefix`:** `Document` and `docs` → `Do` (case rule); one candidate → itself; none → empty.
- **Cycling:** wraps around; a count of 1 stays.
- **Manual:** type `C:\Prog`, press `Ctrl+Space` (extends to `C:\Program Files`), press again (lists `Program Files` and
  `ProgramData`), cycle; a relative name; a non-existent folder (no change).

## Documentation updates

- `AIPrompt/my-commander.md`: new row **NAV-018**; version bump; history paragraph; NAV-003's note that completion exists.
- `README.md`: keybindings row (`Ctrl+Space`); a "Path completion (NAV-018)" paragraph in the command-line section, with
  the note that `Tab` still switches panels.
- `USERGUIDE.md`: keybindings row and the same paragraph.
- `Source/MyCommander/HintBar.h`: the command-line hint text gains `Ctrl+Space complete` when the command line has text.
- `Source/MyCommander/Help.cpp`: F1 row.
- `CLAUDE.md`: `PathCompletion` in the source layout; Testing list.
- `AIPrompt/ManualTests.md`: new MT entry.
- `AIPrompt/features/NAV-018-plan.md`: status and differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.

## Phasing

1. `PathCompletion` and its tests (pure). 2. Command-line integration and candidate state. 3. Hint, help, docs.

## Risks

- **IME conflict:** `Ctrl+Space` toggles the input method on some Windows keyboard layouts (Japanese, Chinese, and some
  IMEs). If it does on the user's layout, the key never reaches the app. Open question 1; the command is rebindable
  through `keys.conf`, so the user can change it.
- **Surprise completions:** a unique match is applied immediately; a backslash is added only to folders, so a file name
  is never followed by one.

## Open questions

1. **Key:** `Ctrl+Space` (recommended, checked free), or another key chosen with the user's layout in mind (for example
   `Ctrl+Enter`-style chords are taken). Confirm on the user's keyboard.
2. **Hidden entries:** excluded by default (recommended).

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.48.0, version 2.12.0): `PathCompletion.{h,cpp}`,
`PathCompletionTests.cpp`, `Ctrl+Space` (`complete-path`), Commands > Complete path, MT-065.

Open questions: key `Ctrl+Space` (rebindable; IME caveat documented); hidden entries excluded by default.

Differences from the plan:

- The candidates are listed on the status row (`gStatusOverride`), not the hint row, so the hint bar stays intact; the
  hint row gained a clickable `Ctrl+Space Complete` segment.
- A press that cannot extend the common prefix starts cycling at once (it shows the list and the first candidate).
- Completion works only with the cursor at the end of the line, and the directory part is resolved with
  `ResolveExistingDirectory` (so quotes and `..` work).
- A Commands > Complete path menu item was added, as for every built-in shortcut.
