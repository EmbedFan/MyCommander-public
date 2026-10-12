// EXT-004: the SFTP MC Plugin (AIPrompt/features/EXT-004-plan.md, spec §21.2).
//
// Network > "SFTP connection" (saved connections: connect / edit / delete /
// new), "New SFTP connection" and "Disconnect SFTP" (shown while connected)
// (SFTP-001). A connected server is shown in the active panel through the
// shared PluginCommon/RemotePanel -- the same keys, transfers, conflicts and
// reconnect as the FTP plugin (SFTP-002/003/004/008). This file holds the SSH
// specifics: the connection form (password, key file or SSH agent login,
// SFTP-005), the host-key prompts (DEC-015, SFTP-007), keyboard-interactive
// questions, and the exports. Passwords and passphrases are never shown,
// logged or written to sftp-connections.conf; saving one in the Windows
// Credential Manager is offered at every prompt (SFTP-006).

// winsock2.h must come before anything that includes <windows.h>.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include "../PluginCommon/RemotePanel.h"
#include "SftpClient.h"
#include "SftpConnections.h"
#include "SftpLogic.h"
#include "Version.h"

#include <lmcons.h>
#include <shlobj.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kConnectionsCommandId = 1;
constexpr uint32_t kNewConnectionCommandId = 2;
constexpr uint32_t kDisconnectCommandId = 3;

std::wstring SshFolder() {
    PWSTR profile = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Profile, 0, nullptr, &profile)) && profile) path = std::wstring(profile) + L"\\.ssh";
    CoTaskMemFree(profile);
    return path;
}

bool FileExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// The Windows OpenSSH agent's pipe, or Pageant's window.
bool AgentAvailable() {
    return FileExists(L"\\\\.\\pipe\\openssh-ssh-agent") || FindWindowW(L"Pageant", L"Pageant") != nullptr;
}

std::wstring WindowsUserName() {
    wchar_t name[UNLEN + 1];
    DWORD size = UNLEN + 1;
    return GetUserNameW(name, &size) ? std::wstring(name) : std::wstring();
}

std::wstring FileName(const std::wstring& path) { return path.substr(path.find_last_of(L"\\/") + 1); }

bool KeyFileEncrypted(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::string text(65536, '\0');
    DWORD read = 0;
    ReadFile(h, text.data(), static_cast<DWORD>(text.size()), &read, nullptr);
    CloseHandle(h);
    text.resize(read);
    bool encrypted = sftp::InspectKeyFile(text).encrypted;
    SecureZeroMemory(text.data(), text.size());
    return encrypted;
}

// ---------------------------------------------------------------------------
// SSH prompts

// DEC-015 / SFTP-007: an unknown host's key is shown (SHA-256, as ssh shows
// it) and trusted only on request; a changed key is refused, with no way to
// connect anyway from here.
sftp::HostKeyDecision DecideHostKey(const sftp::HostKeyInfo& info) {
    std::wstring where = info.port == 22 ? info.host : info.host + L":" + std::to_wstring(info.port);
    if (info.verdict == sftp::HostKeyVerdict::Changed) {
        std::vector<std::wstring> lines{
            L"WARNING: the host key of " + where + L" has CHANGED.",
            L"Someone could be intercepting the connection (a man-in-the-middle attack),",
            L"or the server was reinstalled. The connection was refused.",
            L"New key: " + info.keyType + L" " + info.fingerprint,
        };
        if (!info.conflictFile.empty())
            lines.push_back(L"The old key is in " + info.conflictFile +
                            (info.conflictLine > 0 ? L", line " + std::to_wstring(info.conflictLine) : L"") + L".");
        lines.push_back(L"If the change is expected, remove that line, then connect again.");
        remote::Message(L"SSH host key CHANGED", lines);
        remote::Log(kMcLogWarning, L"host key of " + where + L" changed (" + info.fingerprint + L"); refused");
        return sftp::HostKeyDecision::Reject;
    }
    int choice = remote::Choose(L"Unknown SSH host",
                                {where + L" presented a " + info.keyType + L" key:", info.fingerprint,
                                 L"Compare it with the fingerprint the server's administrator gives you."},
                                {{L'T', L"Trust and remember"}, {L'O', L"Once"}, {L'C', L"Cancel"}});
    if (choice == 0) return sftp::HostKeyDecision::AcceptAndRemember;
    if (choice == 1) return sftp::HostKeyDecision::AcceptOnce;
    return sftp::HostKeyDecision::Reject;
}

// A keyboard-interactive question that isn't the password (a one-time code, ...).
std::optional<std::wstring> AskServerPrompt(const std::wstring& title, const std::wstring& prompt, bool echo) {
    std::vector<wchar_t> buffer(256, L'\0');
    McFormField field{};
    field.structSize = sizeof(McFormField);
    field.kind = echo ? kMcFormFieldText : kMcFormFieldPassword;
    field.label = prompt.c_str();
    field.textBuffer = buffer.data();
    field.textBufferSize = static_cast<int32_t>(buffer.size());
    bool ok = remote::Host()->ShowForm(title.c_str(), &field, 1);
    std::wstring answer = buffer.data();
    SecureZeroMemory(buffer.data(), buffer.size() * sizeof(wchar_t));
    if (!ok) return std::nullopt;
    return answer;
}

// ---------------------------------------------------------------------------
// The connection form

const wchar_t* kAuthChoices[] = {L"Password", L"Key file", L"SSH agent"};

// Returns false on Esc. `secret` in/out (password or passphrase); `saveConnection` out.
bool EditConnection(const std::wstring& title, remote::Connection& c, std::wstring& secret, bool& saveConnection) {
    bool isNew = c.host.empty();
    if (isNew) {
        c.auth = AgentAvailable() ? remote::AuthMethod::Agent : remote::AuthMethod::Password;
        if (c.keyFile.empty()) c.keyFile = sftp::DefaultKeyFile(SshFolder(), FileExists);
        if (c.user.empty()) c.user = WindowsUserName();
    }
    std::vector<wchar_t> name(256), host(512), user(256), keyFile(1024), pass(256), folder(1024);
    wcsncpy_s(name.data(), name.size(), c.name.c_str(), _TRUNCATE);
    wcsncpy_s(host.data(), host.size(), c.host.c_str(), _TRUNCATE);
    wcsncpy_s(user.data(), user.size(), c.user.c_str(), _TRUNCATE);
    wcsncpy_s(keyFile.data(), keyFile.size(), c.keyFile.c_str(), _TRUNCATE);
    wcsncpy_s(pass.data(), pass.size(), secret.c_str(), _TRUNCATE);
    wcsncpy_s(folder.data(), folder.size(), c.folder.c_str(), _TRUNCATE);
    McFormField f[11]{};
    auto text = [&](int i, const wchar_t* label, std::vector<wchar_t>& buffer, McFormFieldKind kind = kMcFormFieldText) {
        f[i].structSize = sizeof(McFormField);
        f[i].kind = kind;
        f[i].label = label;
        f[i].textBuffer = buffer.data();
        f[i].textBufferSize = static_cast<int32_t>(buffer.size());
    };
    auto number = [&](int i, const wchar_t* label, int64_t value, int64_t lo, int64_t hi) {
        f[i].structSize = sizeof(McFormField);
        f[i].kind = kMcFormFieldNumber;
        f[i].label = label;
        f[i].numberValue = value;
        f[i].numberMin = lo;
        f[i].numberMax = hi;
    };
    auto check = [&](int i, const wchar_t* label, bool value) {
        f[i].structSize = sizeof(McFormField);
        f[i].kind = kMcFormFieldCheckbox;
        f[i].label = label;
        f[i].boolValue = value;
    };
    text(0, L"Name", name);
    text(1, L"Host (or sftp://user@host:port/folder)", host);
    number(2, L"Port", c.port, 1, 65535);
    text(3, L"User", user);
    f[4].structSize = sizeof(McFormField);
    f[4].kind = kMcFormFieldChoice;
    f[4].label = L"Log in with";
    f[4].choices = kAuthChoices;
    f[4].choiceCount = 3;
    f[4].choiceIndex = static_cast<int32_t>(c.auth);
    text(5, L"Key file (for \"Key file\")", keyFile);
    text(6, L"Password / key passphrase", pass, kMcFormFieldPassword);
    text(7, L"Initial folder", folder);
    number(8, L"Timeout (seconds)", c.timeoutSeconds, 5, 600);
    check(9, L"Save this connection", saveConnection);
    check(10, L"Save the password (Windows Credential Manager)", c.savePassword);
    bool ok = remote::Host()->ShowForm(title.c_str(), f, 11);
    secret = pass.data();
    SecureZeroMemory(pass.data(), pass.size() * sizeof(wchar_t));
    if (!ok) return false;

    c.name = name.data();
    c.host = host.data();
    c.port = static_cast<uint16_t>(f[2].numberValue);
    c.user = user.data();
    c.auth = static_cast<remote::AuthMethod>(std::clamp<int32_t>(f[4].choiceIndex, 0, 2));
    c.keyFile = keyFile.data();
    c.folder = folder.data();
    c.timeoutSeconds = static_cast<int>(f[8].numberValue);
    saveConnection = f[9].boolValue;
    c.savePassword = f[10].boolValue;
    // A pasted sftp:// address fills the other fields (its password is used, never shown).
    if (c.host.find(L"://") != std::wstring::npos || c.host.find(L'@') != std::wstring::npos ||
        c.host.find(L'/') != std::wstring::npos) {
        if (auto url = sftp::ParseSftpUrl(c.host)) {
            c.host = url->host;
            if (url->port != 22 || c.port == 22) c.port = url->port;
            if (!url->user.empty()) c.user = url->user;
            if (!url->password.empty()) {
                secret = url->password;
                c.auth = remote::AuthMethod::Password;
            }
            if (!url->path.empty()) c.folder = url->path;
        }
    }
    if (c.user.empty()) c.user = WindowsUserName();
    if (c.name.empty()) c.name = c.host;
    if (c.auth == remote::AuthMethod::KeyFile && c.keyFile.empty()) {
        remote::Message(title, {L"\"Key file\" login needs the path of a private key file."});
        return false;
    }
    return !c.host.empty();
}

bool NeedsSecret(const remote::Connection& c) {
    if (c.auth == remote::AuthMethod::Password) return true;
    if (c.auth == remote::AuthMethod::KeyFile) return KeyFileEncrypted(c.keyFile);
    return false;  // the agent holds the keys
}

std::wstring SecretLabel(const remote::Connection& c) {
    if (c.auth == remote::AuthMethod::KeyFile) return L"Passphrase for " + FileName(c.keyFile);
    return L"Password for " + c.user;
}

remote::PanelProtocol SftpProtocol() {
    remote::PanelProtocol p;
    p.name = L"SFTP";
    p.scheme = L"sftp";
    p.partSuffix = L".mcsftp-part";
    p.store = &sftp::Store();
    p.connectionsCommandId = kConnectionsCommandId;
    p.newConnectionCommandId = kNewConnectionCommandId;
    p.disconnectCommandId = kDisconnectCommandId;
    p.makeClient = [] { return std::make_unique<sftp::Client>(DecideHostKey, AskServerPrompt); };
    p.editConnection = EditConnection;
    p.needsSecret = NeedsSecret;
    p.secretLabel = SecretLabel;
    return p;
}

}  // namespace

extern "C" __declspec(dllexport) const McPluginMetadata* __stdcall McPlugin_GetInfo(void) {
    static const McPluginMetadata info = {
        sizeof(McPluginMetadata),
        MC_PLUGIN_API_VERSION_MAJOR,
        MC_PLUGIN_API_VERSION_MINOR,  // needs 1.9: kMcFormFieldPassword
        MC_PLUGIN_MAGIC,
        L"com.mycommander.sftpplugin",
        L"SFTP",
        L"SFTP client: browse, download and upload files on SSH servers in a panel.",
        MCPLUGIN_VERSION_WSTR,
        L"MyCommander project",
        kMcPluginCapMenu | kMcPluginCapPanel | kMcPluginCapHelp,
    };
    return &info;
}

extern "C" __declspec(dllexport) bool __stdcall McPlugin_Init(const McPluginHost* host) {
    remote::PanelInit(host, SftpProtocol());
    bool a = host->AddMenuItem(L'N', L"SFTP connection", L'P', kConnectionsCommandId, kMcMenuPlacementAppend, 0);
    bool b = host->AddMenuItem(L'N', L"New SFTP connection", L'H', kNewConnectionCommandId, kMcMenuPlacementAppend, 0);
    bool c = host->AddMenuItem(L'N', L"Disconnect SFTP", L'X', kDisconnectCommandId, kMcMenuPlacementAppend, 0);
    remote::UpdateDisconnectItem();  // hidden until a panel is connected
    remote::Log(a && b && c ? kMcLogInfo : kMcLogWarning,
                a && b && c ? L"menu items registered." : L"a menu item could not be registered.");
    return true;
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_Shutdown(void) { remote::PanelShutdown(); }

extern "C" __declspec(dllexport) const wchar_t* __stdcall McPlugin_GetHelpMarkdown(void) {
    return L"# SFTP panel\n"
           L"- Network > SFTP connection: connect to, edit, delete or add a saved connection\n"
           L"- Network > New SFTP connection: connect (host, or a sftp://user@host:port/folder address)\n"
           L"- Network > Disconnect SFTP (shown while connected): disconnect the active panel's server\n"
           L"- Log in with a password, a private key file (RSA in PEM format, or ECDSA) or the SSH agent\n"
           L"  (the Windows OpenSSH agent or Pageant; use it for Ed25519 and other OpenSSH-format keys)\n"
           L"- First connection: the server's key fingerprint (SHA256:...) is shown; Trust and remember saves it\n"
           L"  in %APPDATA%\\MyCommander\\known_hosts (keys in your .ssh\\known_hosts are trusted too)\n"
           L"- A CHANGED host key is always refused; remove the old line from known_hosts if the change is expected\n"
           L"- Enter: open a folder, or view a file; Backspace / `..`: parent folder (at the root: disconnect)\n"
           L"- F3: view the current file\n"
           L"- F5 / F6: download (copy / move) the selection to the other panel's folder\n"
           L"- F5 in the other panel: upload its selection into the current server folder\n"
           L"- F7: create a folder; F2: rename; F8 / Delete: delete (always asks; there is no Recycle Bin)\n"
           L"- Symbolic links to folders open with Enter but are never copied into or deleted through\n"
           L"- Ctrl+R: re-read the folder; Esc: cancel a running transfer\n"
           L"- Insert, Shift+Up/Down, Num+ / Num- / Num*, Ctrl+Num+ / Ctrl+Num-, right-click: selection\n"
           L"- Saved passwords and passphrases are kept only in the Windows Credential Manager, never in a file\n"
           L"  or the log; every password prompt offers \"Save the password (Windows Credential Manager)\"\n";
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_OnMenuAction(uint32_t commandId) {
    remote::OnMenuAction(commandId);
}

extern "C" __declspec(dllexport) bool __stdcall McPlugin_OnReceiveFiles(McPanelSide side, McPanelSide sourceSide) {
    return remote::OnReceiveFiles(side, sourceSide);
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_OnPanelDraw(McPanelSide side, uint16_t width, uint16_t height,
                                                                    bool active) {
    remote::Draw(side, width, height, active);
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_OnPanelKey(McPanelSide side, uint16_t key, uint32_t control) {
    remote::HandleKey(side, key, control);
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_OnPanelClick(McPanelSide side, int16_t x, int16_t y,
                                                                     bool doubleClick) {
    remote::HandleMouse(side, x, y, FROM_LEFT_1ST_BUTTON_PRESSED, doubleClick ? DOUBLE_CLICK : 0, 0);
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_OnPanelMouse(McPanelSide side, int16_t x, int16_t y,
                                                                     uint32_t buttons, uint32_t flags, uint32_t control) {
    remote::HandleMouse(side, x, y, buttons, flags, control);
}
