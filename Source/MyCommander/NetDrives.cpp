#include "NetDrives.h"

#include "PathUtil.h"

#include <winnetwk.h>

#pragma comment(lib, "mpr.lib")

namespace mc {

bool SetVolumeLabelDirect(const std::filesystem::path& root, const std::wstring& label, DWORD& win32Error) {
    win32Error = 0;
    LPCWSTR newLabel = label.empty() ? nullptr : label.c_str();
    if (SetVolumeLabelW(root.c_str(), newLabel)) return true;
    win32Error = GetLastError();
    return false;
}

bool SetVolumeLabelElevated(const std::filesystem::path& root, const std::wstring& label, std::wstring& err,
                            DWORD& win32Error) {
    if (SetVolumeLabelDirect(root, label, win32Error)) return true;
    err = FormatWinError(win32Error);
    return false;
}

std::wstring FormatConnectionRow(const NetworkConnectionInfo& info) {
    std::wstring local = info.localDevice.empty() ? L"    " : (info.localDevice + L"  ");
    std::wstring row = local + info.remoteName;
    if (info.persistent) row += L"  (persistent)";
    return row;
}

bool IsValidUncPath(const std::wstring& path) {
    if (path.size() < 5 || path[0] != L'\\' || path[1] != L'\\') return false;
    size_t serverStart = 2;
    size_t serverEnd = path.find(L'\\', serverStart);
    if (serverEnd == std::wstring::npos || serverEnd == serverStart) return false;
    size_t shareStart = serverEnd + 1;
    size_t shareEnd = path.find(L'\\', shareStart);
    std::wstring share = path.substr(shareStart, shareEnd == std::wstring::npos ? std::wstring::npos : shareEnd - shareStart);
    return !share.empty();
}

std::vector<NetworkConnectionInfo> EnumerateNetworkConnections() {
    std::vector<NetworkConnectionInfo> result;
    HANDLE enumHandle = nullptr;
    if (WNetOpenEnumW(RESOURCE_CONNECTED, RESOURCETYPE_DISK, 0, nullptr, &enumHandle) != NO_ERROR) return result;

    DWORD count = 0xFFFFFFFF;
    std::vector<BYTE> buffer(16 * 1024);
    for (;;) {
        DWORD bufferSize = static_cast<DWORD>(buffer.size());
        DWORD entriesRead = count;
        DWORD rc = WNetEnumResourceW(enumHandle, &entriesRead, buffer.data(), &bufferSize);
        if (rc == ERROR_MORE_DATA) {
            buffer.resize(bufferSize);
            continue;
        }
        if (rc != NO_ERROR) break;
        const auto* resources = reinterpret_cast<NETRESOURCEW*>(buffer.data());
        for (DWORD i = 0; i < entriesRead; ++i) {
            NetworkConnectionInfo info;
            if (resources[i].lpLocalName) info.localDevice = resources[i].lpLocalName;
            if (resources[i].lpRemoteName) info.remoteName = resources[i].lpRemoteName;
            // WNet doesn't report persistence per connection directly here;
            // a persistent mapping also appears in the registry, but
            // distinguishing that is out of scope -- shown as session-only
            // unless MapNetworkDrive's own caller already knows otherwise.
            result.push_back(std::move(info));
        }
        break;
    }
    WNetCloseEnum(enumHandle);
    return result;
}

DWORD MapNetworkDrive(const std::wstring& driveLetter, const std::wstring& uncPath, bool persistent,
                      const std::wstring& username, const std::wstring& password) {
    NETRESOURCEW resource{};
    std::wstring localName = driveLetter;  // e.g. "Z:" -- empty maps without a drive letter
    resource.dwType = RESOURCETYPE_DISK;
    resource.lpLocalName = localName.empty() ? nullptr : const_cast<LPWSTR>(localName.c_str());
    resource.lpRemoteName = const_cast<LPWSTR>(uncPath.c_str());

    DWORD flags = persistent ? CONNECT_UPDATE_PROFILE : 0;
    return WNetAddConnection2W(&resource, password.empty() ? nullptr : password.c_str(),
                              username.empty() ? nullptr : username.c_str(), flags);
}

DWORD DisconnectNetworkDrive(const std::wstring& localDeviceOrUncPath, bool force) {
    return WNetCancelConnection2W(localDeviceOrUncPath.c_str(), CONNECT_UPDATE_PROFILE, force);
}

} // namespace mc
