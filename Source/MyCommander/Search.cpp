#include "Search.h"

#include "PathUtil.h"

namespace fs = std::filesystem;

namespace mc {

namespace {

void Walk(const fs::path& dir, const std::wstring& pattern, const SearchCancelPoll& cancelPoll, size_t maxResults,
          SearchOutcome& out) {
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
        if (WildcardMatch(name, pattern)) {
            out.results.push_back({item.path(), isDir});
            if (out.results.size() >= maxResults) {
                out.truncated = true;
                return;
            }
        }
        if (isDir && !IsReparsePoint(item.path())) {
            Walk(item.path(), pattern, cancelPoll, maxResults, out);
        }
    }
}

}  // namespace

SearchOutcome SearchFiles(const fs::path& root, const std::wstring& pattern, const SearchCancelPoll& cancelPoll,
                          size_t maxResults) {
    SearchOutcome out;
    if (!pattern.empty()) {
        Walk(root, pattern, cancelPoll, maxResults, out);
    }
    return out;
}

}  // namespace mc
