#include "Search.h"

#include "PathUtil.h"

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
        if (isDir && !IsReparsePoint(item.path())) {
            Walk(item.path(), options, cancelPoll, onProgress, out);
        }
    }
}

}  // namespace

SearchOutcome SearchFiles(const fs::path& root, const SearchOptions& options, const SearchCancelPoll& cancelPoll,
                          const SearchProgress& onProgress) {
    SearchOutcome out;
    if (!options.nameMask.empty()) {
        Walk(root, options, cancelPoll, onProgress, out);
    }
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
