#pragma once

// FEA-0010 (Network Info plugin): the pure part. Data types for what the panel shows, the rows built from
// them, the cursor / key / click rules and the drawing as plain lines. No Win32 network call, no threads,
// no plugin-host call: NetInfoModelTests.cpp drives all of it with hand-built snapshots.

#include <cstdint>
#include <set>
#include <string>
#include <vector>

namespace netinfo {

enum class ShareType { Disk, Printer, Device, Ipc, Other };

struct ShareEntry {
    std::wstring name;
    ShareType type = ShareType::Disk;
    std::wstring remark;
    std::wstring path;     // the folder on the machine it is on (local shares only)
    bool special = false;  // administrative: C$, ADMIN$, IPC$ ...
};

struct MappedDrive {
    std::wstring local;   // "Z:"
    std::wstring remote;  // "\server\share"
};

enum class LoadState { NotLoaded, Loading, Loaded, Failed };

struct Machine {
    std::wstring name;     // short display name (or the address when no name is known)
    std::wstring host;     // what goes after the backslashes of a UNC path
    std::wstring address;  // "192.168.1.20" (may be empty for a machine found by browsing)
    bool viaBrowse = false;
    bool viaNeighbour = false;
    LoadState shares = LoadState::NotLoaded;
    std::vector<ShareEntry> shareList;
    std::wstring error;  // why the share list could not be read
};

enum class WifiStatus { NotScanned, Scanning, Ok, NoAdapter, ServiceStopped, RadioOff, LocationBlocked, NoNetworksFound, Failed };

struct WifiNetwork {
    std::wstring ssid;
    int signal = 0;  // 0..100, Windows' own quality value
    std::wstring security;
    bool secured = false;
    bool connected = false;
};

// Everything the panel needs to draw, copied out of the scanner under its lock.
struct Snapshot {
    std::wstring computerName;
    std::vector<ShareEntry> localShares;
    std::vector<MappedDrive> mapped;
    bool localLoaded = false;

    WifiStatus wifiStatus = WifiStatus::NotScanned;
    std::wstring wifiDetail;
    std::vector<WifiNetwork> wifi;

    bool machinesScanning = false;
    bool machinesDone = false;
    std::wstring machinesNote;
    std::vector<Machine> machines;
};

// --- text helpers (pure) ---------------------------------------------------------------------------------

// "[##..]" for Windows' 0..100 signal quality.
std::wstring SignalBar(int quality);
// DOT11_AUTH_ALGORITHM value -> "WPA2-Personal" ...; `secured` false and an unknown value gives "Open".
std::wstring SecurityName(unsigned authAlgorithm, bool secured);
std::wstring ShareTypeName(ShareType type);
std::wstring WifiStatusText(WifiStatus status);
// Connected first, then the stronger signal, then the name (case-insensitive).
void SortWifi(std::vector<WifiNetwork>& networks);
// By display name, case-insensitive; a machine without a name sorts by its address, after the named ones.
void SortMachines(std::vector<Machine>& machines);
// The folder a share opens as: "\host\name" for a remote disk share, the real folder for a local one.
// Empty when the share is not a folder share.
std::wstring ShareOpenPath(const ShareEntry& share, const Machine* machine);
std::wstring LowerKey(const std::wstring& text);

// --- rows -------------------------------------------------------------------------------------------------

enum class RowKind { Heading, Text, LocalShare, MappedDrive, Wifi, Machine, MachineShare, MachineNote };

struct Row {
    RowKind kind = RowKind::Text;
    std::wstring text;    // left part
    std::wstring detail;  // right-aligned part
    bool selectable = false;
    bool connected = false;  // the Wi-Fi network this computer is on
    bool warn = false;       // drawn in the warning colour
    int index = -1;          // local share / mapped drive / wifi / machine index
    int share = -1;          // machine share index (MachineShare rows)
    std::wstring key;        // Machine rows: LowerKey(host), the expanded-set key
};

struct BuildOptions {
    bool showSpecialShares = false;
    std::set<std::wstring> expanded;  // LowerKey(machine.host) of the expanded machines
};

std::vector<Row> BuildRows(const Snapshot& snapshot, const BuildOptions& options);

// --- view state, keys, clicks ------------------------------------------------------------------------------

struct ViewState {
    int cursor = 0;
    int top = 0;
    std::set<std::wstring> expanded;
};

enum class ActionKind { None, Close, Rescan, ToggleMachine, OpenShare, Details };

struct Action {
    ActionKind kind = ActionKind::None;
    int rowIndex = -1;  // into the rows the key was handled against
};

// Moves the cursor to the first selectable row at or after `from` (or before it when none follows).
int NearestSelectable(const std::vector<Row>& rows, int from);
// Re-places the cursor after the rows changed under it (a rescan): the row with the same kind, text and index
// when it still exists, else the nearest selectable row.
int RestoreCursor(const std::vector<Row>& oldRows, int oldCursor, const std::vector<Row>& newRows);
// Scrolls `top` just enough for `cursor` (and, when it is the first selectable row, its heading) to be visible.
void EnsureVisible(const std::vector<Row>& rows, ViewState& state, int visibleRows);

// `visibleRows` is the number of list rows on screen (the status row is not counted).
Action HandleKey(const std::vector<Row>& rows, ViewState& state, uint16_t virtualKey, int visibleRows);
// `relativeY` is the row inside the content area. A click outside the list does nothing.
Action HandleClick(const std::vector<Row>& rows, ViewState& state, int relativeY, bool doubleClick, int visibleRows);

// What a row's Enter/Space "details" box shows.
std::vector<std::wstring> DetailLines(const Snapshot& snapshot, const Row& row);

// --- drawing ----------------------------------------------------------------------------------------------

enum class DrawColor { Normal, Highlight, Section, Warning, Selected };

struct DrawLine {
    int y = 0;
    std::wstring text;  // exactly `width` columns wide
    DrawColor color = DrawColor::Normal;
};

// The list rows (from state.top) followed by the status row at height - 1. Each line is padded or trimmed to
// `width` display columns (TextWidth.h). `active` decides how the cursor row looks.
std::vector<DrawLine> RenderPanel(const std::vector<Row>& rows, const ViewState& state, int width, int height,
                                  bool active, const std::wstring& status);

// The status row's text while nothing special is going on.
std::wstring DefaultStatusText(const Snapshot& snapshot);

}  // namespace netinfo
