// Standalone real-console regression: animation must work without callbacks.
#include "../MyCommander/Dialog.h"
#include <cstdio>
#include <stdexcept>
#include <thread>

std::wstring ReadScreen(mc::Console& console) {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(console.OutputHandle(), &info)) throw std::runtime_error("console info");
    COORD size{static_cast<SHORT>(info.srWindow.Right - info.srWindow.Left + 1),
               static_cast<SHORT>(info.srWindow.Bottom - info.srWindow.Top + 1)};
    std::vector<CHAR_INFO> cells(static_cast<size_t>(size.X) * size.Y);
    SMALL_RECT area = info.srWindow;
    if (!ReadConsoleOutputW(console.OutputHandle(), cells.data(), size, {0, 0}, &area)) throw std::runtime_error("console read");
    std::wstring text;
    for (const auto& cell : cells) text += cell.Char.UnicodeChar;
    return text;
}
void Require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    bool allocated = AllocConsole() != FALSE;
    if (allocated) ShowWindow(GetConsoleWindow(), SW_HIDE);
    // The test runner redirects stdin; open the real console input explicitly.
    HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    SetStdHandle(STD_INPUT_HANDLE, input);
    try {
        mc::Console console;
        auto redraw = [&] { console.Clear(7); console.PutText(0, 0, L"BACKGROUND", 7); };
        redraw(); console.Present();
        {
            mc::AnimatedProgressDialog progress(console, L"ZIP regression progress", 10, redraw);
            progress.Update(3, 10, L"Compressing file.bin");
            Sleep(300);
            auto before = ReadScreen(console);
            Require(before.find(L"3/10 items") != std::wstring::npos, "counter missing");
            Require(before.find(L"Compressing file.bin") != std::wstring::npos, "message missing");
            int changes = 0;
            for (int i = 0; i < 6; ++i) {
                Sleep(300); // Deliberately no Update calls while the UI thread is busy.
                auto after = ReadScreen(console);
                if (after != before) ++changes;
                before = std::move(after);
            }
            Require(changes >= 4, "spinner stopped during blocking work");
            progress.Pause();
            before = ReadScreen(console); Sleep(600);
            Require(before == ReadScreen(console), "paused spinner changed");
            progress.Resume();
            progress.Update(0, 0, L"Finalizing archive"); Sleep(300);
            Require(ReadScreen(console).find(L"Working...") != std::wstring::npos, "unknown total missing");
            std::thread key([&] {
                Sleep(400);
                INPUT_RECORD event{}; event.EventType = KEY_EVENT;
                event.Event.KeyEvent.bKeyDown = TRUE; event.Event.KeyEvent.wRepeatCount = 1;
                event.Event.KeyEvent.wVirtualKeyCode = VK_RETURN; event.Event.KeyEvent.uChar.UnicodeChar = L'\r';
                DWORD written = 0; WriteConsoleInputW(console.InputHandle(), &event, 1, &written);
            });
            int choice = mc::ShowChoiceDialog(console, L"Confirmation", {L"Continue?"}, {{L'Y', L"Yes"}, {L'N', L"No"}}, redraw);
            key.join(); Require(choice == 0, "confirmation failed");
            Require(ReadScreen(console).find(L"ZIP regression progress") != std::wstring::npos, "dialog did not resume");
        }
        auto restored = ReadScreen(console);
        Require(restored.find(L"BACKGROUND") != std::wstring::npos, "background not restored");
        Require(restored.find(L"ZIP regression progress") == std::wstring::npos, "progress not closed");
    } catch (const std::exception& e) { std::fprintf(stderr, "FAIL: %s\n", e.what()); return 1; }
    std::puts("PASS: independent animation, counter, unknown total, pause/resume, modal confirmation, close");
    if (allocated) FreeConsole();
    CloseHandle(input);
    return 0;
}
