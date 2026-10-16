// EXT-005 / EXT-002: standalone regression harness for the FTP plugin
// (FtpPlugin.cpp, included directly) driven through a fake McPluginHost
// against the in-memory loopback server (FakeFtpServer.h). Since EXT-002 the
// plugin is a file-system provider: the fake host keeps the McFsProvider the
// plugin opens and drives it through the HOST's own adapter and copy engine
// (PluginProviderAdapter / ProviderCopy / LocalProvider), exactly the code the
// app runs for F3/F5/F6/F7/F8/F2 on a server panel.
// Build from a Developer Command Prompt (repository root):
//   cl /utf-8 /std:c++20 /EHsc /MTd /W4 /FoBuild\ /FeBuild\FtpPanelTests.exe Source\FtpPlugin\FtpPanelTests.cpp
//      Source\FtpPlugin\FtpParse.cpp Source\FtpPlugin\FtpClient.cpp Source\FtpPlugin\FtpConnections.cpp
//      Source\PluginCommon\RemotePanel.cpp Source\PluginCommon\RemoteConnections.cpp Source\PluginCommon\RemoteUi.cpp
//      Source\MyCommander\PluginProviderAdapter.cpp Source\MyCommander\ProviderCopy.cpp
//      Source\MyCommander\LocalProvider.cpp Source\MyCommander\MemoryProvider.cpp Source\MyCommander\FileProvider.cpp
// Run: Build\FtpPanelTests.exe   (exit code 0 = every check passed)

#include "FtpPlugin.cpp"

#include "../MyCommander/LocalProvider.h"
#include "../MyCommander/MemoryProvider.h"
#include "../MyCommander/PluginProviderAdapter.h"
#include "../MyCommander/ProviderCopy.h"
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
    if (GetEnvironmentVariableW(L"MC_TEST_VERBOSE", nullptr, 0) > 0) {
        std::printf("check: %s\n", label);
        std::fflush(stdout);
    }
    if (!condition) {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}

// --- the fake host ------------------------------------------------------------

std::deque<int> choiceAnswers;                      // ShowChoice: next answers (default 0)
std::function<bool(McFormField*, int)> formScript;  // ShowForm
std::vector<std::wstring> messages, logLines;
std::map<uint32_t, bool> menuEnabled;  // commandId -> enabled
std::shared_ptr<mc::PluginProviderAdapter> panels[2];
std::wstring startPath;

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
bool __stdcall HostPrompt(const wchar_t*, const wchar_t*, wchar_t*, int) { return false; }
bool __stdcall HostForm(const wchar_t*, McFormField* fields, int count) { return formScript && formScript(fields, count); }
void __stdcall HostLog(McLogLevel, const wchar_t* message) { logLines.push_back(message); }
bool __stdcall HostAddMenu(wchar_t, const wchar_t*, wchar_t, uint32_t commandId, McMenuPlacement, wchar_t) {
    menuEnabled[commandId] = true;
    return true;
}
void __stdcall HostSetMenuEnabled(uint32_t commandId, bool enabled) { menuEnabled[commandId] = enabled; }
McPanelSide __stdcall HostActiveSide() { return kMcPanelLeft; }
bool __stdcall HostBeginProgress(const wchar_t*, uint64_t) { return true; }
void __stdcall HostUpdateProgress(uint64_t, uint64_t, const wchar_t*) {}
void __stdcall HostEndProgress() {}
bool __stdcall HostPollCancel() { return false; }
bool __stdcall HostWaitWhilePaused() { return true; }
bool __stdcall HostOpenProvider(McPanelSide side, const McFsProvider* table, const wchar_t* start, const wchar_t*) {
    std::wstring problem;
    if (!mc::PluginProviderAdapter::Validate(*table, problem)) {
        messages.push_back(L"invalid provider: " + problem);
        return false;
    }
    panels[side] = std::make_shared<mc::PluginProviderAdapter>(*table, L"com.mycommander.ftpplugin");
    startPath = start ? start : L"";
    return true;
}
bool __stdcall HostCloseProvider(McPanelSide side) {
    panels[side].reset();  // the adapter calls the table's Close
    return true;
}
void* __stdcall HostGetPanelProvider(McPanelSide side) { return panels[side] ? panels[side]->Context() : nullptr; }

McPluginHost MakeHost() {
    McPluginHost host{};
    host.structSize = sizeof(host);
    host.ShowChoice = HostChoice;
    host.ShowMessage = HostMessage;
    host.ShowTextPrompt = HostPrompt;
    host.Log = HostLog;
    host.AddMenuItem = HostAddMenu;
    host.SetMenuItemEnabled = HostSetMenuEnabled;
    host.ShowForm = HostForm;
    host.GetActiveSide = HostActiveSide;
    host.BeginProgress = HostBeginProgress;
    host.UpdateProgress = HostUpdateProgress;
    host.EndProgress = HostEndProgress;
    host.PollCancel = HostPollCancel;
    host.WaitWhilePaused = HostWaitWhilePaused;
    host.OpenProvider = HostOpenProvider;
    host.CloseProvider = HostCloseProvider;
    host.GetPanelProvider = HostGetPanelProvider;
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

bool Exists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

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

std::vector<std::wstring> Names(const std::wstring& dir) {
    std::vector<mc::ProviderEntry> entries;
    std::wstring err;
    if (!panels[0] || panels[0]->List(dir, entries, {}, err) != mc::ProviderStatus::Ok) return {};
    std::vector<std::wstring> names;
    for (const auto& e : entries) names.push_back(e.name);
    return names;
}

bool Lists(const std::wstring& dir, const std::wstring& name) {
    auto names = Names(dir);
    return std::find(names.begin(), names.end(), name) != names.end();
}

mc::ProviderOpHandlers Conflict(mc::ConflictChoice choice) {
    mc::ProviderOpHandlers h;
    h.onConflict = [choice](const std::filesystem::path&) {
        mc::ConflictAskResult r;
        r.choice = choice;
        return r;
    };
    return h;
}

void FillForm(McFormField* f, uint16_t port, const wchar_t* password) {
    wcsncpy_s(f[1].textBuffer, static_cast<size_t>(f[1].textBufferSize), L"127.0.0.1", _TRUNCATE);
    f[2].numberValue = port;
    wcsncpy_s(f[3].textBuffer, static_cast<size_t>(f[3].textBufferSize), L"tester", _TRUNCATE);
    if (password) wcsncpy_s(f[4].textBuffer, static_cast<size_t>(f[4].textBufferSize), password, _TRUNCATE);
    f[8].boolValue = false;  // don't save the connection (no file written by the test)
}

}  // namespace

// wmain is the harness entry point, called by the C runtime.
// cppcheck-suppress unusedFunction
int wmain() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    fakeftp::FakeServer server(fakeftp::FakeServerOptions{});
    McPluginHost host = MakeHost();
    const std::wstring local = MakeTempDir();
    mc::LocalProvider disk;

    const McPluginMetadata* info = McPlugin_GetInfo();
    Check(info->apiVersionMinor >= 12, "declares plugin API 1.12 (file-system providers)");
    Check((info->capabilities & kMcPluginCapFileSystem) && !(info->capabilities & kMcPluginCapPanel),
          "a file-system provider, no longer a panel-content plugin");
    Check(McPlugin_Init(&host), "init");
    Check(menuEnabled.count(kDisconnectCommandId) && !menuEnabled[kDisconnectCommandId],
          "Network > Disconnect FTP is hidden while nothing is connected");

    // New FTP connection: the form is filled like a user would.
    bool sawPasswordField = false;
    formScript = [&](McFormField* f, int count) {
        if (count != 10) return false;
        sawPasswordField = f[4].kind == kMcFormFieldPassword;
        FillForm(f, server.Port(), L"secret");
        return true;
    };
    McPlugin_OnMenuAction(kNewConnectionCommandId);
    Check(sawPasswordField, "the password field is masked (FTP-010)");
    Check(panels[0] != nullptr && remote::SessionCount() == 1, "connected; the host shows the server in the panel (FTP-002)");
    if (!panels[0]) {
        for (const auto& m : messages) std::wprintf(L"  message: %s\n", m.c_str());
        return 1;
    }
    mc::IFileProvider& ftpFs = *panels[0];
    Check(menuEnabled[kDisconnectCommandId], "Network > Disconnect FTP is offered while connected");
    Check(startPath == L"/", "starts in the home folder");
    Check(ftpFs.DisplayRoot() == L"ftp://tester@127.0.0.1:" + std::to_wstring(server.Port()),
          "header shows ftp://user@host:port (never a password)");
    Check((ftpFs.Capabilities() & mc::CapFileTransfer) != 0, "whole-file transfers are offered");
    Check(Lists(L"/", L"pub") && Lists(L"/", L"readme.txt"), "root listing");
    Check(Lists(L"/pub", L"data.bin") && Lists(L"/pub", L"árvíz.txt"), "a folder listing (UTF-8 names)");

    // F3: a stream read fetches the file (the host's view path).
    {
        std::unique_ptr<mc::IReadStream> in;
        std::wstring err;
        std::string content;
        if (ftpFs.OpenRead(L"/pub/árvíz.txt", in, {}, err) == mc::ProviderStatus::Ok) {
            char buffer[64];
            size_t got = 0;
            while (in->Read(buffer, sizeof(buffer), got, err) == mc::ProviderStatus::Ok && got > 0) content.append(buffer, got);
        }
        Check(content == "unicode name", "F3 reads the file through the provider");
    }

    // F5: download to a local folder (DownloadToFile through the host engine).
    mc::OperationOutcome o = mc::ProviderCopyItems(ftpFs, L"/pub", {L"data.bin"}, disk, local, {});
    Check(o.succeeded == 1 && ReadAll(local + L"\\data.bin") == std::string(200000, 'x'), "F5 downloads (FTP-003)");
    WriteAll(local + L"\\data.bin", "local version");
    o = mc::ProviderCopyItems(ftpFs, L"/pub", {L"data.bin"}, disk, local, Conflict(mc::ConflictChoice::Skip));
    Check(o.skipped == 1 && ReadAll(local + L"\\data.bin") == "local version", "conflict: Skip keeps the local file");
    o = mc::ProviderCopyItems(ftpFs, L"/pub", {L"data.bin"}, disk, local, Conflict(mc::ConflictChoice::Overwrite));
    Check(ReadAll(local + L"\\data.bin") == std::string(200000, 'x'), "conflict: Overwrite replaces it");
    Check(!Exists(local + L"\\data.bin.mc-part"), "no temporary download file left behind");

    // Esc during a download: no file and no part file.
    DeleteFileW((local + L"\\data.bin").c_str());
    {
        int progress = 0;
        mc::ProviderOpHandlers h;
        h.onByteProgress = [&](uint64_t, uint64_t) { ++progress; };
        h.cancelPoll = [&] { return progress >= 2; };
        o = mc::ProviderCopyItems(ftpFs, L"/pub", {L"data.bin"}, disk, local, h);
    }
    Check(o.cancelled && !Exists(local + L"\\data.bin") && !Exists(local + L"\\data.bin.mc-part"),
          "Esc mid-download leaves nothing behind (FTP-009)");

    // F5 from the local panel: upload (UploadFromFile: temporary name, then renamed into place).
    WriteAll(local + L"\\up.txt", "from the client");
    o = mc::ProviderCopyItems(disk, local, {L"up.txt"}, ftpFs, L"/pub", {});
    if (GetEnvironmentVariableW(L"MC_TEST_VERBOSE", nullptr, 0) > 0) {
        for (const auto& f : o.failed) std::wprintf(L"  failed: %s: %s\n", f.first.c_str(), f.second.c_str());
        for (size_t i = logLines.size() > 12 ? logLines.size() - 12 : 0; i < logLines.size(); ++i)
            std::wprintf(L"  log: %s\n", logLines[i].c_str());
        std::fflush(stdout);
    }
    Check(o.succeeded == 1 && server.Files()["/pub/up.txt"] == "from the client" && Lists(L"/pub", L"up.txt"),
          "upload into the server folder (FTP-004)");
    Check(server.Files().count("/pub/up.txt.mcftp-part") == 0, "no temporary upload name left on the server");
    WriteAll(local + L"\\up.txt", "second version");
    o = mc::ProviderCopyItems(disk, local, {L"up.txt"}, ftpFs, L"/pub", Conflict(mc::ConflictChoice::Overwrite));
    Check(o.succeeded == 1 && server.Files()["/pub/up.txt"] == "second version" &&
              server.Files().count("/pub/up.txt.mcftp-part") == 0,
          "an upload over an existing file replaces it only when complete");

    // Folder trees both ways.
    CreateDirectoryW((local + L"\\tree").c_str(), nullptr);
    CreateDirectoryW((local + L"\\tree\\sub").c_str(), nullptr);
    WriteAll(local + L"\\tree\\sub\\deep.txt", "deep");
    o = mc::ProviderCopyItems(disk, local, {L"tree"}, ftpFs, L"/pub", {});
    Check(o.succeeded == 1 && server.Files()["/pub/tree/sub/deep.txt"] == "deep", "a folder tree uploads");

    // F7 / F2 / F8 on the server.
    std::wstring err;
    Check(ftpFs.MakeDirectory(L"/pub/made", {}, err) == mc::ProviderStatus::Ok && server.Dirs().count("/pub/made") == 1 &&
              Lists(L"/pub", L"made"),
          "F7 creates a folder (FTP-005)");
    Check(ftpFs.Rename(L"/pub/up.txt", L"/pub/renamed.txt", {}, err) == mc::ProviderStatus::Ok &&
              server.Files().count("/pub/renamed.txt") == 1 && Lists(L"/pub", L"renamed.txt"),
          "F2 renames");
    o = mc::ProviderDeleteItems(ftpFs, L"/pub", {L"renamed.txt", L"made"}, {});
    Check(o.succeeded == 2 && server.Files().count("/pub/renamed.txt") == 0 && server.Dirs().count("/pub/made") == 0,
          "F8 deletes files and folders on the server");

    // F6: move from the server = download, then delete there.
    o = mc::ProviderMoveItems(ftpFs, L"/", {L"readme.txt"}, disk, local, {});
    Check(o.succeeded == 1 && ReadAll(local + L"\\readme.txt") == "hello ftp\n" && server.Files().count("/readme.txt") == 0,
          "F6 downloads then deletes on the server");

    // FTP <-> another provider (an archive, another server): staged through a local temp file.
    mc::MemoryProvider archive(L"zip://test.zip");
    o = mc::ProviderCopyItems(ftpFs, L"/pub", {L"data.bin"}, archive, L"/", {});
    Check(o.succeeded == 1 && archive.Content(L"/data.bin") == std::string(200000, 'x'), "server -> archive copy");
    archive.AddFile(L"/from-zip.txt", "zipped");
    o = mc::ProviderCopyItems(archive, L"/", {L"from-zip.txt"}, ftpFs, L"/pub", {});
    Check(o.succeeded == 1 && server.Files()["/pub/from-zip.txt"] == "zipped", "archive -> server copy");

    // FTP-006: the password never reached the log.
    bool leaked = false;
    for (const auto& line : logLines) leaked |= line.find(L"secret") != std::wstring::npos;
    Check(!leaked, "the password never appears in the log");

    // `..` at the top / Network > Disconnect: the host closes the provider, the plugin disconnects.
    McPlugin_OnMenuAction(kDisconnectCommandId);
    Check(panels[0] == nullptr && remote::SessionCount() == 0 && !menuEnabled[kDisconnectCommandId],
          "Disconnect FTP closes the panel's provider and hides itself again");

    // The login password prompt (no password in the connection form) also
    // offers to save it in the Credential Manager. Left unticked here, so the
    // test never touches the real Credential Manager or %APPDATA%.
    bool promptHadSaveOption = false, promptMasked = false;
    // formScript is only called while wmain runs; it is never invoked after wmain returns.
    // cppcheck-suppress danglingLifetime
    formScript = [&](McFormField* f, int count) {
        if (count == 10) {  // the connection form, without a password
            FillForm(f, server.Port(), nullptr);
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
    Check(panels[0] != nullptr, "connected after the login prompt");
    panels[0].reset();  // the panel leaves the provider (`..` at the top folder)
    Check(remote::SessionCount() == 0 && !menuEnabled[kDisconnectCommandId], "leaving the panel disconnects");

    McPlugin_Shutdown();
    RemoveTree(local);
    std::printf("FTP panel tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
