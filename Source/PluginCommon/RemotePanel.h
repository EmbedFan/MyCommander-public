#pragma once

// EXT-004 (D2): the remote-server panel shared by the FTP and SFTP plugins --
// connection manager, login prompt (masked, with the Credential Manager
// opt-in on every prompt), the server shown in a panel (browse, select, F3,
// F5/F6 download, upload via the other panel's F5, F7/F8/F2/Ctrl+R),
// conflicts, download-to-temp-then-replace, reconnect, Disconnect menu item,
// progress and Esc. Extracted from the FTP plugin (EXT-005); everything that
// differs per protocol comes from a PanelProtocol. Compiled into each plugin
// DLL separately: its state (the host, the two panel views) is per plugin.
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
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace remote {

struct PanelProtocol {
    std::wstring name;        // "FTP" / "SFTP": titles, messages, log prefix
    std::wstring scheme;      // "ftp" / "sftp": the panel header
    std::wstring partSuffix;  // ".mcftp-part": the temporary download name
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

// One connected server shown in one panel.
struct ServerView {
    bool open = false;
    McPanelSide side = kMcPanelLeft;
    Connection connection;
    std::wstring password;  // memory only, for reconnecting this session
    std::unique_ptr<Client> client;
    std::wstring path = L"/";
    std::vector<RemoteEntry> entries;  // sorted, without ".."
    std::set<std::wstring> selected;
    int cursor = 0;
    int topIndex = 0;
    int visibleRows = 10;
    bool mouseSelecting = false;
    bool mouseSelectValue = false;
};

// McPlugin_Init / McPlugin_Shutdown. The plugin registers its own menu items
// (labels and mnemonics are its own), then calls UpdateDisconnectItem().
void PanelInit(const McPluginHost* host, PanelProtocol protocol);
void PanelShutdown();
void UpdateDisconnectItem();

// The plugin exports forward here.
void OnMenuAction(uint32_t commandId);
bool OnReceiveFiles(McPanelSide side, McPanelSide sourceSide);
void Draw(McPanelSide side, uint16_t width, uint16_t height, bool active);
void HandleKey(McPanelSide side, uint16_t key, uint32_t control);
void HandleMouse(McPanelSide side, int16_t x, int16_t y, uint32_t buttons, uint32_t flags, uint32_t control);

// Host helpers, also for the protocol's own dialogs (connection form, SSH
// host-key prompt).
const McPluginHost* Host();
void Log(McLogLevel level, const std::wstring& message);  // "<name>: " prefixed
void Message(const std::wstring& title, const std::vector<std::wstring>& lines);
int Choose(const std::wstring& title, const std::vector<std::wstring>& lines, const std::vector<McDialogOption>& options);
bool Cancelled();

ServerView* ViewFor(McPanelSide side);  // the open view in that panel, or null

}  // namespace remote
