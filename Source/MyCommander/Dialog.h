#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Console.h"
#include "Navigation.h"

namespace mc {

// A single labeled choice in a ShowChoiceDialog, triggered by its hotkey
// (case-insensitive). Dialogs are keyboard-only throughout (UI-009) apart
// from IS-0003's mouse click on an option, an additive alternative to the
// hotkey, not a replacement for it.
struct DialogOption {
    wchar_t hotkey;
    std::wstring label;
};

// TST-005: the case-insensitive hotkey match ShowChoiceDialog's own input
// loop uses, pulled out as a pure function so it's directly unit-testable
// without a real console/input loop. Returns the matching option's index,
// or -1 if `ch` (a 0 UnicodeChar, e.g. a bare modifier or function key,
// included) matches none of `options`.
int MatchDialogHotkey(wchar_t ch, const std::vector<DialogOption>& options);

// IS-0003: the on-screen column span of each of `options`' rendered
// "[X]Label" segments within ShowChoiceDialog's own left-aligned options
// line -- pulled out as a pure function (TST-005 precedent) so the layout
// math is testable without a real Console. `row`/`startCol` are the
// caller's own box-geometry values (box top-left + the border/padding
// ShowChoiceDialog's DrawBox call already applies), recomputed by the
// caller since neither the box geometry nor the options line are exposed
// outside Dialog.cpp today.
std::vector<ClickableRegion> BuildOptionRegions(const std::vector<DialogOption>& options, SHORT row, SHORT startCol);

// Draws a centered modal box with `title` and `lines` of body text, then
// waits for the user to press one of `options`' hotkeys. `redrawBackground`
// repaints the screen behind the box (called once up front and again after
// any resize while the dialog is open). Returns the chosen option's index,
// or -1 if the user pressed Esc.
int ShowChoiceDialog(Console& console, const std::wstring& title, const std::vector<std::wstring>& lines,
                     const std::vector<DialogOption>& options, const std::function<void()>& redrawBackground);

// Draws a centered modal message box dismissed by any keypress.
void ShowMessage(Console& console, const std::wstring& title, const std::vector<std::wstring>& lines,
                 const std::function<void()>& redrawBackground);

// IS-0015 Part 3: a positionable cursor over a single line of text --
// the one shared primitive behind ApplyTextPromptKey, ApplyFormFieldKey's
// Text case, and main.cpp's command line, replacing what used to be three
// independently-reimplemented "append at the end, Backspace removes the
// last character only" editors with no cursor concept at all.
struct LineEdit {
    std::wstring text;
    size_t cursor = 0;  // 0..text.size()
};

// Applies one keypress to `edit` in place. Printable character (>= 0x20):
// inserted at `cursor`, which then advances (capped at 240 characters
// total, the same limit every one of the three call sites already
// enforced independently). VK_BACK: erases the character *before* `cursor`
// and moves left; a no-op at position 0. VK_DELETE: erases the character
// *at* `cursor` (forward delete); a no-op at the end. VK_LEFT/VK_RIGHT:
// moves `cursor` by one, clamped to [0, text.size()]. VK_HOME/VK_END:
// jumps to the start/end. Returns true for any of the above -- including a
// recognized-but-no-op case like Home already at position 0 -- so a caller
// can tell "this was a line-editing key" apart from "not recognized at
// all" (Enter/Esc/Tab/arrow-key-elsewhere/...), which it interprets itself;
// false for anything this function doesn't handle.
bool ApplyLineEditKey(LineEdit& edit, WORD virtualKeyCode, wchar_t unicodeChar);

// IS-0016: horizontal scrolling for a single-line editor whose text is
// wider than the `viewWidth` display columns it's drawn into (a long path in
// a text prompt, a form Text field, the command line). Returns the index of
// the first character of `text` to display so the cursor -- the character
// at `cursor`, or one blank cell at end-of-text -- is always on screen.
// Scrolls minimally from `previousOffset` (the value this returned last
// time; 0 initially), the way an ordinary edit box does: moving the cursor
// within the visible window never scrolls; moving it past either edge
// scrolls just far enough; text that fits entirely always shows from 0; and
// after text shrinks, the window slides back left to fill any blank space
// on the right. Measured in display columns (UI-011), not code units.
size_t ComputeLineEditViewOffset(const std::wstring& text, size_t cursor, size_t previousOffset,
                                 size_t viewWidth);

// TST-005: what a single key event does to a text prompt's in-progress
// text/cursor, pulled out of ShowTextPrompt's own input loop as a pure
// function.
enum class TextPromptOutcome { StillEditing, Submitted, Cancelled };
struct TextPromptKeyResult {
    TextPromptOutcome outcome = TextPromptOutcome::StillEditing;
    std::wstring text;    // the (possibly unchanged) text; meaningful for StillEditing/Submitted
    size_t cursor = 0;    // IS-0015 Part 3: the (possibly unchanged) cursor position within `text`
};

// Esc cancels; Enter submits `currentText` unchanged; every other key is
// delegated to ApplyLineEditKey (IS-0015 Part 3) against a LineEdit built
// from `currentText`/`currentCursor`.
TextPromptKeyResult ApplyTextPromptKey(const std::wstring& currentText, size_t currentCursor, WORD virtualKeyCode,
                                       wchar_t unicodeChar);

// Draws a centered modal single-line text prompt pre-filled with
// `initialText`, with a real positionable cursor (IS-0015 Part 3). Returns
// the edited text, or std::nullopt if the user pressed Esc.
std::optional<std::wstring> ShowTextPrompt(Console& console, const std::wstring& title,
                                           const std::wstring& initialText,
                                           const std::function<void()>& redrawBackground);

// FEA-0007, §1: one field in a ShowFormDialog -- a generic, reusable
// multi-field options-dialog primitive (not ZIP-specific), since
// ShowChoiceDialog/ShowTextPrompt each handle exactly one concern and some
// callers (a packing dialog, say) need several fields in one modal.
enum class FormFieldKind { Checkbox, Number, Choice, Text };

struct FormField {
    FormFieldKind kind = FormFieldKind::Checkbox;
    std::wstring label;

    bool boolValue = false;  // Checkbox

    int64_t numberValue = 0;  // Number
    int64_t numberMin = 0;
    int64_t numberMax = 0;

    int choiceIndex = 0;  // Choice -- cycles through `choices` on Left/Right
    std::vector<std::wstring> choices;

    std::wstring text;    // Text
    size_t textCursor = 0;  // Text -- IS-0015 Part 3: cursor position within `text`
    // FTP-010: a Text field whose characters are never drawn -- each shows as
    // '*'. Editing is exactly a Text field's (ApplyFormFieldKey is unchanged).
    bool masked = false;
};

// FTP-010: what a Text field shows on screen -- its text, or one '*' per
// character when `masked` (so the cursor still lines up). Pure.
std::wstring DisplayedFieldText(const FormField& field);

// TST-005: what a single key event does to whichever FormField currently
// has focus, pulled out as a pure function the same way ApplyTextPromptKey
// is above. Returns true if `field` changed (the caller should repaint).
// Checkbox: Left/Right/Space toggles `boolValue`. Choice: Left/Right cycles
// `choiceIndex`, wrapping at either end. Number: Left/Right nudges
// `numberValue` by 1 (clamped to [numberMin, numberMax]); a digit character
// appends to it (`numberValue * 10 + digit`, then clamped) and Backspace
// removes the last digit (`numberValue / 10`, then clamped) -- deliberately
// digit-only, no negative-number input, since no field this project
// currently needs is ever negative. Text: delegates to ApplyLineEditKey
// (IS-0015 Part 3) against `text`/`textCursor` -- a real positionable
// cursor, Left/Right included, not just append/Backspace-removes-last-
// character. Any other key (arrows on a Checkbox, a letter on a Number
// field, ...) is a no-op.
bool ApplyFormFieldKey(FormField& field, WORD virtualKeyCode, wchar_t unicodeChar);

// Draws a centered modal box listing every one of `fields` (one per row,
// "Label: value", the focused row highlighted) plus a final "[Enter] OK
// [Esc] Cancel" hint row. `Up`/`Down` moves focus between fields (wrapping);
// every other key is handed to ApplyFormFieldKey for whichever field
// currently has focus. Returns true if the user pressed Enter (every
// field's current value is then authoritative), or false if Esc (fields are
// left however the user had them edited up to that point -- the caller
// should not apply them).
bool ShowFormDialog(Console& console, const std::wstring& title, std::vector<FormField>& fields,
                    const std::function<void()>& redrawBackground);

// FEA-0009: cursor/scroll state of a ShowListChoice list, and the pure
// (TST-005) key handling behind it. Up/Down move by one, PageUp/PageDown by
// `visibleRows`, Home/End jump to the ends -- all clamped, no wrap-around --
// and `top` then follows so the cursor row stays visible. Returns true for a
// recognized navigation key (even when it's a no-op at an end).
struct ListChoiceState {
    int cursor = 0;
    int top = 0;
};
bool ApplyListChoiceKey(ListChoiceState& state, int itemCount, int visibleRows, WORD virtualKeyCode);

// FEA-0009: a centered modal vertical list of `items` -- for an unknown or
// long number of choices, where ShowChoiceDialog's one-line hotkey bar
// doesn't scale (the Shift+F3 action chooser, the "Assign File to App"
// manager). Keyboard-first (ApplyListChoiceKey, Enter, Esc); a mouse click
// on a row or the wheel are additive alternatives (UI-012/KEY-001). Starts
// with `initialIndex` focused. Returns the chosen index, or -1 on Esc / an
// empty list.
int ShowListChoice(Console& console, const std::wstring& title, const std::vector<std::wstring>& items,
                   const std::function<void()>& redrawBackground, int initialIndex = 0);

// NAV-007: ShowListChoice plus `extraKeys` that also close the list (e.g.
// VK_DELETE / VK_F2 in the directory hotlist), returning which key did it
// together with the cursor's index. `key` is VK_RETURN for Enter or a mouse
// click, an extra key's code otherwise; `index` is -1 (and `key` 0) for Esc
// or an empty list. `hint` replaces the default hint line, so it can name
// the extra keys.
struct ListChoiceResult {
    int index = -1;
    WORD key = 0;
};
ListChoiceResult ShowListChoiceWithKeys(Console& console, const std::wstring& title,
                                        const std::vector<std::wstring>& items,
                                        const std::function<void()>& redrawBackground, int initialIndex,
                                        const std::vector<WORD>& extraKeys, const std::wstring& hint);

// A modal busy indicator for synchronous work. Its independent renderer ticks
// every 250 ms even during a single blocking filesystem/compression call.
// Construct/update/destroy on the UI thread. Worker drawing never calls back
// into application or plugin code. Ordinary dialogs suspend it automatically.
class AnimatedProgressDialog {
public:
    AnimatedProgressDialog(Console& console, std::wstring title, uint64_t total,
                           std::function<void()> redrawBackground);
    ~AnimatedProgressDialog();
    AnimatedProgressDialog(const AnimatedProgressDialog&) = delete;
    AnimatedProgressDialog& operator=(const AnimatedProgressDialog&) = delete;
    void Update(uint64_t completed, uint64_t total, const std::wstring& message);
    void Pause();
    void Resume();
    // FOP-015: the *user* paused the operation behind this box (P). Only the
    // rendering changes: "[paused]" in the title and a resume/cancel hint.
    // Distinct from Pause()/Resume(), which suspend the renderer while an
    // ordinary modal dialog is drawn over it.
    void SetUserPaused(bool paused);
    static AnimatedProgressDialog* Current();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    AnimatedProgressDialog* previous_ = nullptr;
};

} // namespace mc
