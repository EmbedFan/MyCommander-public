#include "RemoteUi.h"

#include <cwchar>
#include <cwctype>

namespace remote {

bool DownloadReplacing(const std::wstring& target, const std::wstring& partSuffix,
                       const std::function<bool(const std::wstring& tempPath)>& transfer) {
    std::wstring temp = target + partSuffix;
    bool ok = transfer(temp);
    if (ok) ok = MoveFileExW(temp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED) != 0;
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}

std::wstring FormatPanelTitle(const std::wstring& scheme, uint16_t defaultPort, const std::wstring& user,
                              const std::wstring& host, uint16_t port, const std::wstring& path) {
    std::wstring title = scheme + L"://";
    if (!user.empty() && _wcsicmp(user.c_str(), L"anonymous") != 0) title += user + L"@";
    title += host.find(L':') != std::wstring::npos ? L"[" + host + L"]" : host;
    if (port != defaultPort) title += L":" + std::to_wstring(port);
    return title + (path.empty() ? L"/" : path);
}

std::wstring FormatSize(uint64_t bytes) {
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) {
        value /= 1024.0;
        ++unit;
    }
    wchar_t buffer[32];
    if (unit == 0) {
        swprintf_s(buffer, L"%llu B", static_cast<unsigned long long>(bytes));
    } else {
        static const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
        swprintf_s(buffer, L"%.1f %s", value, kUnits[unit]);
    }
    return buffer;
}

bool WildcardMatch(const wchar_t* name, const wchar_t* pattern) {
    if (!*pattern) return !*name;
    if (*pattern == L'*') return WildcardMatch(name, pattern + 1) || (*name && WildcardMatch(name + 1, pattern));
    if (!*name) return false;
    if (*pattern == L'?' || std::towlower(*pattern) == std::towlower(*name)) return WildcardMatch(name + 1, pattern + 1);
    return false;
}

}  // namespace remote
