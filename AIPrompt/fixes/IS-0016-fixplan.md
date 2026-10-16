# Fix Plan — IS-0016

Source: `AIPrompt/issue_fix_requests.md`

> Line editor feature:
>     This working well at down in the path editing, but it is required for everywhere where
>     filename, path is entered.

## Summary

IS-0015 Part 3 (UI-016, commit a8701b8) already routes **every** single-line editor through the
one shared `LineEdit`/`ApplyLineEditKey` primitive (`DialogInput.cpp:37-71`):

- the command line ("down in the path editing", `main.cpp`);
- `ShowTextPrompt` (`Dialog.cpp:204-254`), via `ApplyTextPromptKey`;
- `ShowFormDialog`'s Text fields, via `ApplyFormFieldKey`.

So the editing *logic* (Left/Right/Home/End/Backspace/Delete/insert) already works in every
filename/path dialog. What the user sees is different: **in dialogs the cursor is invisible**, so
the editor looks absent. Root cause, plus two smaller gaps found during the investigation:

1. **Invisible dialog cursor (root cause).**
   - `ShowTextPrompt` draws the cursor cell with `kAttrHeaderActive` (`Dialog.cpp:226`).
   - In both palettes (`Theme.h`), `attrHeaderActive` equals `attrBody`: white on dark blue.
   - The cursor cell is therefore indistinguishable from the dialog body around it.
   - The command line works only because its text is drawn with `kAttrCommandLine` (white on
     black), so the blue cursor cell stands out there.
2. **Plugin form text fields start with the cursor at 0.** `HostShowForm`
   (`PluginManager.cpp:1058-1061`) copies `textBuffer` into `field.text` but never sets
   `field.textCursor`. Typing therefore inserts at the start of a pre-filled value instead of
   appending.
3. **No horizontal scrolling.** `DrawBox` truncates a prompt line wider than the box
   (`LayOutBox` caps the content width at console width − 4). With a long path:
   - the tail of the text is cut off;
   - once the cursor moves past the box edge it isn't drawn at all (the
     `cursorX < geo.x + geo.width - 1` guard);
   - the user edits blind.

   The command line has the same limitation on a narrow terminal.

Affected entry points (every filename/path prompt goes through `ShowTextPrompt`):

- F2 rename and conflict "rename" in Copy/Move;
- F7 new folder and Shift+F7 new file;
- the F5/F6 destination;
- Ctrl+F quick filter;
- Numpad +/− select masks;
- Alt+F7 find;
- viewer search;
- plugin prompts via `HostShowTextPromptImpl`: ZIP "Pack to", "Unpack to" and "Create directory".

## Part 1 — Visible cursor in dialogs (the actual IS-0016 fix)

### Design

- **New theme attribute** `attrCursor` in `ThemePalette`, plus a live `kAttrCursor` global set by
  `ApplyThemePalette`.
  - It's a dedicated attribute rather than a reused one, because reusing `kAttrSelected` or
    `kAttrHeaderActive` couples the cursor to unrelated UI elements. That coupling is exactly what
    caused this bug.
  - Default theme: black on light gray/white, i.e. inverse video
    (`BACKGROUND_RED|GREEN|BLUE|INTENSITY`, black foreground). This is the conventional
    text-cursor look, and it contrasts with both the blue dialog body and the black command line.
  - High-contrast theme: the same inverse video, or black on bright yellow if that measures
    better.
- **Use `kAttrCursor` everywhere a line-editor cursor is drawn:**
  - `ShowTextPrompt` (`Dialog.cpp:226`);
  - `ShowFormDialog`'s focused Text field (`Dialog.cpp:315`, visible today only by accident, over
    the `kAttrSelected` row);
  - the command line (`main.cpp:701-713`), so all three editors share one look.
- **Optional, same root cause:** `ShowChoiceDialog`'s focused-option highlight (`Dialog.cpp:118`)
  also uses `kAttrHeaderActive` on a `kAttrBody` box, so it is invisible too. Switching it to
  `kAttrSelected` (yellow on black) fixes it with no new attribute. It's included here because
  it's the same one-line class of bug; drop it if you want the change scoped strictly to text
  entry.

### Files touched

- `Source/MyCommander/Theme.h`:
  - add the `attrCursor` field;
  - set its value in both palettes;
  - add the `kAttrCursor` global;
  - assign it in `ApplyThemePalette`.
- `Source/MyCommander/Dialog.cpp`: change the attribute at lines 226 and 315, and optionally 118.
- `Source/MyCommander/main.cpp`: change the attribute of the command-line cursor cell.
- Check whether plugin-visible theme exposure (`McPluginTypes.h` theme struct / `HostGetTheme`)
  mirrors palette fields one-for-one.
  - If it does, add `attrCursor` at the **end** of the struct (an ABI-additive minor bump).
  - If it doesn't, leave the SDK alone.

### Test plan

- `ColorContrastTests.cpp`:
  - `Theme_EveryFieldOfEveryNamedTheme_MeetsWcagAa...` already iterates palette fields. Extend it
    with `attrCursor` so the new attribute itself meets WCAG AA (ACC-002).
  - Add `CursorAttr_IsDistinguishableFromDialogBodyAndCommandLine_UI016`. It asserts that
    `attrCursor`'s background differs from both `attrBody`'s and `attrCommandLine`'s background, in
    every named theme. This is the regression guard for this exact bug.
- Manual check:
  - F2 rename, F7, F5 destination, Ctrl+F and the ZIP "Pack to" prompt all show a visible
    cursor that moves with Left/Right/Home/End;
  - the command line still shows its cursor;
  - repeat in both themes.

## Part 2 — Plugin form text field cursor starts at end

### Design

In `HostShowForm`'s `kMcFormFieldText` case (`PluginManager.cpp:1058-1061`), set
`field.textCursor = field.text.size();` after copying the text. This matches `ShowTextPrompt`,
which starts its cursor at the end of `initialText`.

### Files touched

- `Source/MyCommander/PluginManager.cpp` (one line).

### Test plan

- `HostShowForm` is real-Console code with no direct unit test today, and this is a one-line
  initialization. Verify by inspection.
- No ZIP form currently has a Text field (the `ShowForm` call at `ZipPlugin.cpp:421` is
  checkboxes/numbers/choices only), so the bug is latent rather than user-visible today.

## Part 3 — Horizontal scrolling for long text (dialogs + command line)

### Design

- **New pure helper in `DialogInput.cpp`** (TST-005 precedent):
  ```cpp
  // Leftmost text index to display so `cursor` stays within a `viewWidth`-column window,
  // given the previous offset (scrolls minimally, like a normal edit box).
  size_t ComputeLineEditViewOffset(const std::wstring& text, size_t cursor,
                                   size_t previousOffset, int viewWidth);
  ```
  - It measures in display columns via `TextWidth.h`, never code units (UI-011), so a CJK path
    scrolls correctly.
  - It reserves one column for the cursor at end-of-text.
- **Use it in `ShowTextPrompt`:**
  - keep a `viewOffset` across loop iterations;
  - draw `text.substr(viewOffset)` padded or truncated to the field width;
  - compute the cursor column relative to `viewOffset`, which removes the "cursor past the edge
    isn't drawn" case;
  - optionally draw a `‹`/`<` marker at the left edge when `viewOffset > 0` (ASCII `<` under
    ACC-006's plain-ASCII frame mode).
- **Use it in `ShowFormDialog`'s Text field** in the same way: its value column has a fixed width.
- **Use it on the main.cpp command line,** whose width is the console width minus the prompt
  prefix.

### Files touched

- `Source/MyCommander/Dialog.h`: declare `ComputeLineEditViewOffset`.
- `Source/MyCommander/DialogInput.cpp`: implement it.
- `Source/MyCommander/Dialog.cpp`: rendering in `ShowTextPrompt` and `ShowFormDialog`.
- `Source/MyCommander/main.cpp`: command-line rendering; keep a `commandLineViewOffset` next to
  `commandLineCursor`.

### Test plan

New cases in the existing dialog-input tests file (where `ApplyLineEditKey`'s tests live):

- short text: the offset stays 0;
- cursor at the end of a long text: the cursor sits on the last visible column;
- Home after scrolling right: the offset returns to 0;
- moving left within the visible window doesn't scroll (minimal scrolling);
- a full-width-character text: offset and cursor fall on column boundaries, never mid-glyph.

## Docs, spec, version

- `AIPrompt/my-commander.md`:
  - amend UI-016's description/Implemented note: a visible cursor in every line editor and
    horizontal scrolling for over-long text;
  - reference IS-0016;
  - if the spec tracks theme attributes under UI-010/ACC-002, mention the new cursor attribute.
- `CLAUDE.md`: no structural change, unless Part 3's helper warrants a mention in `Dialog.{h,cpp}`'s
  bullet; add one clause there.
- `README.md` / `Help.cpp`: no new shortcuts. Left/Right/Home/End/Del in prompts were already
  documented with IS-0015 Part 3; verify both still agree.
- `Version.h`: PATCH bump (bug fix, no new feature), e.g. 1.9.x → next patch.
- Changelog entry in `3_Reports/ChangeLogs/changelog_2026-10-05.md`.

## Risk / scope notes

- **Part 1 is low-risk:**
  - it touches attributes only, no logic;
  - adding a `ThemePalette` field is purely internal unless the plugin theme struct mirrors it,
    and in that case the field goes at the end (additive).
- **Part 2 is a one-line fix** for a latent bug.
- **Part 3 is the only real logic change.** It's isolated in a pure, tested helper, and the
  rendering changes are mechanical. It can be deferred if you want Parts 1+2 only: Part 1 alone
  resolves what the issue literally reports, which is that the editor isn't visibly working
  outside the command line.
- **Out of scope:**
  - selection/clipboard inside prompts (Shift+arrows, Ctrl+C/V);
  - word-wise movement (Ctrl+Left/Right);
  - Insert/overwrite toggle.

  Each would be a natural follow-up issue on the same `ApplyLineEditKey` primitive.

## Status

Implemented 2026-10-05 (all three parts, plus the optional `ShowChoiceDialog` focus fix), version
1.10.1. Deviations from the plan:

- The plugin SDK's theme snapshot was left unchanged: plugins never draw a host line editor, so
  exposing `attrCursor` would be ABI churn without a consumer.
- No left-edge `<` scroll marker: it would cover the first visible character and complicate the
  helper; a scrolled field already shows its text starting mid-string.
- Added to the command line: the path prompt is shortened to `...` plus its tail when it would
  take more than half the row, so a deep directory can't leave the typed command zero room.
