#include "NetInfoModel.h"

#include "../MyCommander/TextCase.h"
#include "../MyCommander/TextWidth.h"

#include <algorithm>

namespace netinfo {

namespace {

bool IsSelectable(const Row& r) { return r.selectable; }

std::wstring HostOf(const Machine& m) { return m.host.empty() ? m.name : m.host; }

bool HasName(const Machine& m) { return !m.name.empty() && m.name != m.address; }

std::wstring StateText(LoadState s) {
    switch (s) {
        case LoadState::NotLoaded: return L"not read yet";
        case LoadState::Loading: return L"reading...";
        case LoadState::Loaded: return L"read";
        case LoadState::Failed: return L"could not be read";
    }
    return L"";
}

Action EnterAction(const std::vector<Row>& rows, int index) {
    if (index < 0 || index >= static_cast<int>(rows.size())) return {};
    switch (rows[static_cast<size_t>(index)].kind) {
        case RowKind::Machine: return {ActionKind::ToggleMachine, index};
        case RowKind::MachineShare:
        case RowKind::LocalShare:
        case RowKind::MappedDrive: return {ActionKind::OpenShare, index};
        case RowKind::Wifi: return {ActionKind::Details, index};
        default: return {};
    }
}

int LastSelectable(const std::vector<Row>& rows) {
    for (int i = static_cast<int>(rows.size()) - 1; i >= 0; --i)
        if (IsSelectable(rows[static_cast<size_t>(i)])) return i;
    return 0;
}

int NextSelectable(const std::vector<Row>& rows, int from, int step) {
    for (int i = from + step; i >= 0 && i < static_cast<int>(rows.size()); i += step)
        if (IsSelectable(rows[static_cast<size_t>(i)])) return i;
    return -1;
}

}  // namespace

std::wstring LowerKey(const std::wstring& text) { return mc::ToLowerInvariant(text); }

std::wstring SignalBar(int quality) {
    int n = std::clamp((quality + 12) / 25, 0, 4);
    return L"[" + std::wstring(static_cast<size_t>(n), L'#') + std::wstring(static_cast<size_t>(4 - n), L'.') + L"]";
}

std::wstring SecurityName(unsigned authAlgorithm, bool secured) {
    switch (authAlgorithm) {
        case 1: return secured ? L"Secured" : L"Open";
        case 2: return L"WEP";
        case 3: return L"WPA-Enterprise";
        case 4: return L"WPA-Personal";
        case 6: return L"WPA2-Enterprise";
        case 7: return L"WPA2-Personal";
        case 8: return L"WPA3-Enterprise 192";
        case 9: return L"WPA3-Personal";
        case 10: return L"OWE";
        case 11: return L"WPA3-Enterprise";
        default: return secured ? L"Secured" : L"Open";
    }
}

std::wstring ShareTypeName(ShareType type) {
    switch (type) {
        case ShareType::Disk: return L"Folder";
        case ShareType::Printer: return L"Printer";
        case ShareType::Device: return L"Device";
        case ShareType::Ipc: return L"IPC";
        case ShareType::Other: return L"Other";
    }
    return L"";
}

std::wstring WifiStatusText(WifiStatus status) {
    switch (status) {
        case WifiStatus::NotScanned: return L"not scanned yet";
        case WifiStatus::Scanning: return L"scanning...";
        case WifiStatus::Ok: return L"";
        case WifiStatus::NoAdapter: return L"No Wi-Fi adapter";
        case WifiStatus::ServiceStopped: return L"The WLAN service is not running";
        case WifiStatus::RadioOff: return L"Wi-Fi is turned off";
        case WifiStatus::LocationBlocked:
            return L"Windows hides Wi-Fi names until Location access for desktop apps is on "
                   L"(Settings > Privacy & security > Location)";
        case WifiStatus::NoNetworksFound:
            return L"No networks found (if you expect some, check that Location access for desktop apps is on)";
        case WifiStatus::Failed: return L"Wi-Fi could not be read";
    }
    return L"";
}

void SortWifi(std::vector<WifiNetwork>& networks) {
    std::stable_sort(networks.begin(), networks.end(), [](const WifiNetwork& a, const WifiNetwork& b) {
        if (a.connected != b.connected) return a.connected;
        if (a.signal != b.signal) return a.signal > b.signal;
        return LowerKey(a.ssid) < LowerKey(b.ssid);
    });
}

void SortMachines(std::vector<Machine>& machines) {
    std::stable_sort(machines.begin(), machines.end(), [](const Machine& a, const Machine& b) {
        const bool an = HasName(a), bn = HasName(b);
        if (an != bn) return an;
        if (an) return LowerKey(a.name) < LowerKey(b.name);
        return a.address < b.address;
    });
}

std::wstring ShareOpenPath(const ShareEntry& share, const Machine* machine) {
    if (share.type != ShareType::Disk) return L"";
    if (machine) {
        const std::wstring host = HostOf(*machine);
        if (host.empty() || share.name.empty()) return L"";
        return L"\\\\" + host + L"\\" + share.name;
    }
    return share.path;
}

std::vector<Row> BuildRows(const Snapshot& s, const BuildOptions& options) {
    std::vector<Row> rows;
    auto add = [&](RowKind kind, std::wstring text, std::wstring detail, bool selectable, int index = -1,
                   int share = -1) -> Row& {
        Row r;
        r.kind = kind;
        r.text = std::move(text);
        r.detail = std::move(detail);
        r.selectable = selectable;
        r.index = index;
        r.share = share;
        rows.push_back(std::move(r));
        return rows.back();
    };

    // --- This computer ---
    add(RowKind::Heading, L"This computer", s.computerName, false);
    if (!s.localLoaded) {
        add(RowKind::Text, L"  Reading...", L"", false);
    } else {
        int visibleShares = 0;
        for (size_t i = 0; i < s.localShares.size(); ++i) {
            const ShareEntry& sh = s.localShares[i];
            if (sh.special && !options.showSpecialShares) continue;
            ++visibleShares;
            std::wstring detail = ShareTypeName(sh.type);
            const std::wstring where = !sh.path.empty() ? sh.path : sh.remark;
            if (!where.empty()) detail += L"  " + where;
            add(RowKind::LocalShare, L"  " + sh.name, detail, true, static_cast<int>(i));
        }
        if (visibleShares == 0) add(RowKind::Text, L"  (no shared folders)", L"", false);
        for (size_t i = 0; i < s.mapped.size(); ++i)
            add(RowKind::MappedDrive, L"  " + s.mapped[i].local, s.mapped[i].remote, true, static_cast<int>(i));
    }

    // --- Wi-Fi ---
    add(RowKind::Heading, L"Wi-Fi",
        s.wifiStatus == WifiStatus::Scanning ? L"scanning..."
                                             : (s.wifi.empty() ? L"" : std::to_wstring(s.wifi.size()) + L" networks"),
        false);
    if (s.wifi.empty()) {
        const std::wstring text = WifiStatusText(s.wifiStatus);
        if (!text.empty()) {
            Row& r = add(RowKind::Text, L"  " + text, L"", false);
            r.warn = s.wifiStatus == WifiStatus::LocationBlocked || s.wifiStatus == WifiStatus::Failed ||
                     s.wifiStatus == WifiStatus::RadioOff || s.wifiStatus == WifiStatus::ServiceStopped;
        }
    }
    for (size_t i = 0; i < s.wifi.size(); ++i) {
        const WifiNetwork& n = s.wifi[i];
        std::wstring name = n.ssid.empty() ? L"(hidden network)" : n.ssid;
        std::wstring detail = n.connected ? L"connected  " : L"";
        detail += SignalBar(n.signal) + L" " + n.security;
        Row& r = add(RowKind::Wifi, std::wstring(n.connected ? L"  * " : L"    ") + name, detail, true,
                     static_cast<int>(i));
        r.connected = n.connected;
    }

    // --- Network ---
    add(RowKind::Heading, L"Network",
        s.machinesScanning ? L"scanning..."
                           : (s.machinesDone ? std::to_wstring(s.machines.size()) + L" machines" : L""),
        false);
    if (s.machines.empty()) {
        if (s.machinesScanning) add(RowKind::Text, L"  Searching...", L"", false);
        else if (s.machinesDone) add(RowKind::Text, L"  No machines found", L"", false);
        if (!s.machinesNote.empty()) add(RowKind::Text, L"  " + s.machinesNote, L"", false);
    }
    for (size_t i = 0; i < s.machines.size(); ++i) {
        const Machine& m = s.machines[i];
        const bool open = options.expanded.count(LowerKey(HostOf(m))) != 0;
        add(RowKind::Machine, std::wstring(open ? L"  [-] " : L"  [+] ") + m.name,
            m.address == m.name ? L"" : m.address, true, static_cast<int>(i))
            .key = LowerKey(HostOf(m));
        if (!open) continue;
        switch (m.shares) {
            case LoadState::NotLoaded:
            case LoadState::Loading:
                add(RowKind::MachineNote, L"        Reading shares...", L"", false, static_cast<int>(i));
                break;
            case LoadState::Failed: {
                Row& r = add(RowKind::MachineNote,
                             L"        " + (m.error.empty() ? std::wstring(L"The shares could not be read") : m.error), L"",
                             false, static_cast<int>(i));
                r.warn = true;
                break;
            }
            case LoadState::Loaded: {
                int shown = 0;
                for (size_t k = 0; k < m.shareList.size(); ++k) {
                    const ShareEntry& sh = m.shareList[k];
                    if (sh.special && !options.showSpecialShares) continue;
                    ++shown;
                    std::wstring detail = ShareTypeName(sh.type);
                    if (!sh.remark.empty()) detail += L"  " + sh.remark;
                    add(RowKind::MachineShare, L"        " + sh.name, detail, true, static_cast<int>(i),
                        static_cast<int>(k));
                }
                if (shown == 0) add(RowKind::MachineNote, L"        (no shares)", L"", false, static_cast<int>(i));
                break;
            }
        }
    }
    return rows;
}

int NearestSelectable(const std::vector<Row>& rows, int from) {
    if (rows.empty()) return 0;
    from = std::clamp(from, 0, static_cast<int>(rows.size()) - 1);
    if (IsSelectable(rows[static_cast<size_t>(from)])) return from;
    int next = NextSelectable(rows, from, +1);
    if (next >= 0) return next;
    int prev = NextSelectable(rows, from, -1);
    return prev >= 0 ? prev : 0;
}

int RestoreCursor(const std::vector<Row>& oldRows, int oldCursor, const std::vector<Row>& newRows) {
    if (oldCursor >= 0 && oldCursor < static_cast<int>(oldRows.size())) {
        const Row& old = oldRows[static_cast<size_t>(oldCursor)];
        for (size_t i = 0; i < newRows.size(); ++i) {
            const Row& r = newRows[i];
            if (r.selectable && r.kind == old.kind && r.text == old.text && r.index == old.index && r.share == old.share)
                return static_cast<int>(i);
        }
        for (size_t i = 0; i < newRows.size(); ++i) {  // same text, the index shifted
            const Row& r = newRows[i];
            if (r.selectable && r.kind == old.kind && r.text == old.text) return static_cast<int>(i);
        }
    }
    return NearestSelectable(newRows, oldCursor);
}

void EnsureVisible(const std::vector<Row>& rows, ViewState& state, int visibleRows) {
    if (visibleRows < 1) visibleRows = 1;
    const int n = static_cast<int>(rows.size());
    state.cursor = std::clamp(state.cursor, 0, std::max(0, n - 1));
    if (state.cursor < state.top) state.top = state.cursor;
    if (state.cursor >= state.top + visibleRows) state.top = state.cursor - visibleRows + 1;
    if (n > 0 && state.cursor == NearestSelectable(rows, 0)) state.top = 0;  // the first row: keep the headings above it
    state.top = std::clamp(state.top, 0, std::max(0, n - visibleRows));
}

Action HandleKey(const std::vector<Row>& rows, ViewState& state, uint16_t vk, int visibleRows) {
    if (visibleRows < 1) visibleRows = 1;
    const int n = static_cast<int>(rows.size());
    auto moveTo = [&](int target) {
        if (target >= 0 && target < n) state.cursor = target;
        EnsureVisible(rows, state, visibleRows);
    };
    state.cursor = NearestSelectable(rows, state.cursor);
    switch (vk) {
        case VK_ESCAPE: return {ActionKind::Close, -1};
        case VK_F5: return {ActionKind::Rescan, -1};
        case VK_UP: moveTo(NextSelectable(rows, state.cursor, -1)); return {};
        case VK_DOWN: moveTo(NextSelectable(rows, state.cursor, +1)); return {};
        case VK_PRIOR: {
            int target = NearestSelectable(rows, state.cursor - visibleRows);
            if (target > state.cursor) target = NearestSelectable(rows, 0);
            moveTo(target);
            return {};
        }
        case VK_NEXT: {
            int target = std::min(state.cursor + visibleRows, n - 1);
            int s = NearestSelectable(rows, target);
            if (s < state.cursor) s = LastSelectable(rows);
            moveTo(s);
            return {};
        }
        case VK_HOME: moveTo(NearestSelectable(rows, 0)); return {};
        case VK_END: moveTo(LastSelectable(rows)); return {};
        case VK_RETURN: return EnterAction(rows, state.cursor);
        case VK_SPACE:
        case VK_F3:
            if (state.cursor >= 0 && state.cursor < n && rows[static_cast<size_t>(state.cursor)].selectable)
                return {ActionKind::Details, state.cursor};
            return {};
        case VK_RIGHT: {
            if (state.cursor >= n) return {};
            const Row& r = rows[static_cast<size_t>(state.cursor)];
            if (r.kind != RowKind::Machine) return {};
            if (state.expanded.count(r.key) == 0) return {ActionKind::ToggleMachine, state.cursor};  // expand
            moveTo(NextSelectable(rows, state.cursor, +1));  // already open: step into its shares
            return {};
        }
        case VK_LEFT: {
            if (state.cursor >= n) return {};
            const Row& r = rows[static_cast<size_t>(state.cursor)];
            if (r.kind == RowKind::MachineShare) {  // back to the machine row
                for (int i = state.cursor - 1; i >= 0; --i)
                    if (rows[static_cast<size_t>(i)].kind == RowKind::Machine) {
                        moveTo(i);
                        break;
                    }
                return {};
            }
            if (r.kind == RowKind::Machine && state.expanded.count(r.key) != 0)
                return {ActionKind::ToggleMachine, state.cursor};  // collapse
            return {};
        }
        default: return {};
    }
}

Action HandleClick(const std::vector<Row>& rows, ViewState& state, int relativeY, bool doubleClick, int visibleRows) {
    if (relativeY < 0 || relativeY >= visibleRows) return {};
    const int index = state.top + relativeY;
    if (index < 0 || index >= static_cast<int>(rows.size())) return {};
    if (!rows[static_cast<size_t>(index)].selectable) return {};
    state.cursor = index;
    EnsureVisible(rows, state, visibleRows);
    return doubleClick ? EnterAction(rows, index) : Action{};
}

std::vector<std::wstring> DetailLines(const Snapshot& s, const Row& row) {
    std::vector<std::wstring> out;
    auto machineOf = [&](int i) -> const Machine* {
        return i >= 0 && i < static_cast<int>(s.machines.size()) ? &s.machines[static_cast<size_t>(i)] : nullptr;
    };
    switch (row.kind) {
        case RowKind::Wifi:
            if (row.index >= 0 && row.index < static_cast<int>(s.wifi.size())) {
                const WifiNetwork& n = s.wifi[static_cast<size_t>(row.index)];
                out.push_back(L"Network: " + (n.ssid.empty() ? std::wstring(L"(hidden network)") : n.ssid));
                out.push_back(L"Signal: " + std::to_wstring(n.signal) + L" %  " + SignalBar(n.signal));
                out.push_back(L"Security: " + n.security);
                out.push_back(n.connected ? L"This computer is connected to it" : L"Not connected");
            }
            break;
        case RowKind::LocalShare:
            if (row.index >= 0 && row.index < static_cast<int>(s.localShares.size())) {
                const ShareEntry& sh = s.localShares[static_cast<size_t>(row.index)];
                out.push_back(L"Share: " + sh.name);
                out.push_back(L"Type: " + ShareTypeName(sh.type));
                if (!sh.path.empty()) out.push_back(L"Folder: " + sh.path);
                if (!sh.remark.empty()) out.push_back(L"Remark: " + sh.remark);
            }
            break;
        case RowKind::MappedDrive:
            if (row.index >= 0 && row.index < static_cast<int>(s.mapped.size())) {
                out.push_back(L"Drive: " + s.mapped[static_cast<size_t>(row.index)].local);
                out.push_back(L"Connected to: " + s.mapped[static_cast<size_t>(row.index)].remote);
            }
            break;
        case RowKind::Machine:
            if (const Machine* m = machineOf(row.index)) {
                out.push_back(L"Machine: " + m->name);
                if (!m->host.empty() && m->host != m->name) out.push_back(L"Host name: " + m->host);
                if (!m->address.empty()) out.push_back(L"Address: " + m->address);
                std::wstring found;
                if (m->viaBrowse) found = L"network browsing";
                if (m->viaNeighbour) found += (found.empty() ? L"" : L", ") + std::wstring(L"recently contacted");
                if (!found.empty()) out.push_back(L"Found by: " + found);
                out.push_back(L"Shares: " + StateText(m->shares));
            }
            break;
        case RowKind::MachineShare:
            if (const Machine* m = machineOf(row.index)) {
                if (row.share >= 0 && row.share < static_cast<int>(m->shareList.size())) {
                    const ShareEntry& sh = m->shareList[static_cast<size_t>(row.share)];
                    out.push_back(L"Share: " + sh.name);
                    out.push_back(L"Machine: " + m->name);
                    out.push_back(L"Type: " + ShareTypeName(sh.type));
                    const std::wstring path = ShareOpenPath(sh, m);
                    if (!path.empty()) out.push_back(L"Path: " + path);
                    if (!sh.remark.empty()) out.push_back(L"Remark: " + sh.remark);
                }
            }
            break;
        default: break;
    }
    return out;
}

std::wstring DefaultStatusText(const Snapshot& s) {
    std::wstring scanning;
    if (s.wifiStatus == WifiStatus::Scanning) scanning = L"Wi-Fi";
    if (s.machinesScanning) scanning += (scanning.empty() ? L"" : L", ") + std::wstring(L"machines");
    if (!scanning.empty()) return L"Scanning: " + scanning + L"   Esc close";
    return L"F5 rescan   Enter open/expand   Space details   Esc close";
}

std::vector<DrawLine> RenderPanel(const std::vector<Row>& rows, const ViewState& state, int width, int height,
                                  bool active, const std::wstring& status) {
    std::vector<DrawLine> out;
    if (width < 1 || height < 1) return out;
    const size_t W = static_cast<size_t>(width);
    const int visible = height - 1;
    for (int i = 0; i < visible; ++i) {
        DrawLine line;
        line.y = i;
        const int index = state.top + i;
        if (index >= static_cast<int>(rows.size())) {
            line.text = std::wstring(W, L' ');
            out.push_back(std::move(line));
            continue;
        }
        const Row& r = rows[static_cast<size_t>(index)];
        std::wstring detail = r.detail;
        const int detailW = mc::StringDisplayWidth(detail);
        const int cap = std::max(1, width / 2);
        if (detail.empty()) {
            line.text = mc::PadToDisplayWidth(r.text, W);
        } else {
            if (detailW > cap) detail = mc::PadToDisplayWidth(detail, static_cast<size_t>(cap));
            const int dw = std::min(detailW, cap);
            const size_t leftW = width - dw - 1 >= 1 ? static_cast<size_t>(width - dw - 1) : 1;
            line.text = mc::PadToDisplayWidth(mc::PadToDisplayWidth(r.text, leftW) + L" " + detail, W);
        }
        if (r.kind == RowKind::Heading) line.color = DrawColor::Section;
        else if (index == state.cursor && r.selectable) line.color = active ? DrawColor::Highlight : DrawColor::Selected;
        else if (r.warn) line.color = DrawColor::Warning;
        else if (r.connected) line.color = DrawColor::Selected;
        out.push_back(std::move(line));
    }
    DrawLine st;
    st.y = height - 1;
    st.text = mc::PadToDisplayWidth(L" " + status, W);
    st.color = DrawColor::Normal;
    out.push_back(std::move(st));
    return out;
}

}  // namespace netinfo
