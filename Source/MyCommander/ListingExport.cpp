#include "ListingExport.h"

#include "PathUtil.h"
#include "Search.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mc {

std::wstring ListingTypeText(const std::wstring& name, bool isDirectory) {
    if (isDirectory) return L"<DIR>";
    const size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0 || dot + 1 >= name.size()) return L"";
    return name.substr(dot + 1);
}

std::wstring ListingAttributeLetters(uint32_t attributes) {
    std::wstring s = L"----";
    if (attributes & FILE_ATTRIBUTE_READONLY) s[0] = L'R';
    if (attributes & FILE_ATTRIBUTE_HIDDEN) s[1] = L'H';
    if (attributes & FILE_ATTRIBUTE_SYSTEM) s[2] = L'S';
    if (attributes & FILE_ATTRIBUTE_ARCHIVE) s[3] = L'A';
    return s;
}

std::wstring FormatListingTime(uint64_t filetime) {
    if (filetime == 0) return L"";
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(filetime & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(filetime >> 32);
    FILETIME local;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &local) || !FileTimeToSystemTime(&local, &st)) return L"";
    wchar_t buf[32];
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    return buf;
}

std::wstring CsvField(const std::wstring& text, bool formulaGuard) {
    std::wstring s = text;
    if (formulaGuard && !s.empty() && (s[0] == L'=' || s[0] == L'+' || s[0] == L'-' || s[0] == L'@' || s[0] == L'\t' || s[0] == L'\r'))
        s.insert(s.begin(), L'\'');
    const bool quote = s.find_first_of(L",\"\r\n") != std::wstring::npos || (!s.empty() && (s.front() == L' ' || s.back() == L' '));
    if (!quote) return s;
    std::wstring out = L"\"";
    for (wchar_t c : s) {
        if (c == L'"') out += L'"';
        out += c;
    }
    out += L'"';
    return out;
}

std::wstring TextField(const std::wstring& text) {
    std::wstring s = text;
    for (wchar_t& c : s)
        if (c == L'\t' || c == L'\r' || c == L'\n') c = L' ';
    return s;
}

std::wstring FormatListing(const std::vector<ListingRow>& rows, ListingFormat format) {
    const bool csv = format == ListingFormat::Csv;
    // `guard` is off for the columns this code generates itself (time, attribute letters).
    auto field = [&](const std::wstring& t, bool guard = true) { return csv ? CsvField(t, guard) : TextField(t); };
    const wchar_t sep = csv ? L',' : L'\t';
    auto line = [&](const std::vector<std::wstring>& cells) {
        std::wstring out;
        for (size_t i = 0; i < cells.size(); ++i) {
            if (i > 0) out += sep;
            out += cells[i];
        }
        out += L"\r\n";
        return out;
    };
    std::wstring text = line({L"Name", L"Type", L"Size", L"Modified", L"Attributes", L"Label", L"Path"});
    for (const ListingRow& r : rows) {
        text += line({field(r.name), field(r.typeText), r.typeText == L"<DIR>" ? L"" : std::to_wstring(r.sizeBytes),
                      field(r.modified, false), field(r.attributes, false), r.label > 0 ? std::to_wstring(r.label) : L"",
                      field(r.fullPath)});
    }
    return text;
}

std::wstring DefaultListingFileName(const std::wstring& folderName, int year, int month, int day, int hour,
                                    int minute, ListingFormat format) {
    std::wstring name = folderName.empty() ? L"drive" : folderName;
    for (wchar_t& c : name)
        if (wcschr(L"<>:\"/\\|?*", c) != nullptr || c < 32) c = L'_';
    while (!name.empty() && (name.back() == L' ' || name.back() == L'.')) name.pop_back();
    if (name.empty()) name = L"drive";
    wchar_t stamp[32];
    swprintf_s(stamp, L"%04d%02d%02d-%02d%02d", year, month, day, hour, minute);
    return L"listing-" + name + L"-" + stamp + (format == ListingFormat::Csv ? L".csv" : L".txt");
}

ListingRow MakeListingRow(const std::wstring& relativeName, const std::filesystem::path& fullPath, bool isDirectory,
                          uint64_t sizeBytes, uint32_t attributes, uint64_t filetime, int label) {
    ListingRow r;
    r.name = relativeName;
    r.typeText = ListingTypeText(fullPath.filename().wstring(), isDirectory);
    r.sizeBytes = isDirectory ? 0 : sizeBytes;
    r.modified = FormatListingTime(filetime);
    r.attributes = ListingAttributeLetters(attributes);
    r.label = label;
    r.fullPath = fullPath.wstring();
    return r;
}

std::vector<ListingRow> GatherPanelRows(const std::vector<Entry>& entries, const std::filesystem::path& folder,
                                        bool selectionOnly, const ListingLabelOf& labelOf) {
    std::vector<ListingRow> rows;
    for (const Entry& e : entries) {
        if (e.name == L"..") continue;
        if (selectionOnly && !e.selected) continue;
        const std::filesystem::path full = folder / e.name;
        rows.push_back(MakeListingRow(e.name, full, e.isDirectory, e.sizeBytes, e.attributes, e.lastWriteTimeUtc,
                                      labelOf ? labelOf(full) : 0));
    }
    return rows;
}

// The search walk reports long-path (\\?\) names; a listing shows the ordinary form.
static std::filesystem::path WithoutLongPathPrefix(const std::filesystem::path& p) {
    std::wstring s = p.wstring();
    if (s.rfind(L"\\\\?\\UNC\\", 0) == 0) return L"\\\\" + s.substr(8);
    if (s.rfind(L"\\\\?\\", 0) == 0) return s.substr(4);
    return p;
}

ListingGather GatherSubtreeRows(const std::filesystem::path& root, const ListingLabelOf& labelOf,
                                const std::function<bool()>& cancelPoll, size_t maxRows) {
    ListingGather out;
    SearchOptions options;
    options.nameMask = L"*";
    options.maxResults = maxRows;
    SearchOutcome found = SearchFiles(root, options, cancelPoll);
    out.cancelled = found.cancelled;
    out.truncated = found.truncated;
    for (const SearchResult& r : found.results) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        const std::filesystem::path longPath = ToLongPath(r.fullPath);
        uint32_t attrs = 0;
        uint64_t time = 0;
        uint64_t size = r.sizeBytes;
        if (GetFileAttributesExW(longPath.c_str(), GetFileExInfoStandard, &data)) {
            attrs = data.dwFileAttributes;
            time = (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
            if (!r.isDirectory) size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        }
        const std::filesystem::path shown = WithoutLongPathPrefix(r.fullPath);
        std::wstring rel = shown.lexically_relative(WithoutLongPathPrefix(root)).wstring();
        if (rel.empty()) rel = shown.wstring();
        out.rows.push_back(MakeListingRow(rel, shown, r.isDirectory, size, attrs, time, labelOf ? labelOf(shown) : 0));
    }
    return out;
}

bool IsExportOutputInsideFolder(const std::filesystem::path& folder, const std::filesystem::path& output) {
    return IsSameOrWithin(folder, output);
}

}  // namespace mc
