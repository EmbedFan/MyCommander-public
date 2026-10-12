// EXT-004: standalone regression harness for the SFTP plugin's UI (SftpPlugin.cpp,
// included directly) and the shared remote panel it drives, through a fake
// McPluginHost and an in-memory remote::Client (PluginCommon/FakeRemoteClient.h)
// -- no network, no SSH server, no real Credential Manager or %APPDATA% writes.
// Build: Source\SftpPlugin\BuildTests.cmd (it compiles libssh2 once, since
// SftpPlugin.cpp references sftp::Client), then run Build\SftpPanelTests.exe.

#include "SftpPlugin.cpp"

#include "../PluginCommon/FakeRemoteClient.h"

#include <cstdio>
#include <deque>
#include <functional>
#include <map>

using remote::ServerView;
using remote::ViewFor;

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

std::deque<int> choiceAnswers;                       // ShowChoice: next answers (default 0)
std::deque<std::wstring> promptAnswers;              // ShowTextPrompt
std::function<bool(McFormField*, int)> formScript;   // ShowForm
std::vector<std::wstring> messages, logLines, drawn, otherSelection, choiceTexts;
std::wstring otherPath, viewedContent;
bool released = false;
std::map<uint32_t, bool> menuEnabled;
std::map<uint32_t, wchar_t> menuMnemonic;

int __stdcall HostChoice(const wchar_t* title, const wchar_t* const* lines, int count, const McDialogOption*, int) {
    std::wstring all = std::wstring(title) + L": ";
    for (int i = 0; i < count; ++i) all += std::wstring(lines[i]) + L" | ";
    choiceTexts.push_back(all);
    if (choiceAnswers.empty()) return 0;
    int a = choiceAnswers.front();
    choiceAnswers.pop_front();
    return a;
}
void __stdcall HostMessage(const wchar_t* title, const wchar_t* const* lines, int count) {
    std::wstring all = std::wstring(title) + L": ";
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
bool __stdcall HostAddMenu(wchar_t, const wchar_t*, wchar_t mnemonic, uint32_t commandId, McMenuPlacement, wchar_t) {
    menuEnabled[commandId] = true;
    menuMnemonic[commandId] = mnemonic;
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
    char buffer[4096];
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
    std::wstring path = std::wstring(dir) + L"mc_sftp_panel_test_" + std::to_wstring(GetTickCount64());
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

const remote::RemoteEntry* Entry(const std::wstring& name) {
    ServerView* view = Left();
    for (size_t i = 0; view && i < view->entries.size(); ++i)
        if (view->entries[i].name == name) return &view->entries[i];
    return nullptr;
}

void Key(uint16_t key, uint32_t control = 0) { McPlugin_OnPanelKey(kMcPanelLeft, key, control); }

bool Contains(const std::wstring& text, const wchar_t* part) { return text.find(part) != std::wstring::npos; }

fakeremote::Node Dir() {
    fakeremote::Node n;
    n.dir = true;
    return n;
}
fakeremote::Node File(const std::string& data) {
    fakeremote::Node n;
    n.data = data;
    return n;
}
fakeremote::Node Link(const std::wstring& target) {
    fakeremote::Node n;
    n.link = true;
    n.target = target;
    return n;
}

// The connection form, filled the way a user would (field order: Name, Host,
// Port, User, Log in with, Key file, Password, Initial folder, Timeout, Save
// this connection, Save the password).
bool FillForm(McFormField* f, int count, int auth, const wchar_t* password) {
    if (count != 11) return false;
    wcsncpy_s(f[1].textBuffer, static_cast<size_t>(f[1].textBufferSize), L"example.org", _TRUNCATE);
    wcsncpy_s(f[3].textBuffer, static_cast<size_t>(f[3].textBufferSize), L"tester", _TRUNCATE);
    f[4].choiceIndex = auth;
    wcsncpy_s(f[6].textBuffer, static_cast<size_t>(f[6].textBufferSize), password, _TRUNCATE);
    f[9].boolValue = false;   // don't save the connection (no %APPDATA% file written)
    f[10].boolValue = false;  // never touch the real Credential Manager
    return true;
}

}  // namespace

int wmain() {
    fakeremote::Tree tree;
    tree[L"/home"] = Dir();
    tree[L"/home/tester"] = Dir();
    tree[L"/home/tester/docs"] = Dir();
    tree[L"/home/tester/docs/a.txt"] = File("alpha");
    tree[L"/home/tester/readme.txt"] = File("hello sftp\n");
    tree[L"/home/tester/shared"] = Link(L"/srv/shared");          // a link to a folder
    tree[L"/home/tester/notes-link"] = Link(L"/home/tester/readme.txt");  // a link to a file
    tree[L"/srv"] = Dir();
    tree[L"/srv/shared"] = Dir();
    tree[L"/srv/shared/big.bin"] = File(std::string(1000, 'b'));

    McPluginHost host = MakeHost();
    otherPath = MakeTempDir();

    Check(McPlugin_GetInfo()->apiVersionMinor >= 9, "declares plugin API 1.9 or later (password field)");
    Check(McPlugin_Init(&host), "init");
    Check(menuMnemonic[kConnectionsCommandId] == L'P' && menuMnemonic[kNewConnectionCommandId] == L'H' &&
              menuMnemonic[kDisconnectCommandId] == L'X',
          "Network menu items P / H / X (SFTP-001)");
    Check(!menuEnabled[kDisconnectCommandId], "Disconnect SFTP is hidden while nothing is connected");

    // The panel runs on an in-memory server: same protocol, fake client.
    remote::PanelProtocol protocol = SftpProtocol();
    protocol.makeClient = [&] { return std::make_unique<fakeremote::FakeClient>(tree, L"s3cret", L"/home/tester"); };
    remote::PanelInit(&host, protocol);

    // New SFTP connection: the form has the SSH fields.
    bool sawMaskedPassword = false, sawAuthChoice = false;
    formScript = [&](McFormField* f, int count) {
        if (count == 11) {
            sawMaskedPassword = f[6].kind == kMcFormFieldPassword;
            sawAuthChoice = f[4].kind == kMcFormFieldChoice && f[4].choiceCount == 3 &&
                            std::wstring(f[4].choices[2]) == L"SSH agent";
        }
        return FillForm(f, count, 0, L"s3cret");
    };
    McPlugin_OnMenuAction(kNewConnectionCommandId);
    Check(sawMaskedPassword && sawAuthChoice, "form: masked password, Password / Key file / SSH agent (SFTP-005)");
    Check(Left() != nullptr && Left()->path == L"/home/tester", "connected, in the home folder (SFTP-002)");
    if (!Left()) {
        for (const auto& m : messages) std::wprintf(L"  message: %s\n", m.c_str());
        return 1;
    }
    Check(menuEnabled[kDisconnectCommandId], "Disconnect SFTP is offered while connected");
    McPlugin_OnPanelDraw(kMcPanelLeft, 80, 10, true);
    Check(!drawn.empty() && drawn[0].find(L"sftp://tester@example.org/home/tester") == 0,
          "header shows sftp://user@host/path (port 22 left out, never a password)");
    Check(Entry(L"shared") && Entry(L"shared")->isDirectory && Entry(L"shared")->isLink, "a link to a folder shows as a folder");
    Check(Entry(L"notes-link") && !Entry(L"notes-link")->isDirectory && Entry(L"notes-link")->isLink,
          "a link to a file shows as a file");

    // Enter opens the linked folder; F3 views through it.
    PutCursorOn(L"shared");
    Key(VK_RETURN);
    Check(Left()->path == L"/home/tester/shared" && Entry(L"big.bin"), "Enter opens a linked folder");
    PutCursorOn(L"big.bin");
    Key(VK_F3);
    Check(viewedContent == std::wstring(1000, L'b'), "F3 views a file inside it");
    Key(VK_BACK);
    Check(Left()->path == L"/home/tester" && Left()->cursor > 0 &&
              Left()->entries[static_cast<size_t>(Left()->cursor - 1)].name == L"shared",
          "Backspace goes up, cursor on the folder just left");

    // F5 with a real folder and a linked folder selected: the link is never descended into.
    Left()->selected = {L"docs", L"shared"};
    Key(VK_F5);
    Check(ReadAll(otherPath + L"\\docs\\a.txt") == "alpha", "F5 downloads a folder recursively (SFTP-003)");
    Check(GetFileAttributesW((otherPath + L"\\shared").c_str()) == INVALID_FILE_ATTRIBUTES,
          "a linked folder is skipped, never copied through");
    bool skipLogged = false;
    for (const auto& line : logLines) skipLogged |= Contains(line, L"skipped the linked folder /home/tester/shared");
    Check(skipLogged, "the skipped link is logged");
    Check(GetFileAttributesW((otherPath + L"\\docs\\a.txt.mcsftp-part").c_str()) == INVALID_FILE_ATTRIBUTES,
          "no temporary download file left behind");

    // F5 on a link to a file downloads the file it points at.
    Left()->selected.clear();
    PutCursorOn(L"notes-link");
    Key(VK_F5);
    Check(ReadAll(otherPath + L"\\notes-link") == "hello sftp\n", "a link to a file downloads its content");

    // Conflict: Skip keeps the local file.
    WriteAll(otherPath + L"\\readme.txt", "local");
    PutCursorOn(L"readme.txt");
    choiceAnswers = {2};  // Skip
    Key(VK_F5);
    Check(ReadAll(otherPath + L"\\readme.txt") == "local", "conflict: Skip keeps the local file");

    // Upload, F7, F2.
    WriteAll(otherPath + L"\\up.txt", "uploaded");
    otherSelection = {L"up.txt"};
    Check(McPlugin_OnReceiveFiles(kMcPanelLeft, kMcPanelRight) && tree[L"/home/tester/up.txt"].data == "uploaded",
          "F5 in the other panel uploads (SFTP-003)");
    promptAnswers = {L"made"};
    Key(VK_F7);
    Check(tree.count(L"/home/tester/made") && tree[L"/home/tester/made"].dir && Entry(L"made"), "F7 creates a folder (SFTP-004)");
    PutCursorOn(L"up.txt");
    promptAnswers = {L"renamed.txt"};
    Key(VK_F2);
    Check(tree.count(L"/home/tester/renamed.txt") && !tree.count(L"/home/tester/up.txt"), "F2 renames");

    // F8 on the linked folder: only the link goes; what it points at stays.
    PutCursorOn(L"shared");
    choiceTexts.clear();
    choiceAnswers = {0};  // Yes
    Key(VK_F8);
    Check(!choiceTexts.empty() && Contains(choiceTexts[0], L"\"shared\"") && !Contains(choiceTexts[0], L"everything in it"),
          "deleting a link doesn't claim to delete what it points to");
    Check(!tree.count(L"/home/tester/shared") && tree.count(L"/srv/shared/big.bin"),
          "F8 on a link removes the link only, never the target's files");

    // F8 on a real folder deletes it with its contents.
    PutCursorOn(L"docs");
    choiceAnswers = {0};
    Key(VK_F8);
    Check(!tree.count(L"/home/tester/docs") && !tree.count(L"/home/tester/docs/a.txt"), "F8 deletes a folder with its contents");

    // SFTP-006: the password never reached the log.
    bool leaked = false;
    for (const auto& line : logLines) leaked |= Contains(line, L"s3cret");
    Check(!leaked, "the password never appears in the log");

    // Disconnect from the menu.
    released = false;
    McPlugin_OnMenuAction(kDisconnectCommandId);
    Check(Left() == nullptr && released && !menuEnabled[kDisconnectCommandId], "Disconnect SFTP disconnects and hides itself");

    // The login prompt (no password in the form) is masked and offers the Credential Manager.
    bool promptMasked = false, promptOffersSave = false;
    formScript = [&](McFormField* f, int count) {
        if (count == 11) return FillForm(f, count, 0, L"");
        if (count == 2) {
            promptMasked = f[0].kind == kMcFormFieldPassword && Contains(f[0].label, L"Password for tester");
            promptOffersSave = f[1].kind == kMcFormFieldCheckbox && Contains(f[1].label, L"Credential Manager");
            wcsncpy_s(f[0].textBuffer, static_cast<size_t>(f[0].textBufferSize), L"wrong", _TRUNCATE);
            f[1].boolValue = false;
            return true;
        }
        return false;
    };
    messages.clear();
    McPlugin_OnMenuAction(kNewConnectionCommandId);
    Check(promptMasked && promptOffersSave, "login prompt: masked + save-to-Credential-Manager option (SFTP-006)");
    Check(Left() == nullptr && !messages.empty() && Contains(messages.back(), L"Wrong user name or password"),
          "a wrong password is reported, nothing connected");

    // --- SSH specifics (no network: the plugin's own prompt functions) -----------
    sftp::HostKeyInfo info;
    info.host = L"example.org";
    info.port = 2222;
    info.keyType = L"ecdsa-sha2-nistp256";
    info.fingerprint = L"SHA256:OzvpQxRUzSfV9F/ECMXbQ7B7zbK0aTngrhFCBUno65c";
    info.verdict = sftp::HostKeyVerdict::Unknown;
    choiceTexts.clear();
    choiceAnswers = {0};
    Check(DecideHostKey(info) == sftp::HostKeyDecision::AcceptAndRemember, "unknown host: Trust and remember");
    Check(!choiceTexts.empty() && Contains(choiceTexts[0], L"SHA256:OzvpQxRUzSfV9F/ECMXbQ7B7zbK0aTngrhFCBUno65c") &&
              Contains(choiceTexts[0], L"example.org:2222"),
          "the prompt shows the host and the SHA-256 fingerprint (SFTP-007)");
    choiceAnswers = {1};
    Check(DecideHostKey(info) == sftp::HostKeyDecision::AcceptOnce, "unknown host: Once");
    choiceAnswers = {2};
    Check(DecideHostKey(info) == sftp::HostKeyDecision::Reject, "unknown host: Cancel");
    info.verdict = sftp::HostKeyVerdict::Changed;
    info.conflictFile = L"C:\\x\\known_hosts";
    info.conflictLine = 7;
    choiceTexts.clear();
    messages.clear();
    Check(DecideHostKey(info) == sftp::HostKeyDecision::Reject && choiceTexts.empty(),
          "a changed key is refused without offering to connect anyway");
    Check(!messages.empty() && Contains(messages[0], L"CHANGED") && Contains(messages[0], L"C:\\x\\known_hosts, line 7"),
          "the warning names the file and line of the old key");

    bool answerMasked = false;
    // formScript is only called while wmain runs; it is never invoked after wmain returns.
    // cppcheck-suppress danglingLifetime
    formScript = [&](McFormField* f, int count) {
        answerMasked = count == 1 && f[0].kind == kMcFormFieldPassword;
        wcsncpy_s(f[0].textBuffer, static_cast<size_t>(f[0].textBufferSize), L"123456", _TRUNCATE);
        return true;
    };
    auto code = AskServerPrompt(L"Login", L"Verification code: ", false);
    Check(code && *code == L"123456" && answerMasked, "keyboard-interactive question asked masked");

    remote::Connection agent;
    agent.auth = remote::AuthMethod::Agent;
    remote::Connection key;
    key.auth = remote::AuthMethod::KeyFile;
    key.keyFile = L"C:\\nowhere\\id_rsa";
    Check(!NeedsSecret(agent) && !NeedsSecret(key), "no password prompt for the agent or an unencrypted/missing key");
    Check(SecretLabel(key) == L"Passphrase for id_rsa", "the prompt asks for the key's passphrase");

    McPlugin_Shutdown();
    RemoveTree(otherPath);
    std::printf("SFTP panel tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
