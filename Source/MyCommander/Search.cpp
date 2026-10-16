#include "Search.h"

#include "PathUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>

namespace fs = std::filesystem;

namespace mc {

namespace {

// Reads `file` looking for the first line `options.content` matches.
// Returns true and fills `result` on a match.
bool SearchFileContent(const fs::path& file, const SearchOptions& options, const SearchCancelPoll& cancelPoll,
                       SearchOutcome& out, SearchResult& result) {
    bool binary = false;
    bool found = false;
    const bool regex = options.content->Query().regex;
    auto onProbe = [&](const TextFileProbe& probe) {
        bool utf16 = probe.utf16Bom || options.encoding == TextEncodingChoice::Utf16Le ||
                     options.encoding == TextEncodingChoice::Utf16Be;
        if (options.skipBinary && LooksBinary(probe.head.data(), probe.head.size(), utf16)) {
            binary = true;
            return false;
        }
        return true;
    };
    auto onLine = [&](size_t lineNumber, const std::wstring& line) {
        // Esc: every line for a regex (each line can take a while), every
        // 4096 lines for plain text (cheap per line).
        if (cancelPoll && (regex || (lineNumber & 4095) == 0) && cancelPoll()) {
            out.cancelled = true;
            return false;
        }
        if (!options.content->Matches(line)) return true;
        result.lineNumber = lineNumber;
        result.lineText = line;
        found = true;
        return false;
    };
    if (!ForEachTextLine(file, options.encoding, onLine, onProbe)) {
        ++out.unreadable;
        return false;
    }
    if (binary) {
        ++out.binarySkipped;
        return false;
    }
    ++out.filesSearched;
    return found;
}

// SRC-010: one file offered to the top-N list. The hidden/system check runs
// only when the file would actually enter the list, so an ordinary tree costs
// no extra attribute call per file.
void OfferLargest(const fs::directory_entry& item, uint64_t size, const SearchOptions& options, SearchOutcome& out) {
    if (size < options.minSize) return;
    SearchResult candidate{item.path(), false, size};
    if (out.results.size() >= options.largestCount && !IsBetterLargest(candidate, out.results.front())) return;
    if (!options.includeHidden) {
        DWORD attrs = GetAttributesOrInvalid(item.path());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0) return;
    }
    OfferTopN(out.results, std::move(candidate), options.largestCount);
}

void Walk(const fs::path& dir, const SearchOptions& options, const SearchCancelPoll& cancelPoll,
          const SearchProgress& onProgress, SearchOutcome& out) {
    if (out.cancelled || out.truncated) return;
    if (cancelPoll && cancelPoll()) {
        out.cancelled = true;
        return;
    }

    std::error_code ec;
    fs::directory_iterator it(ToLongPath(dir), fs::directory_options::skip_permission_denied, ec);
    if (ec) return;

    for (const auto& item : it) {
        if (out.cancelled || out.truncated) return;

        std::error_code entryEc;
        bool isDir = item.is_directory(entryEc);
        std::wstring name = item.path().filename().wstring();
        if (options.largestCount > 0) {
            // SRC-010: ranking mode. Files only; a symbolic link is never ranked
            // (its listed size is not the target's size); hidden/system handled in OfferLargest.
            if (!isDir && !item.is_symlink(entryEc) && WildcardMatch(name, options.nameMask)) {
                std::error_code sizeEc;
                uint64_t size = item.file_size(sizeEc);
                if (!sizeEc) {
                    ++out.filesSearched;
                    if (onProgress) onProgress(out.filesSearched, item.path());
                    OfferLargest(item, size, options, out);
                }
            }
            if (isDir && options.recursive && !IsReparsePoint(item.path())) {
                bool skipFolder = false;
                if (!options.includeHidden) {
                    DWORD attrs = GetAttributesOrInvalid(item.path());
                    skipFolder = attrs != INVALID_FILE_ATTRIBUTES && (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
                }
                if (!skipFolder) Walk(item.path(), options, cancelPoll, onProgress, out);
            }
            if (cancelPoll && cancelPoll()) out.cancelled = true;
            if (out.cancelled) return;
            continue;
        }
        if (WildcardMatch(name, options.nameMask)) {
            SearchResult result{item.path(), isDir};
            bool include = true;
            if (options.content) {
                include = false;
                if (!isDir) {
                    if (onProgress) onProgress(out.filesSearched, item.path());
                    include = SearchFileContent(item.path(), options, cancelPoll, out, result);
                    if (out.cancelled) return;
                }
            }
            if (include) {
                out.results.push_back(std::move(result));
                if (out.results.size() >= options.maxResults) {
                    out.truncated = true;
                    return;
                }
            }
        }
        if (isDir && options.recursive && !IsReparsePoint(item.path())) {
            Walk(item.path(), options, cancelPoll, onProgress, out);
        }
    }
}

void WalkProvider(IFileProvider& provider, const std::wstring& dir, const SearchOptions& options,
                  const SearchCancelPoll& cancelPoll, const SearchProgress& onProgress, SearchOutcome& out) {
    if (out.cancelled || out.truncated) return;
    if (cancelPoll && cancelPoll()) {
        out.cancelled = true;
        return;
    }
    if (onProgress) onProgress(out.results.size(), fs::path(dir));
    std::vector<ProviderEntry> entries;
    std::wstring error;
    if (provider.List(dir, entries, cancelPoll, error) != ProviderStatus::Ok) return;
    for (const auto& e : entries) {
        if (out.cancelled || out.truncated) return;
        std::wstring path = provider.Join(dir, e.name);
        if (WildcardMatch(e.name, options.nameMask)) {
            out.results.push_back(SearchResult{fs::path(path), e.isDirectory});
            if (out.results.size() >= options.maxResults) {
                out.truncated = true;
                return;
            }
        }
        if (e.isDirectory && !e.isLink) WalkProvider(provider, path, options, cancelPoll, onProgress, out);
    }
}

}  // namespace

SearchOutcome SearchProvider(IFileProvider& provider, const std::wstring& root, const SearchOptions& options,
                             const SearchCancelPoll& cancelPoll, const SearchProgress& onProgress) {
    SearchOutcome out;
    if (!options.nameMask.empty()) WalkProvider(provider, root, options, cancelPoll, onProgress, out);
    return out;
}

bool IsBetterLargest(const SearchResult& a, const SearchResult& b) {
    if (a.sizeBytes != b.sizeBytes) return a.sizeBytes > b.sizeBytes;
    return a.fullPath.wstring() < b.fullPath.wstring();
}

void OfferTopN(std::vector<SearchResult>& heap, SearchResult candidate, size_t count) {
    if (count == 0) return;
    // std heaps put the largest element (under the comparator) at the front, so
    // "a precedes b" = "a is better" makes the front the weakest kept entry.
    auto weakestFirst = [](const SearchResult& a, const SearchResult& b) { return IsBetterLargest(a, b); };
    if (heap.size() < count) {
        heap.push_back(std::move(candidate));
        std::push_heap(heap.begin(), heap.end(), weakestFirst);
        return;
    }
    if (!IsBetterLargest(candidate, heap.front())) return;
    std::pop_heap(heap.begin(), heap.end(), weakestFirst);
    heap.back() = std::move(candidate);
    std::push_heap(heap.begin(), heap.end(), weakestFirst);
}

void SortLargestFirst(std::vector<SearchResult>& results) {
    std::sort(results.begin(), results.end(), IsBetterLargest);
}

bool ParseLargestCount(const std::wstring& text, size_t& count) {
    if (text.empty()) return false;
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
    }
    if (text.size() > 6) return false;
    size_t value = std::stoull(text);
    if (value < 1 || value > 10000) return false;
    count = value;
    return true;
}

bool ParseMinimumSize(const std::wstring& text, uint64_t& bytes) {
    if (text.empty()) {
        bytes = 0;
        return true;
    }
    for (wchar_t ch : text) {
        if (ch < L'0' || ch > L'9') return false;
    }
    if (text.size() > 18) return false;
    bytes = std::stoull(text);
    return true;
}

SearchOutcome SearchFiles(const fs::path& root, const SearchOptions& options, const SearchCancelPoll& cancelPoll,
                          const SearchProgress& onProgress) {
    SearchOutcome out;
    if (!options.nameMask.empty()) {
        Walk(root, options, cancelPoll, onProgress, out);
    }
    if (options.largestCount > 0) SortLargestFirst(out.results);
    return out;
}

SearchOutcome SearchFiles(const fs::path& root, const std::wstring& pattern, const SearchCancelPoll& cancelPoll,
                          size_t maxResults) {
    SearchOptions options;
    options.nameMask = pattern;
    options.maxResults = maxResults;
    return SearchFiles(root, options, cancelPoll);
}

}  // namespace mc
