#include "Console.h"
#include "TextWidth.h"

#include <algorithm>

namespace mc {

Console::Console() {
    originalOutput_ = GetStdHandle(STD_OUTPUT_HANDLE);
    input_ = GetStdHandle(STD_INPUT_HANDLE);

    GetConsoleMode(originalOutput_, &originalOutputMode_);
    GetConsoleMode(input_, &originalInputMode_);
    // UI-012: ENABLE_MOUSE_INPUT lets ReadConsoleInputW hand main.cpp real
    // MOUSE_EVENT records (panel click/scroll/double-click). KEY-001's
    // invariant — every function stays reachable without a mouse — is no
    // longer enforced by withholding mouse events structurally (as it was
    // before UI-012); it now depends on every mouse-triggered action in
    // main.cpp's input loop also having a keyboard path, same as any other
    // feature. ENABLE_EXTENDED_FLAGS without ENABLE_QUICK_EDIT_MODE still
    // turns off the legacy console's own mouse text-selection (QuickEdit),
    // which would otherwise intercept clicks before the app ever saw them
    // as MOUSE_EVENT records — the two are mutually exclusive on a Windows
    // console, so keeping QuickEdit off is what makes mouse input usable
    // here at all, not just cosmetic.
    SetConsoleMode(input_, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT);

    output_ = CreateConsoleScreenBuffer(
        GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        nullptr, CONSOLE_TEXTMODE_BUFFER, nullptr);
    SetConsoleActiveScreenBuffer(output_);
    CONSOLE_CURSOR_INFO cursor{1, FALSE};
    SetConsoleCursorInfo(output_, &cursor);

    UpdateSize();
}

Console::~Console() {
    SetConsoleActiveScreenBuffer(originalOutput_);
    SetConsoleMode(input_, originalInputMode_);
    CloseHandle(output_);
}

void Console::UpdateSize() {
    CONSOLE_SCREEN_BUFFER_INFO info;
    GetConsoleScreenBufferInfo(output_, &info);
    width_ = static_cast<SHORT>(info.srWindow.Right - info.srWindow.Left + 1);
    height_ = static_cast<SHORT>(info.srWindow.Bottom - info.srWindow.Top + 1);

    // Shrink the buffer to the visible window so coordinates in the
    // back buffer line up 1:1 with what WriteConsoleOutput expects.
    COORD size{width_, height_};
    SetConsoleScreenBufferSize(output_, size);
    SMALL_RECT window{0, 0, static_cast<SHORT>(width_ - 1), static_cast<SHORT>(height_ - 1)};
    SetConsoleWindowInfo(output_, TRUE, &window);

    buffer_.assign(static_cast<size_t>(width_) * height_, CHAR_INFO{});
}

void Console::Clear(WORD attributes) {
    for (auto& cell : buffer_) {
        cell.Char.UnicodeChar = L' ';
        cell.Attributes = attributes;
    }
}

void Console::PutText(SHORT x, SHORT y, const std::wstring& text, WORD attributes) {
    if (y < 0 || y >= height_) return;
    // UI-011: advance by each character's actual display width (1 or 2
    // columns), not by 1 code unit per character — otherwise every
    // character after a full-width one lands a column left of where its
    // caller (PadToDisplayWidth et al.) budgeted it, since that budgeting
    // is itself display-width-based.
    SHORT cx = x;
    for (wchar_t c : text) {
        int charWidth = CharDisplayWidth(c);
        if (cx >= 0 && cx < width_) {
            auto& cell = buffer_[static_cast<size_t>(y) * width_ + cx];
            cell.Char.UnicodeChar = c;
            cell.Attributes = attributes;
        }
        cx = static_cast<SHORT>(cx + charWidth);
        if (cx >= width_) break;
    }
}

void Console::FillRow(SHORT y, SHORT x, SHORT count, WCHAR ch, WORD attributes) {
    if (y < 0 || y >= height_) return;
    for (SHORT i = 0; i < count; ++i) {
        SHORT cx = static_cast<SHORT>(x + i);
        if (cx < 0 || cx >= width_) continue;
        auto& cell = buffer_[static_cast<size_t>(y) * width_ + cx];
        cell.Char.UnicodeChar = ch;
        cell.Attributes = attributes;
    }
}

void Console::SuspendForChildProcess() {
    SetConsoleActiveScreenBuffer(originalOutput_);
    SetConsoleMode(input_, originalInputMode_);
}

void Console::ResumeAfterChildProcess() {
    SetConsoleActiveScreenBuffer(output_);
    // UI-012: same mode as the constructor — mouse input stays enabled once
    // control returns to this app's own screen buffer.
    SetConsoleMode(input_, ENABLE_WINDOW_INPUT | ENABLE_EXTENDED_FLAGS | ENABLE_MOUSE_INPUT);
}

void Console::Present() {
    SMALL_RECT region{0, 0, static_cast<SHORT>(width_ - 1), static_cast<SHORT>(height_ - 1)};
    COORD bufSize{width_, height_};
    COORD bufOrigin{0, 0};
    WriteConsoleOutputW(output_, buffer_.data(), bufSize, bufOrigin, &region);
}

} // namespace mc
