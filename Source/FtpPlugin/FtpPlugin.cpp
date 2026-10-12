// EXT-005: the FTP MC Plugin (AIPrompt/features/EXT-005-plan.md, spec §21.1).
//
// Network > "FTP connection" (saved connections: connect / edit / delete /
// new) and "New FTP connection" (the connection form) replace the host's old
// stubs (FTP-001). A connected server is shown in the active panel as plugin
// content (FTP-002): browse, select, F3 view, F5 download / F6 download-then-
// delete to the other panel (FTP-003), F5 from the other panel uploads
// (FTP-004), F7 / F8 / F2 / Ctrl+R on the server (FTP-005). Passwords stay in
// memory or, opt-in, in the Windows Credential Manager -- never on screen, in
// the log or in ftp-connections.conf (FTP-006/007). The protocol is FtpClient
// (DEC-012); every wait is bounded and Esc works (FTP-009).
//
// Since EXT-004 the panel, transfers and connection manager are the shared
// PluginCommon/RemotePanel (also used by the SFTP plugin); this file holds
// the FTP specifics: the ftp::Client adapter, the connection form, the menu
// items and the exports.

// winsock2.h must come before anything that includes <windows.h>, or
// windows.h pulls in the old winsock.h and the two collide.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>

#include "../PluginCommon/RemotePanel.h"
#include "FtpClient.h"
#include "FtpConnections.h"
#include "FtpParse.h"
#include "Version.h"

#include <memory>
#include <string>
#include <vector>

namespace {

constexpr uint32_t kConnectionsCommandId = 1;
constexpr uint32_t kNewConnectionCommandId = 2;
constexpr uint32_t kDisconnectCommandId = 3;

// ftp::Client behind the shared panel's remote::Client interface.
class FtpRemoteClient final : public remote::Client {
    ftp::Client client_;

public:
    remote::Status Connect(const remote::Connection& c, const std::wstring& secret, const remote::CancelFn& cancel,
                           std::wstring& error) override {
        ftp::ClientOptions o;
        o.host = c.host;
        o.port = c.port;
        o.user = c.user;
        o.password = secret;
        o.passive = c.passive;
        o.timeoutSeconds = c.timeoutSeconds;
        return client_.Connect(o, cancel, error);
    }
    void Disconnect() override { client_.Disconnect(); }
    void SetTrace(remote::TraceFn trace) override { client_.SetTrace(std::move(trace)); }  // PASS is masked
    remote::Status HomeDirectory(std::wstring& path, const remote::CancelFn& cancel, std::wstring& error) override {
        return client_.CurrentDirectory(path, cancel, error);
    }
    remote::Status List(const std::wstring& dir, std::vector<remote::RemoteEntry>& entries,
                        const remote::CancelFn& cancel, std::wstring& error) override {
        return client_.List(dir, entries, cancel, error);
    }
    remote::Status Download(const std::wstring& remotePath, const std::wstring& localPath, uint64_t sizeHint,
                            const remote::ProgressFn& progress, const remote::CancelFn& cancel,
                            std::wstring& error) override {
        return client_.Download(remotePath, localPath, sizeHint, progress, cancel, error);
    }
    remote::Status Upload(const std::wstring& localPath, const std::wstring& remotePath,
                          const remote::ProgressFn& progress, const remote::CancelFn& cancel,
                          std::wstring& error) override {
        return client_.Upload(localPath, remotePath, progress, cancel, error);
    }
    remote::Status MakeRemoteDirectory(const std::wstring& path, const remote::CancelFn& cancel,
                                       std::wstring& error) override {
        return client_.MakeRemoteDirectory(path, cancel, error);
    }
    remote::Status RemoveRemoteDirectory(const std::wstring& path, const remote::CancelFn& cancel,
                                         std::wstring& error) override {
        return client_.RemoveRemoteDirectory(path, cancel, error);
    }
    remote::Status DeleteRemoteFile(const std::wstring& path, const remote::CancelFn& cancel,
                                    std::wstring& error) override {
        return client_.DeleteRemoteFile(path, cancel, error);
    }
    remote::Status RenameRemote(const std::wstring& from, const std::wstring& to, const remote::CancelFn& cancel,
                                std::wstring& error) override {
        return client_.RenameRemote(from, to, cancel, error);
    }
};

// The connection form. Returns false on Esc. `password` in/out; `saveConnection` out.
bool EditConnection(const std::wstring& title, remote::Connection& c, std::wstring& password, bool& saveConnection) {
    std::vector<wchar_t> name(256), host(512), user(256), pass(256), folder(1024);
    wcsncpy_s(name.data(), name.size(), c.name.c_str(), _TRUNCATE);
    wcsncpy_s(host.data(), host.size(), c.host.c_str(), _TRUNCATE);
    wcsncpy_s(user.data(), user.size(), c.user.c_str(), _TRUNCATE);
    wcsncpy_s(pass.data(), pass.size(), password.c_str(), _TRUNCATE);
    wcsncpy_s(folder.data(), folder.size(), c.folder.c_str(), _TRUNCATE);
    McFormField f[10]{};
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
    text(1, L"Host (or ftp://user@host:port/folder)", host);
    number(2, L"Port", c.port, 1, 65535);
    text(3, L"User", user);
    text(4, L"Password", pass, kMcFormFieldPassword);
    text(5, L"Initial folder", folder);
    check(6, L"Passive mode", c.passive);
    number(7, L"Timeout (seconds)", c.timeoutSeconds, 5, 600);
    check(8, L"Save this connection", saveConnection);
    check(9, L"Save the password (Windows Credential Manager)", c.savePassword);
    std::wstring fullTitle = title + L" -- plain FTP sends the password unencrypted";
    bool ok = remote::Host()->ShowForm(fullTitle.c_str(), f, 10);
    password = pass.data();
    SecureZeroMemory(pass.data(), pass.size() * sizeof(wchar_t));
    if (!ok) return false;

    c.name = name.data();
    c.host = host.data();
    c.port = static_cast<uint16_t>(f[2].numberValue);
    c.user = user.data();
    c.folder = folder.data();
    c.passive = f[6].boolValue;
    c.timeoutSeconds = static_cast<int>(f[7].numberValue);
    saveConnection = f[8].boolValue;
    c.savePassword = f[9].boolValue;
    // A pasted ftp:// URL fills the other fields (FTP-006: its password is used, never shown).
    if (c.host.find(L"://") != std::wstring::npos || c.host.find(L'@') != std::wstring::npos ||
        c.host.find(L'/') != std::wstring::npos) {
        if (auto url = ftp::ParseFtpUrl(c.host)) {
            c.host = url->host;
            if (url->port != 21 || c.port == 21) c.port = url->port;
            if (!url->user.empty()) c.user = url->user;
            if (!url->password.empty()) password = url->password;
            if (!url->path.empty()) c.folder = url->path;
        }
    }
    if (c.user.empty()) c.user = L"anonymous";
    if (c.name.empty()) c.name = c.host;
    return !c.host.empty();
}

bool IsAnonymous(const remote::Connection& c) {
    return c.user.empty() || _wcsicmp(c.user.c_str(), L"anonymous") == 0;
}

remote::PanelProtocol FtpProtocol() {
    remote::PanelProtocol p;
    p.name = L"FTP";
    p.scheme = L"ftp";
    p.partSuffix = L".mcftp-part";
    p.store = &ftp::Store();
    p.connectionsCommandId = kConnectionsCommandId;
    p.newConnectionCommandId = kNewConnectionCommandId;
    p.disconnectCommandId = kDisconnectCommandId;
    p.makeClient = [] { return std::make_unique<FtpRemoteClient>(); };
    p.editConnection = EditConnection;
    p.needsSecret = [](const remote::Connection& c) { return !IsAnonymous(c); };
    p.secretLabel = [](const remote::Connection& c) { return L"Password for " + c.user; };
    p.loginTitleNote = L" (plain FTP: sent unencrypted)";
    return p;
}

}  // namespace

extern "C" __declspec(dllexport) const McPluginMetadata* __stdcall McPlugin_GetInfo(void) {
    static const McPluginMetadata info = {
        sizeof(McPluginMetadata),
        MC_PLUGIN_API_VERSION_MAJOR,
        MC_PLUGIN_API_VERSION_MINOR,  // 1.9: needs kMcFormFieldPassword
        MC_PLUGIN_MAGIC,
        L"com.mycommander.ftpplugin",
        L"FTP",
        L"FTP client: browse, download and upload files on FTP servers in a panel.",
        MCPLUGIN_VERSION_WSTR,
        L"MyCommander project",
        kMcPluginCapMenu | kMcPluginCapPanel | kMcPluginCapHelp,
    };
    return &info;
}

extern "C" __declspec(dllexport) bool __stdcall McPlugin_Init(const McPluginHost* host) {
    remote::PanelInit(host, FtpProtocol());
    // 'F' / 'W': the mnemonics of the Network menu stubs these replace (FTP-001).
    bool a = host->AddMenuItem(L'N', L"FTP connection", L'F', kConnectionsCommandId, kMcMenuPlacementAppend, 0);
    bool b = host->AddMenuItem(L'N', L"New FTP connection", L'W', kNewConnectionCommandId, kMcMenuPlacementAppend, 0);
    bool c = host->AddMenuItem(L'N', L"Disconnect FTP", L'D', kDisconnectCommandId, kMcMenuPlacementAppend, 0);
    remote::UpdateDisconnectItem();  // hidden until a panel is connected
    remote::Log(a && b && c ? kMcLogInfo : kMcLogWarning,
                a && b && c ? L"menu items registered." : L"a menu item could not be registered.");
    return true;
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_Shutdown(void) { remote::PanelShutdown(); }

extern "C" __declspec(dllexport) const wchar_t* __stdcall McPlugin_GetHelpMarkdown(void) {
    return L"# FTP panel\n"
           L"- Network > FTP connection: connect to, edit, delete or add a saved connection\n"
           L"- Network > New FTP connection: connect (host, or a ftp://user@host:port/folder address)\n"
           L"- Network > Disconnect FTP (shown while connected): disconnect the active panel's server\n"
           L"- Plain FTP sends the password unencrypted; use it for public or test servers\n"
           L"- Enter: open a folder, or view a file; Backspace / `..`: parent folder (at the root: disconnect)\n"
           L"- F3: view the current file\n"
           L"- F5 / F6: download (copy / move) the selection to the other panel's folder\n"
           L"- F5 in the other panel: upload its selection into the current server folder\n"
           L"- F7: create a folder; F2: rename; F8 / Delete: delete (always asks; there is no Recycle Bin)\n"
           L"- Ctrl+R: re-read the folder; Esc: cancel a running transfer\n"
           L"- Insert, Shift+Up/Down, Num+ / Num- / Num*, Ctrl+Num+ / Ctrl+Num-, right-click: selection\n"
           L"- Saved passwords are kept only in the Windows Credential Manager, never in a file or the log;\n"
           L"  every password prompt offers \"Save the password (Windows Credential Manager)\"\n";
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
