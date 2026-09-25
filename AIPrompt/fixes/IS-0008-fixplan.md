# Fix Plan — IS-0008

Source: `AIPrompt/issue_fix_requests.md`

> On the help view the mouse does not scroll the view. It shall be fixed. Mouse scroll is needed.

## Summary

The F1 shortcut reference (`Help.cpp`'s `ShowShortcutReference`, KEY-004) is a full-screen,
scrollable, read-only list — but it only scrolls from the keyboard (`Up`/`Down`/`Page Up`/
`Page Down`/`Home`/`End`). Mouse wheel input already reaches it (the console's input mode enables
`ENABLE_MOUSE_INPUT` globally, `Console.cpp`), but its own input loop discards every `MOUSE_EVENT`
unconditionally, so a wheel notch over the help screen has no effect at all.

The fix: handle `MOUSE_EVENT`/`MOUSE_WHEELED` in `ShowShortcutReference`'s own input loop, scrolling
`topLine` by the same "3 rows per notch" granularity `main.cpp`'s dual-pane wheel handling already
uses (IS-0003/UI-012), reusing the existing `clampTop`/`paint` machinery already there for every
other scroll action — no new scrolling logic, just a new input source driving the same `topLine`
state the keyboard already drives.

## Root cause / current behavior

`Source/MyCommander/Help.cpp`, `ShowShortcutReference`'s input loop (~line 256):

```cpp
while (running) {
    INPUT_RECORD record;
    DWORD read = 0;
    if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;

    if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
        console.UpdateSize();
        clampTop();
        paint();
        continue;
    }
    if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;
    switch (record.Event.KeyEvent.wVirtualKeyCode) { ... }
}
```

The `if (record.EventType != KEY_EVENT ...) continue;` line silently drops every `MOUSE_EVENT`
(wheel, click, move, drag) — there's no mouse handling of any kind in this function today.
`Console.cpp` already turns on `ENABLE_MOUSE_INPUT` unconditionally for the whole app (UI-012's own
comment there), so the event genuinely arrives at this loop; it's just never inspected.

This is a narrower version of the same shape `main.cpp`'s own dual-pane wheel scrolling already
solved (IS-0003, `HandleMouseEvent`, ~line 1763):

```cpp
if (mouse.dwEventFlags == MOUSE_WHEELED) {
    short wheelDelta = static_cast<short>(HIWORD(mouse.dwButtonState));
    int steps = (wheelDelta > 0) ? -3 : 3;
    Panel& target = leftActive ? left : right;
    target.MoveCursor(steps, ComputeVisibleRows(console, hintContext, hasStatusLine));
    return;
}
```

That existing block's own comment explicitly scopes wheel handling to the dual-pane screen only —
"dialogs..., the drive/search pickers, the viewer, and the help screen stay keyboard-only" — so
this plan is deliberately narrowing that comment for the one screen the issue actually names, not
contradicting a decision that was never meant to be permanent for every screen.

## Design

Add a `MOUSE_EVENT` branch to `ShowShortcutReference`'s input loop, sitting alongside the existing
`WINDOW_BUFFER_SIZE_EVENT` branch and before the `record.EventType != KEY_EVENT` filter (which
must stay — it's still correct for filtering out everything that isn't a key press *after* the
mouse branch has already had its turn):

```cpp
if (record.EventType == MOUSE_EVENT && record.Event.MouseEvent.dwEventFlags == MOUSE_WHEELED) {
    short wheelDelta = static_cast<short>(HIWORD(record.Event.MouseEvent.dwButtonState));
    topLine += (wheelDelta > 0) ? -3 : 3;
    clampTop();
    paint();
    continue;
}
```

- Same sign convention and 3-row granularity as `main.cpp`'s existing wheel handling (positive
  `wheelDelta` = scrolled toward the user = up = decrease `topLine`), so the two screens feel
  identical to use — no new constant, no new tuning decision.
- Reuses `clampTop`/`paint` exactly as every keyboard scroll case already does; `topLine` going
  negative or past the end before `clampTop()` runs is already the established, safe pattern here
  (`VK_UP`/`VK_DOWN` do the same unclamped-then-clamp sequence).
- Mouse position (`dwMousePosition`) is irrelevant to a wheel event and isn't read — this is a
  content-scroll action, not a click, so (unlike `main.cpp`'s hint-bar/panel click handling) there's
  no row/column hit-testing to get right.
- Every other `MOUSE_EVENT` variant (click, move, drag, button state without `MOUSE_WHEELED`) falls
  through unhandled, same as today — the issue asks specifically for scroll, not click-to-select or
  drag, and this screen is read-only content with nothing a click could meaningfully act on.

### Doc comment update

`Help.h`'s doc comment ("Up/Down/Page Up/Page Down/Home/End scroll... Blocks until the user
presses Esc, F1, or F10") gets one clause added noting the mouse wheel also scrolls, so the header
stays an accurate description of the function's actual input surface.

## Files touched

- `Source/MyCommander/Help.h` — doc comment only (mention wheel scrolling).
- `Source/MyCommander/Help.cpp` — `ShowShortcutReference`'s input loop (~line 256): one new
  `MOUSE_EVENT`/`MOUSE_WHEELED` branch, per **Design** above. No other function in this file
  changes; `Sections`/`BuildRows`/`Metadata`/`ReadFileVersionMetadata` (the recent KEY-004 About
  section) are unaffected.
- `README.md` — the "Help" bullet and/or the "Shortcut reference (`F1`)" row of the
  screen-specific-keys table (`## Other screens' keys` or equivalent) should note wheel scrolling
  once implemented, matching how every other screen's mouse support is documented there.
- `AIPrompt/my-commander.md` — a version-history paragraph; `KEY-004`'s `Implemented` status is
  unaffected (this extends the screen it already covers, not a new requirement).

No `Panel`/`Console`/other pure-module file needs to change — `Console.cpp` already enables
`ENABLE_MOUSE_INPUT` for the whole app, so no mode/flag change is needed there either.

## Test plan

`ShowShortcutReference`'s input loop is `main.cpp`/`Help.cpp`-local key/mouse-dispatch glue, not a
pure function — consistent with this codebase's established limit (`main.cpp`'s own
`HandleMouseEvent`/wheel handling has never had a `TEST_CASE` either, per every prior `IS-000N`
fix plan's own test-plan section), so no new automated test is expected for the wheel branch
itself. Live verification once implemented: open the help screen (`F1`), scroll the wheel down and
confirm `topLine` advances by 3 rows per notch and stops cleanly at the last page (doesn't overshoot
past `End`'s own position), scroll up and confirm it stops cleanly at the top (doesn't go negative
past `Home`'s own position), confirm a resize immediately after a wheel scroll still re-clamps
correctly (shared `clampTop()` call, so this should fall out for free), confirm keyboard scrolling
still works unchanged afterward.

As with prior `IS-000N` live-verification notes this session, this environment's console app has no
traditional HWND under Windows Terminal/ConPTY (confirmed during the KEY-004 About-section work),
so window-handle-based input automation for an unattended screenshot isn't available — verification
will be manual/interactive.

## Risk / scope notes

- **Scope is exactly what the issue names: the help view (`F1`) only.** The identical limitation
  exists in `Viewer.cpp` (`ShowTextFile`'s own `ReadConsoleInputW` loop has the exact same
  `record.EventType != KEY_EVENT` filter, no mouse handling at all) and in the drive/search-results
  pickers — `main.cpp`'s own IS-0007-era comment already lists all of these as deliberately
  keyboard-only. None of those are touched by this plan; if wheel scrolling is wanted there too,
  that's a separate, explicit ask (the pattern would be near-identical to copy, but a different
  file/function each time, not a shared helper — flagging this rather than preemptively
  generalizing into one, since nothing asked for that yet).
- **No new state, no new risk to `DrawFrame`/`ComputeVisibleRows`'s reserved-row agreement.** Unlike
  the IS-0007/IS-0007-addendum fixes, this doesn't touch `main.cpp`'s dual-pane rendering at all —
  it's entirely local to `Help.cpp`'s own already-self-contained `topLine`/`clampTop`/`paint`
  triple, so there's no cross-function row-math-agreement class of bug this could introduce.
- **No `Implemented`/Disposition change to any requirement row.** `KEY-004` (in-application
  shortcut reference) is already `Implemented`; this refines its input handling, not its existence.
- **Click-to-anything on this screen stays explicitly out of scope**, since the issue only asks
  for scroll and the screen has no per-row action a click could trigger (unlike the hint bar, which
  invokes a command, this screen's rows are read-only reference text).

## Status

**Implemented**, per the design above, with one note:

- Live pixel-level verification (scroll the wheel in a running app, confirm the view moves) could
  not be performed automatically in this environment: this session's console app runs hosted under
  Windows Terminal/ConPTY, which gives the process no traditional HWND (`Get-Process`'s
  `MainWindowHandle` returns 0, confirmed during the preceding KEY-004 About-section work), so
  window-handle-based input automation isn't reachable here to inject a synthetic wheel event or
  capture a screenshot. Verified instead by direct comparison against `main.cpp`'s own wheel-scroll
  block (IS-0003), which uses the identical `MOUSE_WHEELED`/`HIWORD(dwButtonState)` pattern and has
  already been exercised live in this app; a clean build and the full test suite (329 cases, 11,508
  checks, 0 failed) confirm no regression elsewhere.

No deviation from the design otherwise: `Help.h`'s doc comment and `Help.cpp`'s input loop gained
exactly the one new `MOUSE_EVENT`/`MOUSE_WHEELED` branch described above; `Viewer.cpp` and the
drive/search-results pickers were left untouched, as scoped. See
`3_Reports/ChangeLogs/changelog_2026-09-25.md`'s IS-0008 entry for the full file list.
