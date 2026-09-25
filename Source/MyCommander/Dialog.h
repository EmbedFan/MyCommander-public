#pragma once

#include <functional>
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

// TST-005: what a single key event does to a text prompt's in-progress
// text, pulled out of ShowTextPrompt's own input loop as a pure function.
enum class TextPromptOutcome { StillEditing, Submitted, Cancelled };
struct TextPromptKeyResult {
    TextPromptOutcome outcome = TextPromptOutcome::StillEditing;
    std::wstring text;  // the (possibly unchanged) text; meaningful for StillEditing/Submitted
};

// Esc cancels; Enter submits `currentText` unchanged; Backspace removes the
// last character (a no-op on empty text); a printable character (>= 0x20,
// matching ShowTextPrompt's own guard) is appended as long as `currentText`
// is under the 240-character cap ShowTextPrompt enforces; anything else
// (arrow keys, function keys, a bare modifier, a 0 UnicodeChar) leaves the
// text unchanged and keeps editing.
TextPromptKeyResult ApplyTextPromptKey(const std::wstring& currentText, WORD virtualKeyCode, wchar_t unicodeChar);

// Draws a centered modal single-line text prompt pre-filled with
// `initialText`, editable only at the end (append/backspace). Returns the
// edited text, or std::nullopt if the user pressed Esc.
std::optional<std::wstring> ShowTextPrompt(Console& console, const std::wstring& title,
                                           const std::wstring& initialText,
                                           const std::function<void()>& redrawBackground);

// Draws a centered modal-style box with `title` and `lines`, then returns
// immediately without waiting for input — for a live progress display
// (FOP-009) that must redraw on every tick while its caller's own loop
// polls for Esc-cancellation, unlike ShowChoiceDialog/ShowMessage/
// ShowTextPrompt above, which block until the user dismisses them.
void ShowProgress(Console& console, const std::wstring& title, const std::vector<std::wstring>& lines,
                  const std::function<void()>& redrawBackground);

} // namespace mc
