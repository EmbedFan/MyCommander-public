#include "CommandHistory.h"

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include <fstream>

namespace mc {

namespace {

// Keeps only the most recent kMaxCommandHistoryEntries of `history`,
// preserving order (oldest first) — shared by both Load and Save so neither
// a file written by a future, larger cap nor a caller passing an oversized
// vector can grow the in-memory/on-disk history without bound.
std::vector<std::wstring> CapToMostRecent(std::vector<std::wstring> history) {
    if (history.size() > kMaxCommandHistoryEntries) {
        history.erase(history.begin(), history.begin() + static_cast<ptrdiff_t>(history.size() - kMaxCommandHistoryEntries));
    }
    return history;
}

}  // namespace

std::filesystem::path CommandHistoryFilePath() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path(buf) / L"MyCommander" / L"history.txt";
}

std::vector<std::wstring> LoadCommandHistoryFrom(const std::filesystem::path& path) {
    std::vector<std::wstring> history;
    if (path.empty()) return history;

    std::wifstream in(path);
    if (!in) return history;  // missing or unreadable -> empty, the expected first-run state

    std::wstring line;
    while (std::getline(in, line)) {
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        if (line.empty()) continue;
        history.push_back(line);
    }
    return CapToMostRecent(std::move(history));
}

std::vector<std::wstring> LoadCommandHistory() { return LoadCommandHistoryFrom(CommandHistoryFilePath()); }

bool SaveCommandHistoryTo(const std::filesystem::path& path, const std::vector<std::wstring>& history) {
    if (path.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    std::vector<std::wstring> capped = CapToMostRecent(history);

    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::wofstream out(temporary, std::ios::trunc);
        if (!out) return false;
        for (const auto& command : capped) {
            out << command << L"\n";
        }
        if (!out) return false;
    }

    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    std::filesystem::remove(temporary, ec);
    return false;
}

bool SaveCommandHistory(const std::vector<std::wstring>& history) {
    return SaveCommandHistoryTo(CommandHistoryFilePath(), history);
}

}  // namespace mc
