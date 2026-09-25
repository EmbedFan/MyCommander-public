#include "Session.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <windows.h>

namespace mc {

namespace {

std::wstring Trim(const std::wstring& text) {
    const size_t first = text.find_first_not_of(L" \t\r\n");
    if (first == std::wstring::npos) return L"";
    const size_t last = text.find_last_not_of(L" \t\r\n");
    return text.substr(first, last - first + 1);
}

std::wstring ToLower(std::wstring text) {
    std::transform(text.begin(), text.end(), text.begin(), [](wchar_t ch) {
        return static_cast<wchar_t>(std::towlower(ch));
    });
    return text;
}

}  // namespace

std::filesystem::path SessionFilePath() {
    wchar_t buffer[MAX_PATH];
    const DWORD length = GetEnvironmentVariableW(L"APPDATA", buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    return std::filesystem::path(buffer) / L"MyCommander" / L"session.ini";
}

SessionState LoadSessionFrom(const std::filesystem::path& path) {
    SessionState state;
    if (path.empty()) return state;

    std::wifstream input(path);
    if (!input) return state;

    std::wstring line;
    while (std::getline(input, line)) {
        const std::wstring trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == L'#' || trimmed.front() == L';') continue;
        const size_t equals = trimmed.find(L'=');
        if (equals == std::wstring::npos) continue;

        const std::wstring key = ToLower(Trim(trimmed.substr(0, equals)));
        const std::wstring value = Trim(trimmed.substr(equals + 1));
        if (value.empty()) continue;
        if (key == L"leftpath") state.leftPath = value;
        if (key == L"rightpath") state.rightPath = value;
    }
    return state;
}

SessionState LoadSession() { return LoadSessionFrom(SessionFilePath()); }

bool SaveSessionTo(const std::filesystem::path& path, const SessionState& state) {
    if (path.empty() || !state.leftPath || !state.rightPath) return false;

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;

    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::wofstream output(temporary, std::ios::trunc);
        if (!output) return false;
        output << L"# MyCommander clean-exit panel locations (NAV-008)\n"
               << L"leftPath = " << state.leftPath->wstring() << L"\n"
               << L"rightPath = " << state.rightPath->wstring() << L"\n";
        if (!output) return false;
    }

    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    std::filesystem::remove(temporary, error);
    return false;
}

bool SaveSession(const SessionState& state) { return SaveSessionTo(SessionFilePath(), state); }

}  // namespace mc
