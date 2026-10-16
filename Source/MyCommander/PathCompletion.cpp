#include "PathCompletion.h"

#include "PathUtil.h"
#include "TextCase.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>

namespace mc {

PathToken ExtractPathToken(const std::wstring& text) {
    PathToken t;
    size_t begin = 0;
    // An odd number of quotes means the cursor is inside one: the path starts after the last quote.
    size_t quotes = 0, lastQuote = std::wstring::npos;
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'"') {
            ++quotes;
            lastQuote = i;
        }
    }
    if (quotes % 2 == 1) {
        t.quoted = true;
        begin = lastQuote + 1;
    } else {
        const size_t space = text.find_last_of(L' ');
        begin = space == std::wstring::npos ? 0 : space + 1;
        // A closed quote group ending the text ("..." ) is not a path being typed; start after it.
        if (begin < text.size() && text[begin] == L'"') ++begin;
    }
    t.start = begin;
    const std::wstring tokenText = text.substr(begin);
    const size_t sep = tokenText.find_last_of(L"\\/");
    if (sep == std::wstring::npos) {
        t.name = tokenText;
    } else {
        t.dir = tokenText.substr(0, sep + 1);
        t.name = tokenText.substr(sep + 1);
    }
    return t;
}

namespace {

bool StartsWithNoCase(const std::wstring& s, const std::wstring& prefix) {
    if (prefix.size() > s.size()) return false;
    return ToLowerInvariant(s.substr(0, prefix.size())) == ToLowerInvariant(prefix);
}

}  // namespace

std::vector<CompletionEntry> FilterCompletions(const std::vector<CompletionEntry>& all, const std::wstring& namePart,
                                               bool includeHidden) {
    const bool wantHidden = includeHidden || (!namePart.empty() && namePart[0] == L'.');
    std::vector<CompletionEntry> out;
    for (const CompletionEntry& e : all) {
        if (e.hidden && !wantHidden) continue;
        if (StartsWithNoCase(e.name, namePart)) out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const CompletionEntry& a, const CompletionEntry& b) {
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
    return out;
}

std::wstring CommonCompletionPrefix(const std::vector<CompletionEntry>& candidates) {
    if (candidates.empty()) return L"";
    std::wstring prefix = candidates.front().name;
    for (size_t i = 1; i < candidates.size() && !prefix.empty(); ++i) {
        const std::wstring other = ToLowerInvariant(candidates[i].name);
        const std::wstring mine = ToLowerInvariant(prefix);
        size_t n = 0;
        while (n < mine.size() && n < other.size() && mine[n] == other[n]) ++n;
        prefix.resize(n);
    }
    return prefix;
}

int NextCompletionIndex(int index, size_t count) {
    if (count == 0) return -1;
    return static_cast<int>((static_cast<size_t>(index + 1)) % count);
}

std::wstring ApplyCompletionPrefix(const std::wstring& text, const PathToken& token, const std::wstring& prefix) {
    return text.substr(0, token.start) + token.dir + prefix;
}

std::wstring ApplyCompletion(const std::wstring& text, const PathToken& token, const CompletionEntry& candidate,
                             bool fullName) {
    std::wstring out = ApplyCompletionPrefix(text, token, candidate.name);
    if (fullName && candidate.isDirectory) out += L'\\';
    return out;
}

std::vector<CompletionEntry> ListFolderForCompletion(const std::filesystem::path& folder) {
    std::vector<CompletionEntry> out;
    WIN32_FIND_DATAW data;
    HANDLE h = FindFirstFileExW(ToLongPath(folder / L"*").c_str(), FindExInfoBasic, &data, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return out;
    do {
        if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) continue;
        CompletionEntry e;
        e.name = data.cFileName;
        e.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        e.hidden = (data.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
        out.push_back(std::move(e));
    } while (out.size() < 50000 && FindNextFileW(h, &data));
    FindClose(h);
    return out;
}

}  // namespace mc
