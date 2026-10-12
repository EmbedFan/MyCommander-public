// EXT-005: standalone regression harness for the FTP plugin's panel/UI code
// (FtpPlugin.cpp, included directly) driven through a fake McPluginHost
// against the in-memory loopback server (FakeFtpServer.h).
// Build from a Developer Command Prompt:
//   cl /utf-8 /std:c++20 /EHsc /MTd /W4 /FoBuild\ /FeBuild\FtpPanelTests.exe Source\FtpPlugin\FtpPanelTests.cpp
//      Source\FtpPlugin\FtpParse.cpp Source\FtpPlugin\FtpClient.cpp Source\FtpPlugin\FtpConnections.cpp
//      Source\PluginCommon\RemotePanel.cpp Source\PluginCommon\RemoteConnections.cpp Source\PluginCommon\RemoteUi.cpp
// Run: Build\FtpPanelTests.exe   (exit code 0 = every check passed)

#include "FtpPlugin.cpp"

using remote::ServerView;  // the shared panel (EXT-004)
using remote::ViewFor;

#include "FakeFtpServer.h"

#include <algorithm>
#include <cstdio>
#include <deque>
#include <functional>

namespace {

int failures = 0;
int checks = 0;
void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}

// --- the fake host ------------------------------------------------------------

std::deque<int> choiceAnswers;                 // ShowChoice: next answers (default 0)
std::deque<std::wstring> promptAnswers;        // ShowTextPrompt
std::function<bool(McFormField*, int)> formScript;  // ShowForm
std::vector<std::wstring> messages, logLines, drawn, otherSelection;
std::wstring otherPath, viewedContent;
bool released = false;

int __stdcall HostChoice(const wchar_t*, const wchar_t* const*, int, const McDialogOption*, int) {
    if (choiceAnswers.empty()) return 0;
    int a = choiceAnswers.front();
    choiceAnswers.pop_front();
    return a;
}
void __stdcall HostMessage(const wchar_t*, const wchar_t* const* lines, int count) {
    std::wstring all;
    for (int i = 0; i < count; ++i) all += std::wstring(lines[i]) + L" ";
    messages.push_back(all);
}
bool __stdcall HostPrompt(const wchar_t*, const wchar_t*, wchar_t* out, int size) {
    if (promptAnswers.empty()) return false;
    wcsncpy_s(out, static_cast<size_t>(size), promptAnswers.front().c_str(), _TRUNCATE);
    promptAnswers.pop_front();
    return true;
}
bool __stdcall HostForm(const wchar_t*, McFormField* fields, int count) { return formScript && formScript(fields, count); }
void __stdcall HostLog(McLogLevel, const wchar_t* message) { logLines.push_back(message); }
std::map<uint32_t, bool> menuEnabled;  // commandId -> enabled (absent = enabled, the host's default)
bool __stdcall HostAddMenu(wchar_t, const wchar_t*, wchar_t, uint32_t commandId, McMenuPlacement, wchar_t) {
    menuEnabled[commandId] = true;
    return true;
}
void __stdcall HostSetMenuEnabled(uint32_t commandId, bool enabled) { menuEnabled[commandId] = enabled; }
bool __stdcall HostRequestContent(McPanelSide, const wchar_t*) { return true; }
void __stdcall HostReleaseContent(McPanelSide) { released = true; }
void __stdcall HostDraw(McPanelSide, int16_t, int16_t y, const wchar_t* text, McPanelColor) {
    if (y >= static_cast<int16_t>(drawn.size())) drawn.resize(static_cast<size_t>(y) + 1);
    drawn[static_cast<size_t>(y)] = text;
}
McPanelSide __stdcall HostActiveSide() { return kMcPanelLeft; }
void __stdcall HostPanelPath(McPanelSide, wchar_t* out, int size) { wcsncpy_s(out, static_cast<size_t>(size), otherPath.c_str(), _TRUNCATE); }
int __stdcall HostPanelSelection(McPanelSide, wchar_t** out, int count, int maxLength) {
    if (out)
        for (int i = 0; i < count && i < static_cast<int>(otherSelection.size()); ++i)
            wcsncpy_s(out[i], static_cast<size_t>(maxLength), otherSelection[static_cast<size_t>(i)].c_str(), _TRUNCATE);
    return static_cast<int>(otherSelection.size());
}
void __stdcall HostRefresh(McPanelSide) {}
void __stdcall HostViewer(const wchar_t* path) {
    HANDLE h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    char buffer[300000];
    DWORD read = 0;
    ReadFile(h, buffer, sizeof(buffer), &read, nullptr);
    CloseHandle(h);
    viewedContent = std::wstring(buffer, buffer + read);
}
bool __stdcall HostBeginProgress(const wchar_t*, uint64_t) { return true; }
void __stdcall HostUpdateProgress(uint64_t, uint64_t, const wchar_t*) {}
void __stdcall HostEndProgress() {}
bool __stdcall HostPollCancel() { return false; }

McPluginHost MakeHost() {
    McPluginHost host{};
    host.structSize = sizeof(host);
    host.ShowChoice = HostChoice;
    host.ShowMessage = HostMessage;
    host.ShowTextPrompt = HostPrompt;
    host.Log = HostLog;
    host.AddMenuItem = HostAddMenu;
    host.SetMenuItemEnabled = HostSetMenuEnabled;
    host.RequestPanelContent = HostRequestContent;
    host.ReleasePanelContent = HostReleaseContent;
    host.PanelDrawText = HostDraw;
    host.ShowForm = HostForm;
    host.GetActiveSide = HostActiveSide;
    host.GetPanelPath = HostPanelPath;
    host.GetPanelSelectedNames = HostPanelSelection;
    host.RefreshPanel = HostRefresh;
    host.ShowFileViewer = HostViewer;
    host.BeginProgress = HostBeginProgress;
    host.UpdateProgress = HostUpdateProgress;
    host.EndProgress = HostEndProgress;
    host.PollCancel = HostPollCancel;
    return host;
}

// --- helpers ----------------------------------------------------------------------

std::string ReadAll(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "<missing>";
    std::string data;
    char buffer[65536];
    DWORD read = 0;
    while (ReadFile(h, buffer, sizeof(buffer), &read, nullptr) && read > 0) data.append(buffer, read);
    CloseHandle(h);
    return data;
}

void WriteAll(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
}

std::wstring MakeTempDir() {
    wchar_t dir[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    std::wstring path = std::wstring(dir) + L"mc_ftp_panel_test_" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(path.c_str(), nullptr);
    return path;
}

void RemoveTree(const std::wstring& dir) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = fd.cFileName;
            if (name == L"." || name == L"..") continue;
            std::wstring full = dir + L"\\" + name;
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) RemoveTree(full);
            else DeleteFileW(full.c_str());
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir.c_str());
}

ServerView* Left() { return ViewFor(kMcPanelLeft); }

void PutCursorOn(const std::wstring& name) {
    ServerView* view = Left();
    for (size_t i = 0; view && i < view->entries.size(); ++i)
        if (view->entries[i].name == name) view->cursor = static_cast<int>(i) + 1;
}

bool Lists(const std::wstring& name) {
    ServerView* view = Left();
    if (!view) return false;
    return std::any_of(view->entries.begin(), view->entries.end(),
                       [&](const remote::RemoteEntry& e) { return e.name == name; });
}

void Key(uint16_t key, uint32_t control = 0) { McPlugin_OnPanelKey(kMcPanelLeft, key, control); }

}  // namespace

// wmain is the harness entry point, called by the C runtime.
// cppcheck-suppress unusedFunction
int wmain() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    fakeftp::FakeServer server(fakeftp::FakeServerOptions{});
    McPluginHost host = MakeHost();
    otherPath = MakeTempDir();

    Check(McPlugin_GetInfo()->apiVersionMinor >= 9, "declares plugin API 1.9 (password field)");
    Check(McPlugin_Init(&host), "init");
    Check(menuEnabled.count(kDisconnectCommandId) && !menuEnabled[kDisconnectCommandId],
          "Network > Disconnect FTP is hidden while nothing is connected");

    // New FTP connection: the form is filled like a user would.
    bool sawPasswordField = false;
    formScript = [&](McFormField* f, int count) {
        if (count != 10) return false;
        wcsncpy_s(f[1].textBuffer, static_cast<size_t>(f[1].textBufferSize), L"127.0.0.1", _TRUNCATE);
        f[2].numberValue = server.Port();
        wcsncpy_s(f[3].textBuffer, static_cast<size_t>(f[3].textBufferSize), L"tester", _TRUNCATE);
        sawPasswordField = f[4].kind == kMcFormFieldPassword;
        wcsncpy_s(f[4].textBuffer, static_cast<size_t>(f[4].textBufferSize), L"secret", _TRUNCATE);
        f[8].boolValue = false;  // don't save the connection (no file written by the test)
        return true;
    };
    McPlugin_OnMenuAction(kNewConnectionCommandId);
    Check(sawPasswordField, "the password field is masked (FTP-010)");
    Check(Left() != nullptr, "connected; the panel shows the server (FTP-002)");
    if (!Left()) {
        for (const auto& m : messages) std::wprintf(L"  message: %s\n", m.c_str());
        return 1;
    }
    Check(Lists(L"pub") && Lists(L"readme.txt"), "root listing");
    McPlugin_OnPanelDraw(kMcPanelLeft, 80, 10, true);
    Check(!drawn.empty() && drawn[0].find(L"ftp://tester@127.0.0.1:" + std::to_wstring(server.Port()) + L"/") == 0,
          "header shows ftp://user@host:port/path");

    // Enter a folder, view a file.
    PutCursorOn(L"pub");
    Key(VK_RETURN);
    Check(Left()->path == L"/pub" && Lists(L"data.bin") && Lists(L"árvíz.txt"), "Enter opens a folder (UTF-8 names)");
    PutCursorOn(L"árvíz.txt");
    Key(VK_F3);
    Check(viewedContent == L"unicode name", "F3 views a downloaded copy");

    // F5: download to the other panel.
    PutCursorOn(L"data.bin");
    Key(VK_F5);
    Check(ReadAll(otherPath + L"\\data.bin") == std::string(200000, 'x'), "F5 downloads to the other panel (FTP-003)");
    // Again: the conflict prompt; Skip keeps the local file.
    WriteAll(otherPath + L"\\data.bin", "local version");
    choiceAnswers = {2};  // Skip
    Key(VK_F5);
    Check(ReadAll(otherPath + L"\\data.bin") == "local version", "conflict: Skip keeps the local file");
    choiceAnswers = {0};  // Overwrite
    Key(VK_F5);
    Check(ReadAll(otherPath + L"\\data.bin") == std::string(200000, 'x'), "conflict: Overwrite replaces it");
    DWORD attrs = GetFileAttributesW((otherPath + L"\\data.bin.mcftp-part").c_str());
    Check(attrs == INVALID_FILE_ATTRIBUTES, "no temporary download file left behind");

    // Upload from the other panel (F5 there).
    WriteAll(otherPath + L"\\up.txt", "from the client");
    otherSelection = {L"up.txt"};
    Check(McPlugin_OnReceiveFiles(kMcPanelLeft, kMcPanelRight), "the plugin takes the other panel's F5");
    Check(server.Files()["/pub/up.txt"] == "from the client" && Lists(L"up.txt"), "upload into the server folder (FTP-004)");

    // F7, F2, F8.
    promptAnswers = {L"made"};
    Key(VK_F7);
    Check(server.Dirs().count("/pub/made") == 1 && Lists(L"made"), "F7 creates a folder (FTP-005)");
    PutCursorOn(L"up.txt");
    promptAnswers = {L"renamed.txt"};
    Key(VK_F2);
    Check(server.Files().count("/pub/renamed.txt") == 1 && Lists(L"renamed.txt"), "F2 renames");
    PutCursorOn(L"renamed.txt");
    choiceAnswers = {1};  // No
    Key(VK_F8);
    Check(server.Files().count("/pub/renamed.txt") == 1, "F8 asks first; No keeps the file");
    choiceAnswers = {0};  // Yes
    Key(VK_F8);
    Check(server.Files().count("/pub/renamed.txt") == 0 && !Lists(L"renamed.txt"), "F8 Yes deletes on the server");

    // F6: download, then delete on the server.
    Key(VK_BACK);
    Check(Left()->path == L"/" && Left()->cursor > 0 && Left()->entries[static_cast<size_t>(Left()->cursor - 1)].name == L"pub",
          "Backspace goes up, cursor on the folder just left");
    PutCursorOn(L"readme.txt");
    choiceAnswers = {0};  // Yes, move
    Key(VK_F6);
    Check(ReadAll(otherPath + L"\\readme.txt") == "hello ftp\n" && server.Files().count("/readme.txt") == 0,
          "F6 downloads then deletes on the server");

    // FTP-006: the password never reached the log.
    bool leaked = false;
    for (const auto& line : logLines) leaked |= line.find(L"secret") != std::wstring::npos;
    Check(!leaked, "the password never appears in the log");

    // Backspace at the root asks, then disconnects.
    choiceAnswers = {1};
    Key(VK_BACK);
    Check(Left() != nullptr && !released, "No keeps the connection");
    choiceAnswers = {0};
    Key(VK_BACK);
    Check(Left() == nullptr && released, "Yes disconnects and releases the panel");

    // The login password prompt (no password in the connection form) also
    // offers to save it in the Credential Manager. Left unticked here, so the
    // test never touches the real Credential Manager or %APPDATA%.
    bool promptHadSaveOption = false, promptMasked = false;
    // formScript is only called while wmain runs; it is never invoked after wmain returns.
    // cppcheck-suppress danglingLifetime
    formScript = [&](McFormField* f, int count) {
        if (count == 10) {  // the connection form, without a password
            wcsncpy_s(f[1].textBuffer, static_cast<size_t>(f[1].textBufferSize), L"127.0.0.1", _TRUNCATE);
            f[2].numberValue = server.Port();
            wcsncpy_s(f[3].textBuffer, static_cast<size_t>(f[3].textBufferSize), L"tester", _TRUNCATE);
            f[8].boolValue = false;
            return true;
        }
        if (count == 2) {  // the login password prompt
            promptMasked = f[0].kind == kMcFormFieldPassword;
            promptHadSaveOption = f[1].kind == kMcFormFieldCheckbox &&
                                  std::wstring(f[1].label).find(L"Credential Manager") != std::wstring::npos;
            wcsncpy_s(f[0].textBuffer, static_cast<size_t>(f[0].textBufferSize), L"secret", _TRUNCATE);
            f[1].boolValue = false;
            return true;
        }
        return false;
    };
    McPlugin_OnMenuAction(kNewConnectionCommandId);
    Check(promptMasked && promptHadSaveOption, "login prompt: masked password + save-to-Credential-Manager option");
    Check(Left() != nullptr, "connected after the login prompt");
    Check(menuEnabled[kDisconnectCommandId], "Network > Disconnect FTP is offered while connected");
    released = false;
    McPlugin_OnMenuAction(kDisconnectCommandId);
    Check(Left() == nullptr && released && !menuEnabled[kDisconnectCommandId],
          "Disconnect FTP disconnects the panel and hides itself again");

    McPlugin_Shutdown();
    RemoveTree(otherPath);
    std::printf("FTP panel tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
