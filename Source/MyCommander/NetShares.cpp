#include "NetShares.h"

#include "PathUtil.h"

#include <lm.h>

#include <algorithm>
#include <cwctype>

#pragma comment(lib, "netapi32.lib")

namespace mc {

bool IsValidShareName(const std::wstring& name) {
    if (name.empty() || name.size() > 80) return false;
    static const wchar_t kForbidden[] = L"\\/:*?\"<>|";
    return name.find_first_of(kForbidden) == std::wstring::npos;
}

std::wstring DefaultShareName(const std::filesystem::path& folder) {
    std::wstring name = folder.filename().wstring();
    if (name.empty()) name = folder.root_name().wstring();  // a drive root has no filename()
    return name;
}

namespace {

// The comparison key: no long-path prefix, one separator style, no trailing
// separator (except "C:\"), lower case.
std::wstring ShareKey(std::wstring s) {
    if (s.rfind(L"\\\\?\\UNC\\", 0) == 0) {
        s = L"\\\\" + s.substr(8);
    } else if (s.rfind(L"\\\\?\\", 0) == 0) {
        s = s.substr(4);
    }
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    if (s.size() == 2 && s[1] == L':') s += L'\\';  // "C:" is the drive root here
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(::towlower(c)); });
    return s;
}

}  // namespace

bool SharePathMatches(const std::wstring& sharePath, const std::filesystem::path& folder) {
    if (sharePath.empty() || folder.empty()) return false;
    return ShareKey(sharePath) == ShareKey(folder.wstring());
}

std::filesystem::path ShareTargetFolder(const std::filesystem::path& panelPath, const std::wstring& cursorName,
                                        bool cursorIsDirectory) {
    if (cursorIsDirectory && !cursorName.empty() && cursorName != L"..") return panelPath / cursorName;
    return panelPath;
}

SharedFolderSet::SharedFolderSet(const std::vector<ShareInfo>& shares) {
    for (const auto& s : shares) {
        if (s.isSpecial || s.path.empty()) continue;
        keys_.push_back(ShareKey(s.path.wstring()));
    }
    std::sort(keys_.begin(), keys_.end());
    keys_.erase(std::unique(keys_.begin(), keys_.end()), keys_.end());
}

bool SharedFolderSet::Contains(const std::filesystem::path& folder) const {
    if (keys_.empty() || folder.empty()) return false;
    return std::binary_search(keys_.begin(), keys_.end(), ShareKey(folder.wstring()));
}

bool EnumerateAllShares(std::vector<ShareInfo>& out) {
    PBYTE buffer = nullptr;
    DWORD entriesRead = 0, totalEntries = 0, resumeHandle = 0;
    NET_API_STATUS status =
        NetShareEnum(nullptr, 2, &buffer, MAX_PREFERRED_LENGTH, &entriesRead, &totalEntries, &resumeHandle);
    if (status != NERR_Success && status != ERROR_MORE_DATA) {
        if (buffer) NetApiBufferFree(buffer);
        return false;
    }
    out.clear();
    const auto* shares = reinterpret_cast<SHARE_INFO_2*>(buffer);
    for (DWORD i = 0; i < entriesRead; ++i) {
        if (!shares[i].shi2_path) continue;
        ShareInfo info;
        info.shareName = shares[i].shi2_netname ? shares[i].shi2_netname : L"";
        info.path = std::wstring(shares[i].shi2_path);
        info.description = shares[i].shi2_remark ? shares[i].shi2_remark : L"";
        info.readOnly = (shares[i].shi2_permissions & ACCESS_ALL) == 0;
        info.isSpecial = (shares[i].shi2_type & STYPE_SPECIAL) != 0;
        out.push_back(std::move(info));
    }
    if (buffer) NetApiBufferFree(buffer);
    return true;
}

std::vector<ShareInfo> EnumerateSharesFor(const std::filesystem::path& folder) {
    std::vector<ShareInfo> result;
    std::vector<ShareInfo> all;
    if (!EnumerateAllShares(all)) return result;
    for (auto& s : all) {
        if (s.isSpecial) continue;
        if (SharePathMatches(s.path.wstring(), folder)) result.push_back(std::move(s));
    }
    return result;
}

bool AddShareDirect(const std::wstring& shareName, const std::filesystem::path& path, const std::wstring& description,
                    bool readOnly, DWORD& win32Error) {
    win32Error = 0;
    std::wstring pathStr = path.wstring();
    std::wstring name = shareName;
    std::wstring remark = description;

    SHARE_INFO_2 info{};
    info.shi2_netname = const_cast<LPWSTR>(name.c_str());
    info.shi2_type = STYPE_DISKTREE;
    info.shi2_remark = const_cast<LPWSTR>(remark.c_str());
    info.shi2_permissions = readOnly ? ACCESS_READ : ACCESS_ALL;
    info.shi2_max_uses = static_cast<DWORD>(-1);
    info.shi2_path = const_cast<LPWSTR>(pathStr.c_str());
    info.shi2_passwd = nullptr;

    DWORD paramError = 0;
    NET_API_STATUS status = NetShareAdd(nullptr, 2, reinterpret_cast<LPBYTE>(&info), &paramError);
    if (status == NERR_Success) return true;
    win32Error = static_cast<DWORD>(status);
    return false;
}

bool DeleteShareDirect(const std::wstring& shareName, DWORD& win32Error) {
    win32Error = 0;
    std::wstring name = shareName;
    NET_API_STATUS status = NetShareDel(nullptr, const_cast<LPWSTR>(name.c_str()), 0);
    if (status == NERR_Success) return true;
    win32Error = static_cast<DWORD>(status);
    return false;
}

bool AddShareElevated(const std::wstring& shareName, const std::filesystem::path& path,
                      const std::wstring& description, bool readOnly, std::wstring& err, DWORD& win32Error) {
    if (AddShareDirect(shareName, path, description, readOnly, win32Error)) return true;
    err = FormatWinError(win32Error);
    return false;
}

bool DeleteShareElevated(const std::wstring& shareName, std::wstring& err, DWORD& win32Error) {
    if (DeleteShareDirect(shareName, win32Error)) return true;
    err = FormatWinError(win32Error);
    return false;
}

} // namespace mc
