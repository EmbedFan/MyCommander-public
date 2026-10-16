#include "RecentLocations.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "TextCase.h"
#include "Utf8TextFile.h"

#include <algorithm>

namespace mc {

std::wstring RecentLocationKey(const std::filesystem::path& folder) {
    std::wstring s = folder.lexically_normal().wstring();
    std::replace(s.begin(), s.end(), L'/', L'\\');
    // "C:\" keeps its separator (size 3); every other trailing one goes.
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return ToLowerInvariant(s);
}

void RecentLocations::Note(const std::filesystem::path& folder) {
    if (folder.empty()) return;
    const std::wstring key = RecentLocationKey(folder);
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [&](const std::filesystem::path& e) { return RecentLocationKey(e) == key; }),
                   entries_.end());
    entries_.insert(entries_.begin(), folder);
    if (entries_.size() > cap_) entries_.resize(cap_);
}

bool RecentLocations::Remove(const std::filesystem::path& folder) {
    const std::wstring key = RecentLocationKey(folder);
    auto it = std::find_if(entries_.begin(), entries_.end(),
                           [&](const std::filesystem::path& e) { return RecentLocationKey(e) == key; });
    if (it == entries_.end()) return false;
    entries_.erase(it);
    return true;
}

void RecentLocations::Replace(const std::vector<std::filesystem::path>& newestFirst) {
    entries_.clear();
    for (const auto& p : newestFirst) {
        if (p.empty()) continue;
        const std::wstring key = RecentLocationKey(p);
        bool seen = std::any_of(entries_.begin(), entries_.end(),
                                [&](const std::filesystem::path& e) { return RecentLocationKey(e) == key; });
        if (!seen) entries_.push_back(p);
        if (entries_.size() >= cap_) break;
    }
}

std::vector<std::filesystem::path> ParseRecentFile(const std::wstring& text, size_t cap) {
    std::vector<std::filesystem::path> raw;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        std::wstring line = text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start);
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        size_t a = line.find_first_not_of(L" \t");
        if (a != std::wstring::npos && line[a] != L'#') {
            size_t b = line.find_last_not_of(L" \t");
            raw.emplace_back(line.substr(a, b - a + 1));
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    RecentLocations list(cap);
    list.Replace(raw);
    return list.Entries();
}

std::wstring FormatRecentFile(const std::vector<std::filesystem::path>& newestFirst) {
    std::wstring out =
        L"# MyCommander recent folders, newest first (NAV-017).\n"
        L"# Rewritten when MyCommander exits; one folder per line.\n";
    for (const auto& p : newestFirst) out += p.wstring() + L"\n";
    return out;
}

std::filesystem::path RecentFoldersFilePath() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path(buf) / L"MyCommander" / L"recent-folders.txt";
}

std::vector<std::filesystem::path> LoadRecentFoldersFrom(const std::filesystem::path& path) {
    if (path.empty()) return {};
    return ParseRecentFile(ReadUtf8TextFile(path));
}

std::vector<std::filesystem::path> LoadRecentFolders() { return LoadRecentFoldersFrom(RecentFoldersFilePath()); }

bool SaveRecentFoldersTo(const std::filesystem::path& path, const std::vector<std::filesystem::path>& newestFirst) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return WriteUtf8TextFileAtomic(path, FormatRecentFile(newestFirst));
}

bool SaveRecentFolders(const std::vector<std::filesystem::path>& newestFirst) {
    return SaveRecentFoldersTo(RecentFoldersFilePath(), newestFirst);
}

}  // namespace mc
