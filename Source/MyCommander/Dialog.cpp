#include "Dialog.h"
#include "Strings.h"
#include "TextWidth.h"
#include "Theme.h"

#include <algorithm>

namespace mc {

namespace {

// UI-011: both display-width-aware (see TextWidth.h) so a box border stays
// aligned when a title or body line contains full-width (CJK) characters.
std::wstring PadLeft(const std::wstring& text, size_t width) {
    return PadToDisplayWidth(text, width);
}

std::wstring PadCenter(const std::wstring& text, size_t width) {
    return CenterToDisplayWidth(text, width);
}

void DrawBox(Console& console, SHORT x, SHORT y, SHORT width, const std::wstring& title,
             const std::vector<std::wstring>& bodyLines) {
    // UI-014: use Unicode's CP437-compatible single-line frame set by
    // default; ACC-006's portable ASCII fallback comes from Config.
    const bool ascii = console.UseBasicSymbols();
    const wchar_t horizontal = ascii ? L'-' : L'\x2500';
    const wchar_t vertical = ascii ? L'|' : L'\x2502';
    const wchar_t topLeft = ascii ? L'+' : L'\x250C';
    const wchar_t topRight = ascii ? L'+' : L'\x2510';
    const wchar_t bottomLeft = ascii ? L'+' : L'\x2514';
    const wchar_t bottomRight = ascii ? L'+' : L'\x2518';
    const wchar_t leftJunction = ascii ? L'+' : L'\x251C';
    const wchar_t rightJunction = ascii ? L'+' : L'\x2524';

    std::wstring top = std::wstring(1, topLeft) + std::wstring(width - 2, horizontal) + topRight;
    std::wstring divider = std::wstring(1, leftJunction) + std::wstring(width - 2, horizontal) + rightJunction;
    std::wstring bottom = std::wstring(1, bottomLeft) + std::wstring(width - 2, horizontal) + bottomRight;
    console.PutText(x, y, top, kAttrBorder);
    // Paint the frame cells separately from the coloured title/body interior.
    // Otherwise a vertical box-drawing glyph inherits the blue background,
    // making the frame appear as a thick coloured strip (UI-014).
    console.PutText(x, static_cast<SHORT>(y + 1), std::wstring(1, vertical), kAttrBorder);
    console.PutText(static_cast<SHORT>(x + 1), static_cast<SHORT>(y + 1), PadCenter(title, width - 2), kAttrTitle);
    console.PutText(static_cast<SHORT>(x + width - 1), static_cast<SHORT>(y + 1), std::wstring(1, vertical), kAttrBorder);
    console.PutText(x, static_cast<SHORT>(y + 2), divider, kAttrBorder);
    for (size_t i = 0; i < bodyLines.size(); ++i) {
        SHORT rowY = static_cast<SHORT>(y + 3 + i);
        console.PutText(x, rowY, std::wstring(1, vertical), kAttrBorder);
        console.PutText(static_cast<SHORT>(x + 1), rowY, PadLeft(L" " + bodyLines[i], width - 2), kAttrBody);
        console.PutText(static_cast<SHORT>(x + width - 1), rowY, std::wstring(1, vertical), kAttrBorder);
    }
    console.PutText(x, static_cast<SHORT>(y + 3 + bodyLines.size()), bottom, kAttrBorder);
}

// Computes a box wide enough for the title and every body line, capped to
// the screen width, and returns its top-left corner centered on screen.
struct BoxGeometry {
    SHORT x, y, width;
};

BoxGeometry LayOutBox(Console& console, const std::wstring& title, const std::vector<std::wstring>& bodyLines,
                      size_t minContentWidth) {
    size_t contentWidth = std::max<size_t>(StringDisplayWidth(title), minContentWidth);
    for (const auto& line : bodyLines) contentWidth = std::max<size_t>(contentWidth, StringDisplayWidth(line));
    SHORT maxContent = static_cast<SHORT>(std::max<int>(20, console.Width() - 4));
    contentWidth = std::min<size_t>(contentWidth, static_cast<size_t>(maxContent));
    contentWidth = std::max<size_t>(contentWidth, 20);

    SHORT width = static_cast<SHORT>(contentWidth + 4);
    SHORT height = static_cast<SHORT>(bodyLines.size() + 4);
    SHORT x = static_cast<SHORT>(std::max<int>(0, (console.Width() - width) / 2));
    SHORT y = static_cast<SHORT>(std::max<int>(0, (console.Height() - height) / 2));
    return {x, y, width};
}

} // namespace

// MatchDialogHotkey/ApplyTextPromptKey (TST-005's pure, Console-independent
// dialog-input logic) are defined in DialogInput.cpp, not here — kept in a
// separate translation unit so the test project can compile them directly
// (matching Navigation.cpp's precedent) without pulling in this file's real
// Console/Win32 dependency.

int ShowChoiceDialog(Console& console, const std::wstring& title, const std::vector<std::wstring>& lines,
                     const std::vector<DialogOption>& options, const std::function<void()>& redrawBackground) {
    std::wstring optionsLine;
    for (size_t i = 0; i < options.size(); ++i) {
        if (i) optionsLine += L"  ";
        optionsLine += L"[" + std::wstring(1, options[i].hotkey) + L"]" + options[i].label;
    }

    std::vector<std::wstring> body = lines;
    body.push_back(L"");
    body.push_back(optionsLine);

    auto paint = [&]() {
        redrawBackground();
        BoxGeometry geo = LayOutBox(console, title, body, 0);
        DrawBox(console, geo.x, geo.y, geo.width, title, body);
        console.Present();
    };
    paint();

    while (true) {
        INPUT_RECORD record;
        DWORD read = 0;
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;
        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            console.UpdateSize();
            paint();
            continue;
        }
        if (record.EventType == MOUSE_EVENT) {
            // IS-0003: a click on a rendered "[X]Label" option selects it,
            // an additive alternative to pressing its hotkey. Geometry is
            // recomputed fresh here (matching paint()'s own LayOutBox call)
            // since BoxGeometry isn't smuggled out of the paint lambda.
            const MOUSE_EVENT_RECORD& mouse = record.Event.MouseEvent;
            bool isClick = (mouse.dwEventFlags == 0 || mouse.dwEventFlags == DOUBLE_CLICK) &&
                          (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
            if (isClick) {
                BoxGeometry geo = LayOutBox(console, title, body, 0);
                SHORT optionsRow = static_cast<SHORT>(geo.y + 3 + static_cast<SHORT>(body.size()) - 1);
                SHORT startCol = static_cast<SHORT>(geo.x + 2);
                std::vector<ClickableRegion> regions = BuildOptionRegions(options, optionsRow, startCol);
                int matched = MatchClickRegion(regions, mouse.dwMousePosition.X, mouse.dwMousePosition.Y);
                if (matched >= 0) return matched;
            }
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;
        if (record.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) return -1;

        int matched = MatchDialogHotkey(record.Event.KeyEvent.uChar.UnicodeChar, options);
        if (matched >= 0) return matched;
    }
}

void ShowMessage(Console& console, const std::wstring& title, const std::vector<std::wstring>& lines,
                 const std::function<void()>& redrawBackground) {
    std::vector<std::wstring> body = lines;
    body.push_back(L"");
    body.push_back(kStrPressAnyKeyToContinue);

    auto paint = [&]() {
        redrawBackground();
        BoxGeometry geo = LayOutBox(console, title, body, 0);
        DrawBox(console, geo.x, geo.y, geo.width, title, body);
        console.Present();
    };
    paint();

    while (true) {
        INPUT_RECORD record;
        DWORD read = 0;
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;
        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            console.UpdateSize();
            paint();
            continue;
        }
        // IS-0003: a click anywhere dismisses the message, matching "any
        // key" with "any click" -- there's no option list here to hit-test
        // against, just the same unconditional dismissal a keypress gets.
        if (record.EventType == MOUSE_EVENT) {
            const MOUSE_EVENT_RECORD& mouse = record.Event.MouseEvent;
            bool isClick = (mouse.dwEventFlags == 0 || mouse.dwEventFlags == DOUBLE_CLICK) &&
                          (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
            if (isClick) return;
            continue;
        }
        if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) return;
    }
}

std::optional<std::wstring> ShowTextPrompt(Console& console, const std::wstring& title,
                                           const std::wstring& initialText,
                                           const std::function<void()>& redrawBackground) {
    std::wstring text = initialText;

    auto paint = [&]() {
        redrawBackground();
        std::vector<std::wstring> body{text + L"_"};
        BoxGeometry geo = LayOutBox(console, title, body, 30);
        DrawBox(console, geo.x, geo.y, geo.width, title, body);
        console.Present();
    };
    paint();

    while (true) {
        INPUT_RECORD record;
        DWORD read = 0;
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;
        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            console.UpdateSize();
            paint();
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

        WORD vk = record.Event.KeyEvent.wVirtualKeyCode;
        wchar_t ch = record.Event.KeyEvent.uChar.UnicodeChar;
        TextPromptKeyResult result = ApplyTextPromptKey(text, vk, ch);
        if (result.outcome == TextPromptOutcome::Cancelled) return std::nullopt;
        if (result.outcome == TextPromptOutcome::Submitted) return result.text;

        bool changed = result.text != text;
        text = result.text;
        if (vk == VK_BACK || changed) paint();
    }
}

void ShowProgress(Console& console, const std::wstring& title, const std::vector<std::wstring>& lines,
                  const std::function<void()>& redrawBackground) {
    redrawBackground();
    BoxGeometry geo = LayOutBox(console, title, lines, 30);
    DrawBox(console, geo.x, geo.y, geo.width, title, lines);
    console.Present();
}

} // namespace mc
