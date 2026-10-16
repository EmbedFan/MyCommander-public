#pragma once

// EXT-004 (D2) / EXT-002 (D4): the remote-server code shared by the FTP and
// SFTP plugins -- the connection manager, the login prompt (masked, with the
// Credential Manager opt-in on every prompt), the Disconnect menu item, and
// the connected server as a file-system provider (McFileSystem.h, API 1.12).
// The HOST shows the server in its own panel: browsing, sorting, the filter,
// selection, F3/F5/F6/F7/F8/F2, conflicts, progress, Esc and pause are the
// host's. This code only answers the provider calls through a remote::Client:
//   - listing (with a short-lived cache for the host's per-file Stat calls),
//     offering to reconnect when the connection dropped (FTP-009/SFTP-008);
//   - whole-file DownloadToFile / UploadFromFile (the client's own transfers),
//     an upload going to "<name><partSuffix>" first and renamed into place
//     only when complete, so a failure or Esc never leaves a partial file or
//     destroys the existing one (FTP-003/SFTP-003);
//   - mkdir / delete (a link is removed itself, never followed) / rename.
// Everything that differs per protocol comes from a PanelProtocol. Compiled
// into each plugin DLL separately: its state is per plugin.
//
// winsock2.h users must include it before this header (it includes windows.h).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../MyCommanderPluginSdk/McPluginAbi.h"
#include "../MyCommanderPluginSdk/McPluginHost.h"
#include "../MyCommanderPluginSdk/McPluginTypes.h"
#include "RemoteClient.h"
#include "RemoteConnections.h"

#include <functional>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace remote {

struct PanelProtocol {
    std::wstring name;        // "FTP" / "SFTP": titles, messages, log prefix
    std::wstring scheme;      // "ftp" / "sftp": the panel header
    std::wstring partSuffix;  // ".mcftp-part": the temporary upload name
    const StoreSpec* store = nullptr;
    uint32_t connectionsCommandId = 0;    // Network > "<name> connection"
    uint32_t newConnectionCommandId = 0;  // Network > "New <name> connection"
    uint32_t disconnectCommandId = 0;     // Network > "Disconnect <name>" (shown while connected)
    std::function<std::unique_ptr<Client>()> makeClient;
    // The connection form (protocol-specific fields). `secret` in/out;
    // `saveConnection` out. False on Esc or an unusable entry.
    std::function<bool(const std::wstring& title, Connection& c, std::wstring& secret, bool& saveConnection)>
        editConnection;
    // Whether logging in needs a password/passphrase the user must type.
    std::function<bool(const Connection& c)> needsSecret;
    // The login prompt's field label ("Password for bob").
    std::function<std::wstring(const Connection& c)> secretLabel;
    std::wstring loginTitleNote;  // appended to the login prompt's title
};

// One connected server, shown by the host in one panel. It is the provider's
// `context`; the host's Close frees it.
struct ServerSession {
    McPanelSide side = kMcPanelLeft;
    Connection connection;
    std::wstring password;  // memory only, for reconnecting this session
    std::unique_ptr<Client> client;
    std::wstring displayRoot;  // "sftp://bob@host:2222"
    // The last listing, for the host's Stat calls during a copy; updated in
    // place by this session's own changes, re-read after kCacheMs.
    std::wstring cachedDir;
    std::vector<RemoteEntry> cached;
    ULONGLONG cachedAt = 0;
    bool cacheValid = false;
    std::set<std::wstring> pendingParts;  // unfinished uploads to remove after a reconnect / on close
};

// McPlugin_Init / McPlugin_Shutdown. The plugin registers its own menu items
// (labels and mnemonics are its own), then calls UpdateDisconnectItem().
void PanelInit(const McPluginHost* host, PanelProtocol protocol);
void PanelShutdown();
void UpdateDisconnectItem();

// The plugin's McPlugin_OnMenuAction forwards here.
void OnMenuAction(uint32_t commandId);

// Host helpers, also for the protocol's own dialogs (connection form, SSH
// host-key prompt).
const McPluginHost* Host();
void Log(McLogLevel level, const std::wstring& message);  // "<name>: " prefixed
void Message(const std::wstring& title, const std::vector<std::wstring>& lines);
int Choose(const std::wstring& title, const std::vector<std::wstring>& lines, const std::vector<McDialogOption>& options);
bool Cancelled();

// The server this plugin shows in that panel, or null.
ServerSession* SessionFor(McPanelSide side);
// The provider table of a session (exposed for the plugin harnesses).
McFsProvider MakeProviderTable(ServerSession& session);
// Every open session (the harnesses check that Close freed them).
size_t SessionCount();

}  // namespace remote
