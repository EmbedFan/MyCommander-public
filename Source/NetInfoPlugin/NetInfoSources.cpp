#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include <iphlpapi.h>
#include <lm.h>
#include <netioapi.h>
#include <winnetwk.h>
#include <wlanapi.h>

#include "NetInfoSources.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <map>
#include <mutex>
#include <set>
#include "DetachedThread.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "netapi32.lib")
#pragma comment(lib, "mpr.lib")

namespace netinfo {

std::wstring ShortHostName(const std::wstring& host) {
    if (host.empty() || host.find(L':') != std::wstring::npos) return host;  // an IPv6 literal
    bool digitsAndDots = true;
    for (wchar_t c : host)
        if (!(c == L'.' || (c >= L'0' && c <= L'9'))) digitsAndDots = false;
    if (digitsAndDots) return host;  // an IPv4 address
    const size_t dot = host.find(L'.');
    return dot == std::wstring::npos || dot == 0 ? host : host.substr(0, dot);
}

std::wstring ShareErrorText(unsigned long error) {
    switch (error) {
        case ERROR_ACCESS_DENIED: return L"Access denied";
        case ERROR_BAD_NETPATH:
        case ERROR_NETWORK_UNREACHABLE:
        case ERROR_HOST_UNREACHABLE:
        case ERROR_NETWORK_BUSY:
        case ERROR_BAD_NET_NAME:
        case ERROR_REM_NOT_LIST:
        case ERROR_UNEXP_NET_ERR:
        case ERROR_NO_NETWORK:
        case 1722 /* RPC_S_SERVER_UNAVAILABLE */: return L"The machine could not be reached";
        case 2114 /* NERR_ServerNotStarted */: return L"File sharing is not available on it";
        case ERROR_LOGON_FAILURE:
        case ERROR_SESSION_CREDENTIAL_CONFLICT: return L"Sign-in refused";
        default: break;
    }
    wchar_t* text = nullptr;
    DWORD n = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS,
                             nullptr, error, 0, reinterpret_cast<LPWSTR>(&text), 0, nullptr);
    std::wstring result;
    if (n > 0 && text) {
        result.assign(text, n);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n' || result.back() == L' '))
            result.pop_back();
    }
    if (text) LocalFree(text);
    if (result.empty()) result = L"Error " + std::to_wstring(error);
    return result;
}

namespace {

ShareType TypeOf(DWORD shareType) {
    switch (shareType & STYPE_MASK) {
        case STYPE_DISKTREE: return ShareType::Disk;
        case STYPE_PRINTQ: return ShareType::Printer;
        case STYPE_DEVICE: return ShareType::Device;
        case STYPE_IPC: return ShareType::Ipc;
        default: return ShareType::Other;
    }
}

std::wstring Utf8ToWide(const char* bytes, size_t length) {
    if (length == 0) return L"";
    UINT cp = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    int n = MultiByteToWideChar(cp, flags, bytes, static_cast<int>(length), nullptr, 0);
    if (n <= 0) {  // not valid UTF-8: take it as the ANSI code page
        cp = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(cp, flags, bytes, static_cast<int>(length), nullptr, 0);
    }
    if (n <= 0) return L"";
    std::wstring out(static_cast<size_t>(n), L'\0');
    MultiByteToWideChar(cp, flags, bytes, static_cast<int>(length), &out[0], n);
    return out;
}

bool SameText(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// ---- shares ----------------------------------------------------------------------------------------------

// Level 2 (paths) for this computer works unelevated; level 1 for another machine.
bool EnumerateShares(const std::wstring& server, bool withPaths, std::vector<ShareEntry>& out, DWORD& error) {
    PBYTE buffer = nullptr;
    DWORD read = 0, total = 0, resume = 0;
    std::wstring name = server.empty() ? L"" : L"\\\\" + server;
    NET_API_STATUS status = NetShareEnum(server.empty() ? nullptr : const_cast<LPWSTR>(name.c_str()), withPaths ? 2 : 1,
                                         &buffer, MAX_PREFERRED_LENGTH, &read, &total, &resume);
    if (status != NERR_Success && status != ERROR_MORE_DATA) {
        error = static_cast<DWORD>(status);
        if (buffer) NetApiBufferFree(buffer);
        return false;
    }
    for (DWORD i = 0; i < read; ++i) {
        ShareEntry e;
        if (withPaths) {
            const auto& s = reinterpret_cast<SHARE_INFO_2*>(buffer)[i];
            e.name = s.shi2_netname ? s.shi2_netname : L"";
            e.remark = s.shi2_remark ? s.shi2_remark : L"";
            e.path = s.shi2_path ? s.shi2_path : L"";
            e.type = TypeOf(s.shi2_type);
            e.special = (s.shi2_type & STYPE_SPECIAL) != 0;
        } else {
            const auto& s = reinterpret_cast<SHARE_INFO_1*>(buffer)[i];
            e.name = s.shi1_netname ? s.shi1_netname : L"";
            e.remark = s.shi1_remark ? s.shi1_remark : L"";
            e.type = TypeOf(s.shi1_type);
            e.special = (s.shi1_type & STYPE_SPECIAL) != 0;
        }
        out.push_back(std::move(e));
    }
    if (buffer) NetApiBufferFree(buffer);
    std::sort(out.begin(), out.end(), [](const ShareEntry& a, const ShareEntry& b) {
        if (a.special != b.special) return !a.special;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return true;
}

void EnumerateMappedDrives(std::vector<MappedDrive>& out) {
    HANDLE h = nullptr;
    if (WNetOpenEnumW(RESOURCE_CONNECTED, RESOURCETYPE_DISK, 0, nullptr, &h) != NO_ERROR) return;
    std::vector<BYTE> buffer(16 * 1024);
    for (;;) {
        DWORD count = static_cast<DWORD>(-1);
        DWORD size = static_cast<DWORD>(buffer.size());
        DWORD r = WNetEnumResourceW(h, &count, buffer.data(), &size);
        if (r == ERROR_MORE_DATA) {
            buffer.resize(size);
            continue;
        }
        if (r != NO_ERROR) break;
        const auto* items = reinterpret_cast<NETRESOURCEW*>(buffer.data());
        for (DWORD i = 0; i < count; ++i) {
            if (!items[i].lpLocalName || !items[i].lpRemoteName) continue;
            out.push_back({items[i].lpLocalName, items[i].lpRemoteName});
        }
    }
    WNetCloseEnum(h);
    std::sort(out.begin(), out.end(), [](const MappedDrive& a, const MappedDrive& b) { return a.local < b.local; });
}

// ---- machines --------------------------------------------------------------------------------------------

// Network browsing (the Computer Browser / SMB1 world): empty on a default Windows 10/11 machine, useful on
// networks where it still runs.
void BrowseGlobalNet(NETRESOURCEW* parent, int depth, std::vector<std::wstring>& names, const CancelFn& cancelled) {
    HANDLE h = nullptr;
    if (WNetOpenEnumW(RESOURCE_GLOBALNET, RESOURCETYPE_ANY, 0, parent, &h) != NO_ERROR) return;
    std::vector<BYTE> buffer(32 * 1024);
    for (;;) {
        if (cancelled()) break;
        DWORD count = static_cast<DWORD>(-1);
        DWORD size = static_cast<DWORD>(buffer.size());
        DWORD r = WNetEnumResourceW(h, &count, buffer.data(), &size);
        if (r == ERROR_MORE_DATA) {
            buffer.resize(size);
            continue;
        }
        if (r != NO_ERROR) break;
        auto* items = reinterpret_cast<NETRESOURCEW*>(buffer.data());
        for (DWORD i = 0; i < count && names.size() < 500; ++i) {
            if (cancelled()) break;
            if (items[i].dwDisplayType == RESOURCEDISPLAYTYPE_SERVER) {
                if (items[i].lpRemoteName) {
                    std::wstring n = items[i].lpRemoteName;
                    while (!n.empty() && n.front() == L'\\') n.erase(n.begin());
                    if (!n.empty()) names.push_back(n);
                }
            } else if ((items[i].dwUsage & RESOURCEUSAGE_CONTAINER) != 0 && depth < 3) {
                BrowseGlobalNet(&items[i], depth + 1, names, cancelled);
            }
        }
    }
    WNetCloseEnum(h);
}

void BrowseServers(std::vector<std::wstring>& names) {
    PBYTE buffer = nullptr;
    DWORD read = 0, total = 0, resume = 0;
    NET_API_STATUS status = NetServerEnum(nullptr, 100, &buffer, MAX_PREFERRED_LENGTH, &read, &total,
                                          SV_TYPE_WORKSTATION | SV_TYPE_SERVER, nullptr, &resume);
    if ((status == NERR_Success || status == ERROR_MORE_DATA) && buffer) {
        const auto* items = reinterpret_cast<SERVER_INFO_100*>(buffer);
        for (DWORD i = 0; i < read && names.size() < 500; ++i)
            if (items[i].sv100_name && *items[i].sv100_name) names.push_back(items[i].sv100_name);
    }
    if (buffer) NetApiBufferFree(buffer);
}

struct NeighbourAddress {
    SOCKADDR_INET address{};
    std::wstring text;
    bool v6 = false;
};

bool UsableAddress(const SOCKADDR_INET& a) {
    if (a.si_family == AF_INET) {
        const BYTE* b = reinterpret_cast<const BYTE*>(&a.Ipv4.sin_addr);
        if (b[0] == 0 || b[0] == 127 || b[0] >= 224) return false;  // unspecified, loopback, multicast, broadcast
        return true;
    }
    if (a.si_family == AF_INET6) {
        const BYTE* b = a.Ipv6.sin6_addr.u.Byte;
        if (b[0] == 0xff) return false;  // multicast
        bool zero = true;
        for (int i = 0; i < 16; ++i)
            if (b[i] != 0) zero = false;
        return !zero;
    }
    return false;
}

std::vector<NeighbourAddress> ReadNeighbours() {
    std::vector<NeighbourAddress> out;
    PMIB_IPNET_TABLE2 table = nullptr;
    if (GetIpNetTable2(AF_UNSPEC, &table) != NO_ERROR || !table) return out;
    for (ULONG i = 0; i < table->NumEntries; ++i) {
        const MIB_IPNET_ROW2& row = table->Table[i];
        if (row.State != NlnsReachable && row.State != NlnsStale && row.State != NlnsDelay && row.State != NlnsProbe)
            continue;
        if (!UsableAddress(row.Address)) continue;
        NeighbourAddress n;
        n.address = row.Address;
        n.v6 = row.Address.si_family == AF_INET6;
        wchar_t text[64] = {};
        if (n.v6) InetNtopW(AF_INET6, &row.Address.Ipv6.sin6_addr, text, 64);
        else InetNtopW(AF_INET, &row.Address.Ipv4.sin_addr, text, 64);
        n.text = text;
        if (n.text.empty()) continue;
        out.push_back(std::move(n));
    }
    FreeMibTable(table);
    return out;
}

// Reverse lookups can each take seconds when nothing answers, so they run side by side with an overall
// deadline; workers past the deadline only touch this private state.
struct ResolveState {
    std::vector<SOCKADDR_INET> addresses;
    std::vector<std::wstring> names;
    std::unique_ptr<std::atomic<bool>[]> ready;
    std::atomic<size_t> next{0};
};

std::vector<std::wstring> ResolveNames(const std::vector<NeighbourAddress>& neighbours, const CancelFn& cancelled,
                                       unsigned deadlineMs) {
    auto state = std::make_shared<ResolveState>();
    const size_t n = neighbours.size();
    state->names.resize(n);
    state->ready.reset(new std::atomic<bool>[n]);
    for (size_t i = 0; i < n; ++i) {
        state->ready[i].store(false);
        state->addresses.push_back(neighbours[i].address);
    }
    const size_t workers = std::min<size_t>(8, n);
    for (size_t w = 0; w < workers; ++w) {
        RunDetached([state, n]() {
            for (;;) {
                const size_t i = state->next.fetch_add(1);
                if (i >= n) break;
                SOCKADDR_INET a = state->addresses[i];
                const int len = a.si_family == AF_INET6 ? static_cast<int>(sizeof(SOCKADDR_IN6)) : static_cast<int>(sizeof(SOCKADDR_IN));
                wchar_t host[NI_MAXHOST] = {};
                if (GetNameInfoW(reinterpret_cast<const SOCKADDR*>(&a), len, host, NI_MAXHOST, nullptr, 0, NI_NAMEREQD) == 0)
                    state->names[i] = host;
                state->ready[i].store(true, std::memory_order_release);
            }
        });
    }
    const auto start = std::chrono::steady_clock::now();
    for (;;) {
        size_t done = 0;
        for (size_t i = 0; i < n; ++i)
            if (state->ready[i].load(std::memory_order_acquire)) ++done;
        if (done == n || cancelled()) break;
        if (std::chrono::steady_clock::now() - start > std::chrono::milliseconds(deadlineMs)) break;
        Sleep(40);
    }
    std::vector<std::wstring> names(n);
    for (size_t i = 0; i < n; ++i)
        if (state->ready[i].load(std::memory_order_acquire)) names[i] = state->names[i];
    return names;
}

// ---- Wi-Fi (wlanapi.dll loaded on demand: a machine without the WLAN stack just has no Wi-Fi) --------------

struct WlanApi {
    HMODULE module = nullptr;
    decltype(&WlanOpenHandle) open = nullptr;
    decltype(&WlanCloseHandle) close = nullptr;
    decltype(&WlanEnumInterfaces) enumInterfaces = nullptr;
    decltype(&WlanFreeMemory) freeMemory = nullptr;
    decltype(&WlanScan) scan = nullptr;
    decltype(&WlanGetAvailableNetworkList) availableNetworks = nullptr;
    decltype(&WlanQueryInterface) query = nullptr;

    bool Load() {
        module = LoadLibraryExW(L"wlanapi.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module) return false;
        open = reinterpret_cast<decltype(open)>(GetProcAddress(module, "WlanOpenHandle"));
        close = reinterpret_cast<decltype(close)>(GetProcAddress(module, "WlanCloseHandle"));
        enumInterfaces = reinterpret_cast<decltype(enumInterfaces)>(GetProcAddress(module, "WlanEnumInterfaces"));
        freeMemory = reinterpret_cast<decltype(freeMemory)>(GetProcAddress(module, "WlanFreeMemory"));
        scan = reinterpret_cast<decltype(scan)>(GetProcAddress(module, "WlanScan"));
        availableNetworks =
            reinterpret_cast<decltype(availableNetworks)>(GetProcAddress(module, "WlanGetAvailableNetworkList"));
        query = reinterpret_cast<decltype(query)>(GetProcAddress(module, "WlanQueryInterface"));
        // Seven separate GetProcAddress results; each must be non-null. cppcheck wrongly treats them as aliases.
        // cppcheck-suppress knownConditionTrueFalse
        return open && close && enumInterfaces && freeMemory && scan && availableNetworks && query;
    }
    ~WlanApi() {
        if (module) FreeLibrary(module);
    }
};

class WindowsSource : public INetInfoSource {
public:
    WindowsSource() {
        WSADATA data;
        winsockOk_ = WSAStartup(MAKEWORD(2, 2), &data) == 0;
    }
    ~WindowsSource() override {
        if (winsockOk_) WSACleanup();
    }

    LocalInfo ReadLocal() override {
        LocalInfo info;
        wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
        DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
        if (GetComputerNameW(name, &len)) info.computerName = name;
        DWORD error = 0;
        EnumerateShares(L"", true, info.shares, error);  // an error simply leaves the list empty
        EnumerateMappedDrives(info.mapped);
        return info;
    }

    SharesResult ReadSharesOf(const std::wstring& host, const CancelFn& cancelled) override {
        SharesResult r;
        if (cancelled()) return r;
        DWORD error = 0;
        if (EnumerateShares(host, false, r.shares, error)) {
            r.ok = true;
        } else {
            r.error = ShareErrorText(error);
        }
        return r;
    }

    MachineScanResult FindMachines(const CancelFn& cancelled) override {
        MachineScanResult result;
        std::wstring self;
        {
            wchar_t name[MAX_COMPUTERNAME_LENGTH + 1] = {};
            DWORD len = MAX_COMPUTERNAME_LENGTH + 1;
            if (GetComputerNameW(name, &len)) self = name;
        }
        std::map<std::wstring, size_t> byKey;  // lower-case short name or address -> index
        auto keyOf = [](const std::wstring& s) {
            std::wstring k = s;
            std::transform(k.begin(), k.end(), k.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
            return k;
        };

        // Source B first (quick): the neighbour table, names resolved side by side.
        if (winsockOk_ && !cancelled()) {
            std::vector<NeighbourAddress> neighbours = ReadNeighbours();
            std::vector<std::wstring> names = ResolveNames(neighbours, cancelled, 6000);
            for (size_t i = 0; i < neighbours.size() && !cancelled(); ++i) {
                Machine m;
                m.viaNeighbour = true;
                m.address = neighbours[i].text;
                if (!names[i].empty()) {
                    m.host = names[i];
                    m.name = ShortHostName(names[i]);
                } else if (neighbours[i].v6) {
                    continue;  // an IPv6 address without a name says nothing useful
                } else {
                    m.host = m.name = neighbours[i].text;
                }
                if (!self.empty() && SameText(m.name, self)) continue;
                const std::wstring k = keyOf(m.name);
                auto it = byKey.find(k);
                if (it != byKey.end()) {
                    Machine& existing = result.machines[it->second];
                    if (existing.address.empty() || (existing.address.find(L':') != std::wstring::npos && !neighbours[i].v6))
                        existing.address = m.address;  // prefer an IPv4 address for display
                    continue;
                }
                byKey[k] = result.machines.size();
                result.machines.push_back(std::move(m));
            }
        }

        // Source A: browsing. Can wait on the network, so it comes after the quick source.
        if (!cancelled()) {
            std::vector<std::wstring> browsed;
            BrowseGlobalNet(nullptr, 0, browsed, cancelled);
            if (browsed.empty() && !cancelled()) BrowseServers(browsed);
            for (const std::wstring& n : browsed) {
                if (!self.empty() && SameText(ShortHostName(n), self)) continue;
                const std::wstring k = keyOf(ShortHostName(n));
                auto it = byKey.find(k);
                if (it != byKey.end()) {
                    result.machines[it->second].viaBrowse = true;
                    continue;
                }
                Machine m;
                m.name = ShortHostName(n);
                m.host = n;
                m.viaBrowse = true;
                byKey[k] = result.machines.size();
                result.machines.push_back(std::move(m));
            }
        }
        if (result.machines.empty()) {
            result.note =
                L"No machines answered. Windows no longer browses the network by itself; machines show up once this computer has contacted them.";
        }
        return result;
    }

    WifiResult ReadWifi(const CancelFn& cancelled) override {
        WifiResult result;
        WlanApi api;
        if (!api.Load()) {
            result.status = WifiStatus::ServiceStopped;
            return result;
        }
        DWORD negotiated = 0;
        HANDLE handle = nullptr;
        DWORD r = api.open(2, nullptr, &negotiated, &handle);
        if (r == ERROR_SERVICE_NOT_ACTIVE) {
            result.status = WifiStatus::ServiceStopped;
            return result;
        }
        if (r != ERROR_SUCCESS) {
            result.status = WifiStatus::Failed;
            result.detail = ShareErrorText(r);
            return result;
        }
        PWLAN_INTERFACE_INFO_LIST interfaces = nullptr;
        r = api.enumInterfaces(handle, nullptr, &interfaces);
        if (r != ERROR_SUCCESS || !interfaces || interfaces->dwNumberOfItems == 0) {
            if (interfaces) api.freeMemory(interfaces);
            api.close(handle, nullptr);
            result.status = r == ERROR_SERVICE_NOT_ACTIVE ? WifiStatus::ServiceStopped
                            : r == ERROR_SUCCESS          ? WifiStatus::NoAdapter
                                                          : WifiStatus::Failed;
            return result;
        }

        bool denied = false, anyRadioOff = false, anyScanStarted = false;
        std::vector<bool> radioOff(interfaces->dwNumberOfItems, false);
        for (DWORD i = 0; i < interfaces->dwNumberOfItems && !cancelled(); ++i) {
            const GUID& guid = interfaces->InterfaceInfo[i].InterfaceGuid;
            PWLAN_RADIO_STATE radio = nullptr;
            DWORD size = 0;
            WLAN_OPCODE_VALUE_TYPE type;
            if (api.query(handle, &guid, wlan_intf_opcode_radio_state, nullptr, &size, reinterpret_cast<PVOID*>(&radio), &type) ==
                    ERROR_SUCCESS &&
                radio) {
                bool off = radio->dwNumberOfPhys > 0;
                for (DWORD p = 0; p < radio->dwNumberOfPhys && p < 64; ++p)
                    if (radio->PhyRadioState[p].dot11SoftwareRadioState == dot11_radio_state_on &&
                        radio->PhyRadioState[p].dot11HardwareRadioState == dot11_radio_state_on)
                        off = false;
                radioOff[i] = off;
                api.freeMemory(radio);
            }
            if (radioOff[i]) {
                anyRadioOff = true;
                continue;
            }
            DWORD sr = api.scan(handle, &guid, nullptr, nullptr, nullptr);
            if (sr == ERROR_ACCESS_DENIED) denied = true;
            else if (sr == ERROR_SUCCESS) anyScanStarted = true;
        }
        // Scan results arrive within a couple of seconds; wait in short steps so closing the panel is quick.
        for (int waited = 0; anyScanStarted && waited < 2500 && !cancelled(); waited += 100) Sleep(100);

        std::map<std::wstring, size_t> seen;  // ssid + security -> index
        for (DWORD i = 0; i < interfaces->dwNumberOfItems && !cancelled(); ++i) {
            if (radioOff[i]) continue;
            const GUID& guid = interfaces->InterfaceInfo[i].InterfaceGuid;
            PWLAN_AVAILABLE_NETWORK_LIST list = nullptr;
            DWORD lr = api.availableNetworks(handle, &guid, 0, nullptr, &list);
            if (lr == ERROR_ACCESS_DENIED) {
                denied = true;
                continue;
            }
            if (lr == ERROR_SUCCESS && list) {
                for (DWORD k = 0; k < list->dwNumberOfItems; ++k) {
                    const WLAN_AVAILABLE_NETWORK& net = list->Network[k];
                    WifiNetwork n;
                    n.ssid = Utf8ToWide(reinterpret_cast<const char*>(net.dot11Ssid.ucSSID), net.dot11Ssid.uSSIDLength);
                    n.signal = static_cast<int>(net.wlanSignalQuality);
                    n.secured = net.bSecurityEnabled != FALSE;
                    n.security = SecurityName(static_cast<unsigned>(net.dot11DefaultAuthAlgorithm), n.secured);
                    n.connected = (net.dwFlags & WLAN_AVAILABLE_NETWORK_CONNECTED) != 0;
                    const std::wstring key = n.ssid + std::wstring(1, L'|') + n.security;
                    auto it = seen.find(key);
                    if (it != seen.end()) {
                        WifiNetwork& e = result.networks[it->second];
                        e.signal = std::max(e.signal, n.signal);
                        e.connected = e.connected || n.connected;
                        continue;
                    }
                    seen[key] = result.networks.size();
                    result.networks.push_back(std::move(n));
                }
            }
            if (list) api.freeMemory(list);
        }
        api.freeMemory(interfaces);
        api.close(handle, nullptr);

        if (cancelled()) {
            result.status = WifiStatus::Failed;
            return result;
        }
        if (!result.networks.empty()) result.status = WifiStatus::Ok;
        else if (denied) result.status = WifiStatus::LocationBlocked;
        else if (anyRadioOff) result.status = WifiStatus::RadioOff;
        else result.status = WifiStatus::NoNetworksFound;
        return result;
    }

private:
    bool winsockOk_ = false;
};

}  // namespace

std::shared_ptr<INetInfoSource> MakeWindowsSource() { return std::make_shared<WindowsSource>(); }

}  // namespace netinfo
