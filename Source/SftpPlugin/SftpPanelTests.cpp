// EXT-004 / EXT-002: standalone regression harness for the SFTP plugin's UI
// (SftpPlugin.cpp, included directly) and the shared remote provider it opens,
// through a fake McPluginHost and an in-memory remote::Client
// (PluginCommon/FakeRemoteClient.h) -- no network, no SSH server, no real
// Credential Manager or %APPDATA% writes. The fake host keeps the McFsProvider
// the plugin opens and drives it through the HOST's adapter and copy engine
// (PluginProviderAdapter / ProviderCopy / LocalProvider), the same code the app
// runs for F3/F5/F6/F7/F8/F2 on a server panel.
// Build: Source\SftpPlugin\BuildTests.cmd (it compiles libssh2 once, since
// SftpPlugin.cpp references sftp::Client), then run Build\SftpPanelTests.exe.

#include "SftpPlugin.cpp"

#include "../MyCommander/LocalProvider.h"
#include "../MyCommander/PluginProviderAdapter.h"
#include "../MyCommander/ProviderCopy.h"
#include "../PluginCommon/FakeRemoteClient.h"

#include <cstdio>
#include <deque>
#include <functional>
#include <map>

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

std::deque<int> choiceAnswers;                      // ShowChoice: next answers (default 0)
std::function<bool(McFormField*, int)> formScript;  // ShowForm
std::vector<std::wstring> messages, logLines, choiceTexts;
std::map<uint32_t, bool> menuEnabled;
std::map<uint32_t, wchar_t> menuMnemonic;
std::shared_ptr<mc::PluginProviderAdapter> panels[2];
std::wstring startPath;

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
bool __stdcall HostPrompt(const wchar_t*, const wchar_t*, wchar_t*, int) { return false; }
bool __stdcall HostForm(const wchar_t*, McFormField* fields, int count) { return formScript && formScript(fields, count); }
void __stdcall HostLog(McLogLevel, const wchar_t* message) { logLines.push_back(message); }
bool __stdcall HostAddMenu(wchar_t, const wchar_t*, wchar_t mnemonic, uint32_t commandId, McMenuPlacement, wchar_t) {
    menuEnabled[commandId] = true;
    menuMnemonic[commandId] = mnemonic;
    return true;
}
void __stdcall HostSetMenuEnabled(uint32_t commandId, bool enabled) { menuEnabled[commandId] = enabled; }
McPanelSide __stdcall HostActiveSide() { return kMcPanelLeft; }
bool __stdcall HostBeginProgress(const wchar_t*, uint64_t) { return true; }
void __stdcall HostUpdateProgress(uint64_t, uint64_t, const wchar_t*) {}
void __stdcall HostEndProgress() {}
bool __stdcall HostPollCancel() { return false; }
bool __stdcall HostOpenProvider(McPanelSide side, const McFsProvider* table, const wchar_t* start, const wchar_t*) {
    std::wstring problem;
    if (!mc::PluginProviderAdapter::Validate(*table, problem)) {
        messages.push_back(L"invalid provider: " + problem);
        return false;
    }
    panels[side] = std::make_shared<mc::PluginProviderAdapter>(*table, L"com.mycommander.sftpplugin");
    startPath = start ? start : L"";
    return true;
}
bool __stdcall HostCloseProvider(McPanelSide side) {
    panels[side].reset();
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

bool Contains(const std::wstring& text, const wchar_t* part) { return text.find(part) != std::wstring::npos; }

const mc::ProviderEntry* Find(const std::vector<mc::ProviderEntry>& entries, const std::wstring& name) {
    for (const auto& e : entries)
        if (e.name == name) return &e;
    return nullptr;
}

std::vector<mc::ProviderEntry> ListLeft(const std::wstring& dir) {
    std::vector<mc::ProviderEntry> entries;
    if (panels[0]) {
        std::wstring err;
        panels[0]->List(dir, entries, {}, err);
    }
    return entries;
}

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

mc::ProviderOpHandlers Conflict(mc::ConflictChoice choice) {
    mc::ProviderOpHandlers h;
    h.onConflict = [choice](const std::filesystem::path&) {
        mc::ConflictAskResult r;
        r.choice = choice;
        return r;
    };
    return h;
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
    tree[L"/home/tester/shared"] = Link(L"/srv/shared");                  // a link to a folder
    tree[L"/home/tester/notes-link"] = Link(L"/home/tester/readme.txt");  // a link to a file
    tree[L"/srv"] = Dir();
    tree[L"/srv/shared"] = Dir();
    tree[L"/srv/shared/big.bin"] = File(std::string(1000, 'b'));

    McPluginHost host = MakeHost();
    const std::wstring local = MakeTempDir();
    mc::LocalProvider disk;

    const McPluginMetadata* meta = McPlugin_GetInfo();
    Check(meta->apiVersionMinor >= 12 && (meta->capabilities & kMcPluginCapFileSystem) &&
              !(meta->capabilities & kMcPluginCapPanel),
          "declares API 1.12, a file-system provider (no panel content any more)");
    Check(McPlugin_Init(&host), "init");
    Check(menuMnemonic[kConnectionsCommandId] == L'P' && menuMnemonic[kNewConnectionCommandId] == L'H' &&
              menuMnemonic[kDisconnectCommandId] == L'X',
          "Network menu items P / H / X (SFTP-001)");
    Check(!menuEnabled[kDisconnectCommandId], "Disconnect SFTP is hidden while nothing is connected");

    // The provider runs on an in-memory server: same protocol, fake client.
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
    Check(panels[0] != nullptr && startPath == L"/home/tester", "connected; the host shows the home folder (SFTP-002)");
    if (!panels[0]) {
        for (const auto& m : messages) std::wprintf(L"  message: %s\n", m.c_str());
        return 1;
    }
    mc::IFileProvider& fs = *panels[0];
    Check(menuEnabled[kDisconnectCommandId], "Disconnect SFTP is offered while connected");
    Check(fs.DisplayRoot() == L"sftp://tester@example.org" && fs.Kind() == L"sftp",
          "header shows sftp://user@host (port 22 left out, never a password)");
    auto home = ListLeft(L"/home/tester");
    const mc::ProviderEntry* shared = Find(home, L"shared");
    const mc::ProviderEntry* notes = Find(home, L"notes-link");
    Check(shared && shared->isDirectory && shared->isLink, "a link to a folder shows as a folder link");
    Check(notes && !notes->isDirectory && notes->isLink, "a link to a file shows as a file link");
    Check(Find(ListLeft(L"/home/tester/shared"), L"big.bin") != nullptr, "a linked folder opens (Enter)");

    // F3: a stream read through the linked folder.
    {
        std::unique_ptr<mc::IReadStream> in;
        std::wstring err;
        std::string content;
        if (fs.OpenRead(L"/home/tester/shared/big.bin", in, {}, err) == mc::ProviderStatus::Ok) {
            char buffer[256];
            size_t got = 0;
            while (in->Read(buffer, sizeof(buffer), got, err) == mc::ProviderStatus::Ok && got > 0) content.append(buffer, got);
        }
        Check(content == std::string(1000, 'b'), "F3 reads a file inside it");
    }

    // F5 with a real folder and a linked folder: the link is never descended into.
    std::vector<std::wstring> notes_;
    mc::ProviderOpHandlers h;
    h.onNote = [&](const std::wstring& n) { notes_.push_back(n); };
    mc::OperationOutcome o = mc::ProviderCopyItems(fs, L"/home/tester", {L"docs", L"shared"}, disk, local, h);
    Check(ReadAll(local + L"\\docs\\a.txt") == "alpha", "F5 downloads a folder recursively (SFTP-003)");
    Check(!Exists(local + L"\\shared"), "a linked folder is skipped, never copied through (SEC-006)");
    Check(notes_.size() == 1 && Contains(notes_[0], L"/home/tester/shared") && o.skipped == 1, "the skipped link is reported");
    Check(!Exists(local + L"\\docs\\a.txt.mc-part"), "no temporary download file left behind");

    // F5 on a link to a file downloads the file it points at.
    o = mc::ProviderCopyItems(fs, L"/home/tester", {L"notes-link"}, disk, local, {});
    Check(ReadAll(local + L"\\notes-link") == "hello sftp\n", "a link to a file downloads its content");

    // Conflict: Skip keeps the local file.
    WriteAll(local + L"\\readme.txt", "local");
    o = mc::ProviderCopyItems(fs, L"/home/tester", {L"readme.txt"}, disk, local, Conflict(mc::ConflictChoice::Skip));
    Check(o.skipped == 1 && ReadAll(local + L"\\readme.txt") == "local", "conflict: Skip keeps the local file");

    // Upload (and over an existing file: replaced only when complete), F7, F2.
    WriteAll(local + L"\\up.txt", "uploaded");
    o = mc::ProviderCopyItems(disk, local, {L"up.txt"}, fs, L"/home/tester", {});
    Check(o.succeeded == 1 && tree[L"/home/tester/up.txt"].data == "uploaded", "F5 from a local panel uploads (SFTP-003)");
    WriteAll(local + L"\\up.txt", "second");
    o = mc::ProviderCopyItems(disk, local, {L"up.txt"}, fs, L"/home/tester", Conflict(mc::ConflictChoice::Overwrite));
    Check(o.succeeded == 1 && tree[L"/home/tester/up.txt"].data == "second" && !tree.count(L"/home/tester/up.txt.mcsftp-part"),
          "an upload over an existing file replaces it, no temporary name left");
    std::wstring err;
    Check(fs.MakeDirectory(L"/home/tester/made", {}, err) == mc::ProviderStatus::Ok && tree[L"/home/tester/made"].dir &&
              Find(ListLeft(L"/home/tester"), L"made"),
          "F7 creates a folder (SFTP-004)");
    Check(fs.Rename(L"/home/tester/up.txt", L"/home/tester/renamed.txt", {}, err) == mc::ProviderStatus::Ok &&
              tree.count(L"/home/tester/renamed.txt") && !tree.count(L"/home/tester/up.txt"),
          "F2 renames");

    // F8 on the linked folder: only the link goes; what it points at stays.
    o = mc::ProviderDeleteItems(fs, L"/home/tester", {L"shared"}, {});
    Check(o.succeeded == 1 && !tree.count(L"/home/tester/shared") && tree.count(L"/srv/shared/big.bin"),
          "F8 on a link removes the link only, never the target's files");

    // F8 on a real folder deletes it with its contents.
    o = mc::ProviderDeleteItems(fs, L"/home/tester", {L"docs"}, {});
    Check(o.succeeded == 1 && !tree.count(L"/home/tester/docs") && !tree.count(L"/home/tester/docs/a.txt"),
          "F8 deletes a folder with its contents");

    // SFTP-006: the password never reached the log.
    bool leaked = false;
    for (const auto& line : logLines) leaked |= Contains(line, L"s3cret");
    Check(!leaked, "the password never appears in the log");

    // Disconnect from the menu.
    McPlugin_OnMenuAction(kDisconnectCommandId);
    Check(panels[0] == nullptr && remote::SessionCount() == 0 && !menuEnabled[kDisconnectCommandId],
          "Disconnect SFTP closes the provider and hides itself");

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
    Check(panels[0] == nullptr && remote::SessionCount() == 0 && !messages.empty() &&
              Contains(messages.back(), L"Wrong user name or password"),
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
    RemoveTree(local);
    std::printf("SFTP panel tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
