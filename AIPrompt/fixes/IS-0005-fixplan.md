# Fix Plan — IS-0005

Source: `AIPrompt/issue_fix_requests.md`

> Remove the behaviour, the app cna be closed by ESC!
> Modify the documentation too where it is required to remove this from the documentatio and
> tests too

## Summary

Today, pressing `Esc` at the top level (either main panel active, command line empty) quits the
application — the exact same effect as `F10`. This is a plain removal: after the fix, `F10` stays
the *only* way to quit; `Esc` with an empty command line becomes a no-op at the top level. `Esc`'s
*other* job — clearing a non-empty command line — is untouched; the issue only asks to remove the
quit behavior, not command-line clearing.

Everywhere else in the app `Esc` already means something else entirely (cancel a dialog, close a
picker/viewer/help screen, cancel an in-progress file operation) — none of that is "the app
closing," so none of it is affected. See **Scope boundary** below for the precise line between
what changes and what doesn't.

## Root cause / current behavior

`Source/MyCommander/main.cpp`, `wmain`'s own key-dispatch switch:

```cpp
case VK_ESCAPE:
    if (!commandLine.empty()) {
        commandLine.clear();
        historyIndex = -1;
    } else {
        running = false;   // <-- this is what IS-0005 asks to remove
    }
    break;
```

`running = false` is the loop-exit flag `wmain`'s own `while (running)` main loop checks; `F10`'s
own case (`case VK_F10: running = false; break;`) sets exactly the same flag — so currently `Esc`
and `F10` are fully interchangeable for quitting. After this fix they won't be: only `F10` sets
`running = false` from the top-level switch.

## Design

### Code change

Drop the `else` branch entirely — `Esc` with an empty command line does nothing:

```cpp
case VK_ESCAPE:
    if (!commandLine.empty()) {
        commandLine.clear();
        historyIndex = -1;
    }
    break;
```

One line removed (the `else { running = false; }`), nothing else in the switch changes. `F10`'s
own case is untouched.

### Scope boundary: what does *not* change

`Esc` already has several other, screen-local meanings elsewhere in `main.cpp` — none of these is
"quit the application," so none of them are touched:

- The drive picker (`DoSelectDrive`, Alt+F1/F2) and the search-results picker (`DoSearch`,
  Alt+F7) each have their *own* local `bool running` inside their own input loop, with their own
  `case VK_ESCAPE: case VK_F10: running = false;` — this exits *that screen*, back to the main
  panels, never the app itself. Same variable name, different scope, different effect entirely;
  not what the issue is describing.
- `ShowChoiceDialog`/`ShowTextPrompt` (`Dialog.cpp`) treat `Esc` as "cancel this dialog" (returns
  `-1`/`nullopt` to the caller, which then does nothing) — this is UI-009's own, deliberate,
  unrelated keyboard convention for dialogs.
- `PeekEscapePressed`/`ProgressUi::PollCancel` (`main.cpp`, PER-002) let `Esc` interrupt an
  in-progress copy/move/delete/attribute operation — a cancel, not a quit.
- The built-in viewer (F3) and the shortcut reference (F1) both close on `Esc` (among other
  keys) — closing that screen, returning to the main panels, not quitting the app.
- The too-small-terminal guard (UI-006) already only honors `F10` (`if (record.Event.KeyEvent.
  wVirtualKeyCode == VK_F10) running = false;` — `Esc` was never wired there) — no change needed.

## Files touched

- `Source/MyCommander/main.cpp` — the one-line removal above; also its own `NAV-008` comment
  ("captured only after an explicit, normal loop exit (F10/Esc)") gets reworded to say `F10` only,
  since that's now accurate.
- `Source/MyCommander/Help.cpp` — `Sections()`'s "Command line" group currently lists
  `{L"Esc", L"Clear the command line if it has text; otherwise quit"}`; becomes
  `{L"Esc", L"Clear the command line if it has text"}`. Per `CLAUDE.md`'s own note, `Help.cpp`'s
  content must mirror `README.md`'s keybindings table exactly — both are updated together, not
  just one.
- `README.md` — the keybindings table's row `| \`Esc\` | Clear non-empty command-line text;
  otherwise quit. |` becomes `| \`Esc\` | Clear non-empty command-line text. |`. No other `Esc`
  mention in the file describes quitting the app (they're all the screen-local cancels listed
  under Scope boundary above), so nothing else there needs to change.
- `AIPrompt/my-commander.md` — no row's `Implemented`/Disposition field changes (this behavior was
  never tracked under its own requirement ID — there is no "`Esc` quits" row in the spec to begin
  with, only a version-history paragraph documenting the removal, the same treatment every
  behavior-only fix in this series has gotten).
- Tests: none to change. Grepped `Source/MyCommanderTests/` for `VK_ESCAPE` — the only hit is
  `DialogInputTests.cpp`'s `ApplyTextPromptKey_Escape_Cancels`, which tests `ShowTextPrompt`'s
  own, unrelated "Esc cancels this dialog" behavior (see Scope boundary above) and is unaffected.
  `wmain`'s key-dispatch switch itself has never had test coverage (`main.cpp`'s established
  limit, noted throughout this fix series), so there was never a test asserting "Esc quits" to
  remove — the issue's "and tests too" turns out to be a no-op here, not an oversight.

## Test plan

No new or changed `TEST_CASE`s — there is nothing pure/`Console`-independent to extract here (this
is a one-line change to `wmain`'s own switch, which stays untested like the rest of that
function's key dispatch). Verification is: build clean, run the full existing suite to confirm no
regression (it can't regress anything this touches, since nothing tested it before), and a manual
check in the running app — `Esc` with an empty command line no longer quits, `F10` still does,
and `Esc` still clears non-empty command-line text — noted as still outstanding per this
environment's standing limitation (no interactive-console capture available).

## Risk / scope notes

- This removes a redundant quit path, not a capability — `F10` remains fully available and
  unchanged, so quitting the app is still always possible.
- No requirement ID's `Implemented`/Disposition field is expected to change — this behavior was
  never its own tracked spec row.
- Every other `Esc` meaning in the app (dialogs, pickers, viewer, help, operation-cancel) is
  explicitly out of scope and untouched — see **Scope boundary** above for the full list, so this
  doesn't get rediscovered as a surprise later.

## Status

Implemented, per the design above, with no deviations:

- The `VK_ESCAPE` case in `wmain`'s key-dispatch switch (`main.cpp`) no longer sets
  `running = false`; it only clears a non-empty command line, exactly as designed. `F10`'s own
  case is unchanged.
- `Help.cpp`'s shortcut-reference table and README's keybindings table were updated together,
  both dropping the "otherwise quit" wording.
- As predicted in the Files-touched section, there was nothing to remove from the test suite —
  the only `VK_ESCAPE` hit in `Source/MyCommanderTests/` was the unrelated
  `ApplyTextPromptKey_Escape_Cancels`, left untouched.
- See `3_Reports/ChangeLogs/changelog_2026-09-25.md`'s IS-0005 entry for the full file list and
  test counts.
