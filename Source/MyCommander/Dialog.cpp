#include "Dialog.h"
#include "Strings.h"
#include "TextWidth.h"
#include "Theme.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace mc {

namespace {

thread_local AnimatedProgressDialog* activeProgress = nullptr;
struct PauseProgress {
    AnimatedProgressDialog* dialog = AnimatedProgressDialog::Current();
    PauseProgress() { if (dialog) dialog->Pause(); }
    ~PauseProgress() { if (dialog) dialog->Resume(); }
};

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
    PauseProgress pauseProgress;
    int focused = 0;
    std::wstring optionsLine;
    for (size_t i = 0; i < options.size(); ++i) {
        if (i) optionsLine += L"  ";
        optionsLine += L"[" + std::wstring(1, options[i].hotkey) + L"]" + options[i].label;
    }

    std::vector<std::wstring> body = lines;
    body.push_back(L"Left/Right: choose  Enter: confirm  Esc: cancel");
    body.push_back(optionsLine);

    auto paint = [&]() {
        redrawBackground();
        BoxGeometry geo = LayOutBox(console, title, body, 0);
        DrawBox(console, geo.x, geo.y, geo.width, title, body);
        auto regions = BuildOptionRegions(options, static_cast<SHORT>(geo.y + 3 + body.size() - 1),
                                           static_cast<SHORT>(geo.x + 2));
        if (!options.empty()) {
            std::wstring label = L"[" + std::wstring(1, options[focused].hotkey) + L"]" + options[focused].label;
            // IS-0016: kAttrSelected, not kAttrHeaderActive -- the latter
            // equals the box's own kAttrBody, so the focus was invisible.
            console.PutText(regions[focused].startCol, regions[focused].row, label, kAttrSelected);
        }
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
        WORD key = record.Event.KeyEvent.wVirtualKeyCode;
        if (!options.empty() && key == VK_RETURN) return focused;
        if (!options.empty() && (key == VK_LEFT || key == VK_UP || key == VK_RIGHT || key == VK_DOWN || key == VK_TAB)) {
            focused = (focused + ((key == VK_LEFT || key == VK_UP) ? -1 : 1) + (int)options.size()) % (int)options.size();
            paint();
            continue;
        }

        int matched = MatchDialogHotkey(record.Event.KeyEvent.uChar.UnicodeChar, options);
        if (matched >= 0) return matched;
    }
}

void ShowMessage(Console& console, const std::wstring& title, const std::vector<std::wstring>& lines,
                 const std::function<void()>& redrawBackground) {
    PauseProgress pauseProgress;
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
    PauseProgress pauseProgress;
    std::wstring text = initialText;
    // IS-0015 Part 3: starts at the end, matching the old trailing-"_"
    // convention's own implicit position.
    size_t cursor = text.size();
    // IS-0016: first visible character when `text` is wider than the box.
    size_t viewOffset = 0;

    auto paint = [&]() {
        redrawBackground();
        // The box is sized from the full text (capped at the screen width,
        // LayOutBox), then only the scrolled window of it is drawn inside.
        BoxGeometry geo = LayOutBox(console, title, {text}, 30);
        size_t viewWidth = static_cast<size_t>(std::max<int>(1, geo.width - 3));
        viewOffset = ComputeLineEditViewOffset(text, cursor, viewOffset, viewWidth);
        std::vector<std::wstring> body{text.substr(viewOffset)};
        DrawBox(console, geo.x, geo.y, geo.width, title, body);
        // A real cursor glyph at `cursor`'s own position (UI-011 display-
        // width-aware) -- the character there if one exists, or a blank
        // cell at end-of-text. IS-0016: drawn with its own kAttrCursor;
        // the kAttrHeaderActive it used to borrow equals kAttrBody, which
        // made it invisible on the dialog's own background.
        SHORT cursorX = static_cast<SHORT>(geo.x + 2 +
                                           StringDisplayWidth(text.substr(viewOffset, cursor - viewOffset)));
        SHORT cursorY = static_cast<SHORT>(geo.y + 3);
        if (cursorX < static_cast<SHORT>(geo.x + geo.width - 1)) {
            wchar_t cursorChar = (cursor < text.size()) ? text[cursor] : L' ';
            console.PutText(cursorX, cursorY, std::wstring(1, cursorChar), kAttrCursor);
        }
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
        TextPromptKeyResult result = ApplyTextPromptKey(text, cursor, vk, ch);
        if (result.outcome == TextPromptOutcome::Cancelled) return std::nullopt;
        if (result.outcome == TextPromptOutcome::Submitted) return result.text;

        bool changed = result.text != text || result.cursor != cursor;
        text = result.text;
        cursor = result.cursor;
        if (changed) paint();
    }
}

namespace {

// FEA-0007, §1: one "Label: value" text rendering of a FormField, used both
// to measure the dialog's own content width and to paint each row.
// `focused` only affects which attribute paint() uses for this row, not the
// text itself.
std::wstring FormatFormFieldLine(const FormField& field) {
    switch (field.kind) {
        case FormFieldKind::Checkbox:
            return (field.boolValue ? L"[x] " : L"[ ] ") + field.label;
        case FormFieldKind::Choice: {
            std::wstring value = (field.choiceIndex >= 0 && field.choiceIndex < static_cast<int>(field.choices.size()))
                                     ? field.choices[static_cast<size_t>(field.choiceIndex)]
                                     : L"";
            return field.label + L": < " + value + L" >";
        }
        case FormFieldKind::Number:
            return field.label + L": " + std::to_wstring(field.numberValue);
        case FormFieldKind::Text:
            return field.label + L": " + DisplayedFieldText(field);  // FTP-010: masked shows '*'
    }
    return field.label;
}

} // namespace

bool ShowFormDialog(Console& console, const std::wstring& title, std::vector<FormField>& fields,
                    const std::function<void()>& redrawBackground) {
    PauseProgress pauseProgress;
    int focusedIndex = 0;
    // IS-0016: per-field horizontal scroll position for Text fields.
    std::vector<size_t> viewOffsets(fields.size(), 0);

    auto buildBody = [&]() {
        std::vector<std::wstring> body(fields.size());
        std::transform(fields.begin(), fields.end(), body.begin(), [](const FormField& f) { return FormatFormFieldLine(f); });
        body.push_back(L"");
        body.push_back(kStrFormDialogHint);
        return body;
    };

    auto paint = [&]() {
        redrawBackground();
        std::vector<std::wstring> body = buildBody();
        BoxGeometry geo = LayOutBox(console, title, body, 30);
        DrawBox(console, geo.x, geo.y, geo.width, title, body);
        if (!fields.empty()) {
            SHORT rowY = static_cast<SHORT>(geo.y + 3 + focusedIndex);
            const FormField& focused = fields[static_cast<size_t>(focusedIndex)];
            std::wstring line = FormatFormFieldLine(focused);
            size_t offset = 0;
            if (focused.kind == FormFieldKind::Text) {
                // IS-0016: scroll the value part (after "Label: ") so the
                // cursor stays inside the box for an over-long value.
                std::wstring prefix = focused.label + L": ";
                int valueWidth = geo.width - 3 - StringDisplayWidth(prefix);
                size_t& stored = viewOffsets[static_cast<size_t>(focusedIndex)];
                stored = ComputeLineEditViewOffset(focused.text, focused.textCursor, stored,
                                                   static_cast<size_t>(std::max(1, valueWidth)));
                offset = stored;
                line = prefix + DisplayedFieldText(focused).substr(offset);
            }
            console.PutText(static_cast<SHORT>(geo.x + 2), rowY, PadLeft(line, geo.width - 3), kAttrSelected);
            // IS-0015 Part 3: a focused Text field also gets a real cursor
            // glyph, overlaid on top of the focused-row highlight above, at
            // its own textCursor position within the rendered "Label: "
            // prefix plus text (UI-011 display-width-aware). IS-0016: in
            // kAttrCursor, the same look every line editor now shares.
            if (focused.kind == FormFieldKind::Text) {
                const std::wstring shown = DisplayedFieldText(focused);
                SHORT cursorX = static_cast<SHORT>(geo.x + 2 + StringDisplayWidth(focused.label + L": ") +
                                                   StringDisplayWidth(shown.substr(offset, focused.textCursor - offset)));
                if (cursorX < static_cast<SHORT>(geo.x + geo.width - 1)) {
                    wchar_t cursorChar = (focused.textCursor < shown.size()) ? shown[focused.textCursor] : L' ';
                    console.PutText(cursorX, rowY, std::wstring(1, cursorChar), kAttrCursor);
                }
            }
        }
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

        if (vk == VK_ESCAPE) return false;
        if (vk == VK_RETURN) return true;
        if (fields.empty()) continue;

        if (vk == VK_UP) {
            focusedIndex = (focusedIndex - 1 + static_cast<int>(fields.size())) % static_cast<int>(fields.size());
            paint();
            continue;
        }
        if (vk == VK_DOWN || vk == VK_TAB) {
            focusedIndex = (focusedIndex + 1) % static_cast<int>(fields.size());
            paint();
            continue;
        }

        if (ApplyFormFieldKey(fields[static_cast<size_t>(focusedIndex)], vk, ch)) paint();
    }
}

int ShowListChoice(Console& console, const std::wstring& title, const std::vector<std::wstring>& items,
                   const std::function<void()>& redrawBackground, int initialIndex) {
    return ShowListChoiceWithKeys(console, title, items, redrawBackground, initialIndex, {}, kStrListDialogHint).index;
}

ListChoiceResult ShowListChoiceWithKeys(Console& console, const std::wstring& title,
                                        const std::vector<std::wstring>& items,
                                        const std::function<void()>& redrawBackground, int initialIndex,
                                        const std::vector<WORD>& extraKeys, const std::wstring& hint) {
    PauseProgress pauseProgress;
    if (items.empty()) return {};
    const int count = static_cast<int>(items.size());
    ListChoiceState state;
    state.cursor = std::clamp(initialIndex, 0, count - 1);

    size_t widest = 0;
    for (const auto& item : items) widest = std::max<size_t>(widest, StringDisplayWidth(item));
    // Box chrome (4 rows) + blank + hint row, plus a little screen margin.
    auto visibleRowsFor = [&]() { return std::clamp(console.Height() - 8, 1, count); };
    int visibleRows = visibleRowsFor();
    // Keeps `top` such that the cursor row is visible (initially, and after a resize).
    auto keepCursorVisible = [&]() {
        if (state.cursor < state.top) state.top = state.cursor;
        if (state.cursor >= state.top + visibleRows) state.top = state.cursor - visibleRows + 1;
        state.top = std::clamp(state.top, 0, std::max(0, count - visibleRows));
    };
    keepCursorVisible();

    auto layout = [&]() {
        std::vector<std::wstring> body;
        for (int i = 0; i < visibleRows; ++i) body.push_back(items[static_cast<size_t>(state.top + i)]);
        body.push_back(L"");
        body.push_back(hint);
        return body;
    };
    auto paint = [&]() {
        redrawBackground();
        std::vector<std::wstring> body = layout();
        BoxGeometry geo = LayOutBox(console, title, body, widest);
        DrawBox(console, geo.x, geo.y, geo.width, title, body);
        SHORT rowY = static_cast<SHORT>(geo.y + 3 + (state.cursor - state.top));
        console.PutText(static_cast<SHORT>(geo.x + 2), rowY,
                        PadLeft(items[static_cast<size_t>(state.cursor)], geo.width - 3), kAttrSelected);
        console.Present();
    };
    paint();

    while (true) {
        INPUT_RECORD record;
        DWORD read = 0;
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;
        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            console.UpdateSize();
            visibleRows = visibleRowsFor();
            keepCursorVisible();
            paint();
            continue;
        }
        if (record.EventType == MOUSE_EVENT) {
            const MOUSE_EVENT_RECORD& mouse = record.Event.MouseEvent;
            if (mouse.dwEventFlags == MOUSE_WHEELED) {
                WORD vk = static_cast<short>(HIWORD(mouse.dwButtonState)) > 0 ? VK_UP : VK_DOWN;
                for (int i = 0; i < 3; ++i) ApplyListChoiceKey(state, count, visibleRows, vk);
                paint();
                continue;
            }
            bool isClick = (mouse.dwEventFlags == 0 || mouse.dwEventFlags == DOUBLE_CLICK) &&
                           (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
            if (isClick) {
                BoxGeometry geo = LayOutBox(console, title, layout(), widest);
                int row = mouse.dwMousePosition.Y - (geo.y + 3);
                bool insideX = mouse.dwMousePosition.X > geo.x && mouse.dwMousePosition.X < geo.x + geo.width - 1;
                if (insideX && row >= 0 && row < visibleRows) return {state.top + row, VK_RETURN};
            }
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;
        WORD vk = record.Event.KeyEvent.wVirtualKeyCode;
        if (vk == VK_ESCAPE) return {};
        if (vk == VK_RETURN) return {state.cursor, VK_RETURN};
        if (std::find(extraKeys.begin(), extraKeys.end(), vk) != extraKeys.end()) return {state.cursor, vk};
        if (ApplyListChoiceKey(state, count, visibleRows, vk)) paint();
    }
}

struct AnimatedProgressDialog::Impl {
    Console& console;
    HANDLE output;
    std::function<void()> redraw;
    std::wstring title, message = L"Working...";
    uint64_t completed = 0, total;
    WORD normal = kAttrNormal, body = kAttrBody, heading = kAttrTitle, border = kAttrBorder;
    bool ascii, stopping = false, paused = false, userPaused = false;
    SMALL_RECT area{};
    COORD size{};
    std::vector<CHAR_INFO> background;
    std::mutex mutex;
    std::condition_variable changed;
    std::thread renderer;

    Impl(Console& c, std::wstring t, uint64_t n, std::function<void()> r)
        : console(c), output(c.OutputHandle()), redraw(std::move(r)), title(std::move(t)), total(n), ascii(c.UseBasicSymbols()) {}

    void Snapshot() {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(output, &info)) return;
        area = info.srWindow;
        size = {static_cast<SHORT>(area.Right - area.Left + 1), static_cast<SHORT>(area.Bottom - area.Top + 1)};
        background.assign(static_cast<size_t>(size.X) * size.Y, CHAR_INFO{});
        SMALL_RECT readArea = area;
        ReadConsoleOutputW(output, background.data(), size, {0, 0}, &readArea);
    }
    void Paint() {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (!GetConsoleScreenBufferInfo(output, &info)) return;
        if (info.srWindow.Right - info.srWindow.Left + 1 != size.X ||
            info.srWindow.Bottom - info.srWindow.Top + 1 != size.Y) {
            area = info.srWindow;
            size = {static_cast<SHORT>(area.Right - area.Left + 1), static_cast<SHORT>(area.Bottom - area.Top + 1)};
            background.assign(static_cast<size_t>(size.X) * size.Y, CHAR_INFO{});
            for (auto& cell : background) { cell.Char.UnicodeChar = L' '; cell.Attributes = normal; }
        }
        if (size.X < 8 || size.Y < 7) return;
        auto frame = background;
        std::vector<std::wstring> lines;
        size_t start = 0;
        do {
            size_t end = message.find(L'\n', start);
            lines.push_back(message.substr(start, end == std::wstring::npos ? end : end - start));
            if (end == std::wstring::npos) break;
            start = end + 1;
        } while (true);
        const int width = std::min<int>(size.X, 76);
        const int height = std::min<int>(size.Y, 6 + static_cast<int>(lines.size()));
        const int left = (size.X - width) / 2, top = (size.Y - height) / 2;
        auto put = [&](int x, int y, const std::wstring& text, WORD attr) {
            int column = x;
            for (wchar_t ch : text) {
                int columns = CharDisplayWidth(ch);
                if (column + columns > left + width || column + columns > size.X) break;
                auto& cell = frame[static_cast<size_t>(y) * size.X + column];
                cell.Char.UnicodeChar = ch; cell.Attributes = attr;
                for (int extra = 1; extra < columns; ++extra) {
                    auto& padding = frame[static_cast<size_t>(y) * size.X + column + extra];
                    padding.Char.UnicodeChar = L' '; padding.Attributes = attr;
                }
                column += columns;
            }
        };
        for (int row = 0; row < height; ++row) put(left, top + row, std::wstring(width, L' '), body);
        wchar_t horizontal = ascii ? L'-' : L'\x2500', vertical = ascii ? L'|' : L'\x2502';
        put(left, top, std::wstring(1, ascii ? L'+' : L'\x250C') + std::wstring(width - 2, horizontal) + (ascii ? L'+' : L'\x2510'), border);
        put(left, top + height - 1, std::wstring(1, ascii ? L'+' : L'\x2514') + std::wstring(width - 2, horizontal) + (ascii ? L'+' : L'\x2518'), border);
        for (int row = 1; row < height - 1; ++row) {
            put(left, top + row, std::wstring(1, vertical), border);
            put(left + width - 1, top + row, std::wstring(1, vertical), border);
        }
        put(left + 1, top + 1, CenterToDisplayWidth(userPaused ? title + L" [paused]" : title, width - 2), heading);
        const auto tick = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count() / 250;
        const wchar_t frames[] = {L'|', L'/', L'-', L'\\'};
        std::wstring counter = total ? std::to_wstring(std::min(completed, total)) + L"/" + std::to_wstring(total) + L" items" : L"Working...";
        // A paused box doesn't spin: the frozen "[=]" makes the state obvious at a glance.
        const wchar_t spinner = userPaused ? L'=' : frames[tick % 4];
        put(left + 2, top + 3, PadToDisplayWidth(L"[" + std::wstring(1, spinner) + L"] " + counter, width - 4), body);
        for (int row = 0; row < static_cast<int>(lines.size()) && row + 4 < height - 1; ++row)
            put(left + 2, top + 4 + row, PadToDisplayWidth(lines[row], width - 4), body);
        SMALL_RECT writeArea = area;
        WriteConsoleOutputW(output, frame.data(), size, {0, 0}, &writeArea);
    }
    void Start() {
        Snapshot(); Paint();
        renderer = std::thread([this] {
            std::unique_lock<std::mutex> lock(mutex);
            while (!stopping) {
                if (paused) changed.wait(lock, [&] { return stopping || !paused; });
                else {
                    changed.wait_for(lock, std::chrono::milliseconds(250), [&] { return stopping || paused; });
                    // stopping/paused can be changed by the UI thread while wait_for releases the mutex;
                    // cppcheck does not model the other thread.
                    // cppcheck-suppress knownConditionTrueFalse
                    if (!stopping && !paused) Paint();
                }
            }
        });
    }
};

AnimatedProgressDialog* AnimatedProgressDialog::Current() { return activeProgress; }
AnimatedProgressDialog::AnimatedProgressDialog(Console& console, std::wstring title, uint64_t total,
                                               std::function<void()> redrawBackground)
    : impl_(std::make_unique<Impl>(console, std::move(title), total, std::move(redrawBackground))) {
    previous_ = activeProgress;
    if (previous_) previous_->Pause();
    try {
        if (impl_->redraw) impl_->redraw();
        console.Present();
        impl_->Start();
    } catch (...) {
        if (previous_) previous_->Resume();
        throw;
    }
    activeProgress = this;
}
AnimatedProgressDialog::~AnimatedProgressDialog() {
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopping = true;
    }
    impl_->changed.notify_all();
    if (impl_->renderer.joinable()) impl_->renderer.join();
    activeProgress = previous_;
    if (impl_->redraw) {
        impl_->console.UpdateSize();
        impl_->redraw();
        impl_->console.Present();
    } else if (!impl_->background.empty()) {
        SMALL_RECT area = impl_->area;
        WriteConsoleOutputW(impl_->output, impl_->background.data(), impl_->size, {0, 0}, &area);
    }
    if (previous_) previous_->Resume();
}
void AnimatedProgressDialog::Update(uint64_t completed, uint64_t total, const std::wstring& message) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->completed = completed; impl_->total = total; impl_->message = message;
}
void AnimatedProgressDialog::SetUserPaused(bool paused) {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (impl_->userPaused == paused) return;
    impl_->userPaused = paused;
    if (!impl_->paused && !impl_->stopping) impl_->Paint();  // show the change at once, not at the next tick
}
void AnimatedProgressDialog::Pause() {
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->paused = true;
    // The ordinary dialog repaints immediately on the UI thread.
    impl_->changed.notify_all();
}
void AnimatedProgressDialog::Resume() {
    // Only the caller's UI thread invokes the background callback.
    if (impl_->redraw) impl_->redraw();
    impl_->console.Present();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->Snapshot(); impl_->paused = false; impl_->Paint();
    impl_->changed.notify_all();
}

} // namespace mc
