// End-to-end Win32 console regression harness. Launches the supplied executable
// in a hidden console with an isolated profile and fixture directory under Build.
// Build with ZipConsoleTests.cpp and third_party/miniz/miniz.c using MSVC C++20.
#include "ZipPlugin.cpp"
#include <filesystem>

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
    const auto fixture = std::filesystem::absolute(L"Build/zip-console-fixture");
    std::filesystem::create_directories(fixture);
    const auto profile = std::filesystem::absolute(L"Build/zip-console-profile");
    std::filesystem::create_directories(profile);
    SetEnvironmentVariableW(L"APPDATA", profile.c_str());
    std::vector<ArchiveEntry> entries;
    for (int i = 0; i < 20; ++i) {
        wchar_t name[32]; swprintf_s(name, L"file%02d.txt", i);
        entries.push_back({name, {42}, 0, 0});
    }
    const auto zip = fixture / L"navigation.zip";
    if (!WriteArchiveEntries(zip.wstring(), entries)) return 3;
    std::wstring cmd = L"\"" + std::filesystem::absolute(argv[1]).wstring() + L"\" \"" + fixture.wstring() + L"\"";
    STARTUPINFOW si{}; si.cb = sizeof(si); si.dwFlags = STARTF_USESHOWWINDOW; si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, FALSE, CREATE_NEW_CONSOLE, nullptr, nullptr, &si, &pi)) return 4;
    Sleep(1500);
    FreeConsole();
    if (!AttachConsole(pi.dwProcessId)) { TerminateProcess(pi.hProcess, 5); return 5; }
    HANDLE input = CreateFileW(L"CONIN$", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    auto key = [&](WORD vk) {
        INPUT_RECORD records[2]{};
        for (int i = 0; i < 2; ++i) {
            records[i].EventType = KEY_EVENT;
            records[i].Event.KeyEvent.bKeyDown = i == 0;
            records[i].Event.KeyEvent.wRepeatCount = 1;
            records[i].Event.KeyEvent.wVirtualKeyCode = vk;
            records[i].Event.KeyEvent.dwControlKeyState = ENHANCED_KEY;
        }
        DWORD written; WriteConsoleInputW(input, records, 2, &written); Sleep(300);
    };
    auto cursorRow = [&]() {
        HANDLE output = CreateFileW(L"CONOUT$", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        CONSOLE_SCREEN_BUFFER_INFO info{}; GetConsoleScreenBufferInfo(output, &info);
        int result = -1;
        for (SHORT y = 0; y < info.dwSize.Y; ++y) {
            wchar_t text[256]{}; DWORD read = 0;
            ReadConsoleOutputCharacterW(output, text, std::min<SHORT>(255, info.dwSize.X / 2), {0,y}, &read);
            if (std::wstring(text, read).find(L">  file") != std::wstring::npos) result = y;
        }
        CloseHandle(output); return result;
    };
    key(VK_HOME); key(VK_DOWN); key(VK_RETURN);
    int first = cursorRow();
    key(VK_DOWN); int second = cursorRow();
    key(VK_UP); int third = cursorRow();
    auto screenHas = [&](const std::wstring& needle) {
        HANDLE output = CreateFileW(L"CONOUT$", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        CONSOLE_SCREEN_BUFFER_INFO info{}; GetConsoleScreenBufferInfo(output, &info);
        std::wstring text((size_t)info.dwSize.X * info.dwSize.Y, L' '); DWORD read;
        ReadConsoleOutputCharacterW(output, text.data(), (DWORD)text.size(), {0,0}, &read);
        CloseHandle(output); return text.find(needle) != std::wstring::npos;
    };
    int failed = 0;
    auto check = [&](bool condition, const char* label) { if (!condition) { printf("FAIL: %s\n", label); ++failed; } };
    key(VK_F1); key(VK_END); check(screenHas(L"ZIP panel"), "F1 shows ZIP help"); key(VK_ESCAPE);
    auto mouse = [&](SHORT row, DWORD buttons, DWORD control = 0) {
        INPUT_RECORD event{}; event.EventType = MOUSE_EVENT;
        event.Event.MouseEvent.dwMousePosition = {4, row};
        event.Event.MouseEvent.dwButtonState = buttons;
        event.Event.MouseEvent.dwControlKeyState = control;
        DWORD written; WriteConsoleInputW(input, &event, 1, &written); Sleep(150);
    };
    mouse((SHORT)(first + 1), RIGHTMOST_BUTTON_PRESSED);
    mouse((SHORT)(first + 1), 0);
    mouse((SHORT)(first + 2), RIGHTMOST_BUTTON_PRESSED, LEFT_CTRL_PRESSED);
    mouse((SHORT)(first + 2), 0);
    check(screenHas(L"* file01.txt") && screenHas(L"* file02.txt"), "mouse multi-selection reaches ZIP panel");
    HANDLE output = CreateFileW(L"CONOUT$", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    WORD color = 0; DWORD readColor;
    ReadConsoleOutputAttribute(output, &color, 1, {5,(SHORT)(first + 1)}, &readColor);
    check(color == (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY), "selection has yellow foreground");
    CloseHandle(output);
    INPUT_RECORD clear{}; clear.EventType = KEY_EVENT; clear.Event.KeyEvent.bKeyDown = TRUE;
    clear.Event.KeyEvent.wVirtualKeyCode = VK_SUBTRACT; clear.Event.KeyEvent.wRepeatCount = 1;
    clear.Event.KeyEvent.dwControlKeyState = LEFT_CTRL_PRESSED;
    DWORD cleared; WriteConsoleInputW(input, &clear, 1, &cleared); Sleep(150); key(VK_HOME);
    key(VK_F3); check(screenHas(L"mcz"), "F3 opens viewer"); key(VK_ESCAPE);
    key(VK_F5); check(ReadFileBytes((fixture / L"file00.txt").wstring()) == std::vector<uint8_t>{42}, "F5 extracts cursor file");
    INPUT_RECORD attr{}; attr.EventType = KEY_EVENT; attr.Event.KeyEvent.bKeyDown = TRUE;
    attr.Event.KeyEvent.wVirtualKeyCode = 'A'; attr.Event.KeyEvent.wRepeatCount = 1;
    attr.Event.KeyEvent.dwControlKeyState = LEFT_CTRL_PRESSED;
    DWORD written; WriteConsoleInputW(input, &attr, 1, &written); Sleep(300);
    check(screenHas(L"Archive attributes"), "Ctrl+A opens attribute dialog"); key(VK_RETURN);
    ArchiveView stored; LoadArchive(zip.wstring(), stored);
    check(stored.entries[0].attributes & FILE_ATTRIBUTE_READONLY, "Ctrl+A saves attribute after redraw");
    key(VK_F8); check(screenHas(L"Delete from archive"), "F8 opens confirmation"); key(VK_ESCAPE);
    LoadArchive(zip.wstring(), stored); check(stored.entries.size() == 20, "Esc cancels delete");
    key(VK_F8); key(VK_RETURN);
    LoadArchive(zip.wstring(), stored); check(stored.entries.size() == 19, "Enter confirms delete");
    key(VK_BACK); key(VK_RETURN); check(!screenHas(L"Packed"), "leave archive restores filesystem after dialog redraw");
    key(VK_F10);
    DWORD wait = WaitForSingleObject(pi.hProcess, 3000);
    if (wait == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 6);
    CloseHandle(input); FreeConsole(); CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    DeleteFileW(zip.c_str());
    DeleteFileW((fixture / L"file00.txt").c_str());
    printf("Console ZIP cursor rows: %d -> %d -> %d\n", first, second, third);
    return first >= 0 && second == first + 1 && third == first && !failed ? 0 : 1;
}
