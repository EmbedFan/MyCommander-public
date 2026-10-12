#include "Favorites.h"

#include "PathUtil.h"
#include "TextWidth.h"
#include "Utf8TextFile.h"

#include <algorithm>
#include <cwctype>

namespace mc {

namespace {

std::wstring Trim(const std::wstring& s) {
    size_t start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";
    size_t end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

// Lexical comparison key: normalized, '\' separators, lower case, no
// trailing separator (except the one a drive root like "c:\" needs).
std::wstring ComparisonKey(const std::filesystem::path& p) {
    std::wstring s = p.lexically_normal().wstring();
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

}  // namespace

ParseFavoritesResult ParseFavorites(const std::wstring& text) {
    ParseFavoritesResult result;
    int lineNumber = 0;
    size_t start = 0;
    while (start <= text.size()) {
        size_t nl = text.find(L'\n', start);
        std::wstring raw = text.substr(start, nl == std::wstring::npos ? std::wstring::npos : nl - start);
        start = nl == std::wstring::npos ? text.size() + 1 : nl + 1;
        ++lineNumber;

        std::wstring line = Trim(raw);
        if (!line.empty() && line.front() == 0xFEFF) line = Trim(line.substr(1));
        if (line.empty() || line.front() == L'#' || line.front() == L';') continue;

        std::wstring prefix = L"favorites.conf line " + std::to_wstring(lineNumber) + L": ";
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            result.warnings.push_back(prefix + L"expected 'name = path': " + line);
            continue;
        }
        Favorite fav;
        fav.name = Trim(line.substr(0, eq));
        std::wstring path = Trim(line.substr(eq + 1));
        if (path.size() >= 2 && path.front() == L'"' && path.back() == L'"') path = path.substr(1, path.size() - 2);
        if (fav.name.empty() || path.empty()) {
            result.warnings.push_back(prefix + (fav.name.empty() ? L"no name: " : L"no path: ") + line);
            continue;
        }
        fav.path = path;
        result.favorites.push_back(std::move(fav));
    }
    return result;
}

std::wstring FormatFavorites(const std::vector<Favorite>& favorites) {
    std::wstring out =
        L"# MyCommander directory hotlist (NAV-007). Ctrl+D opens it; Enter goes there.\n"
        L"# One favorite per line, in list order:  name = path\n"
        L"# Edit and save to reorder; MyCommander rewrites this file when you add,\n"
        L"# rename, or remove a favorite from the hotlist.\n";
    size_t width = 0;
    for (const auto& f : favorites) width = std::max<size_t>(width, static_cast<size_t>(StringDisplayWidth(f.name)));
    for (const auto& f : favorites) out += PadToDisplayWidth(f.name, width) + L" = " + f.path.wstring() + L"\n";
    return out;
}

std::filesystem::path FavoritesFilePath() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path(buf) / L"MyCommander" / L"favorites.conf";
}

ParseFavoritesResult LoadFavoritesFrom(const std::filesystem::path& path) {
    return ParseFavorites(ReadUtf8TextFile(path));
}

bool SaveFavoritesTo(const std::filesystem::path& path, const std::vector<Favorite>& favorites) {
    return WriteUtf8TextFileAtomic(path, FormatFavorites(favorites));
}

int FindFavoriteByPath(const std::vector<Favorite>& favorites, const std::filesystem::path& folder) {
    std::wstring key = ComparisonKey(folder);
    for (size_t i = 0; i < favorites.size(); ++i)
        if (ComparisonKey(favorites[i].path) == key) return static_cast<int>(i);
    return -1;
}

std::wstring DefaultFavoriteName(const std::filesystem::path& folder) {
    std::filesystem::path p = folder;
    std::wstring s = p.wstring();
    while (s.size() > 3 && (s.back() == L'\\' || s.back() == L'/')) s.pop_back();
    p = s;
    if (IsFilesystemRoot(p)) return p.wstring();
    std::wstring leaf = p.filename().wstring();
    return leaf.empty() ? p.wstring() : leaf;
}

bool IsValidFavoriteName(const std::wstring& name) {
    return !Trim(name).empty() && name.find(L'=') == std::wstring::npos;
}

}  // namespace mc
