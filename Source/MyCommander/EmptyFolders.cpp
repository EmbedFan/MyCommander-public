#include "EmptyFolders.h"

#include "PathUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mc {

namespace {

bool IsJunkFile(const wchar_t* name) {
    return _wcsicmp(name, L"Thumbs.db") == 0 || _wcsicmp(name, L"desktop.ini") == 0;
}

struct Context {
    const EmptyFolderOptions& options;
    const EmptyFoldersCancelPoll& cancelPoll;
    const EmptyFoldersProgress& onProgress;
    EmptyFoldersOutcome& outcome;
};

struct Node {
    bool empty = false;
    size_t below = 0;  // folders underneath (meaningful only when `empty`)
};

void AddResult(Context& ctx, const std::filesystem::path& path, size_t below) {
    if (ctx.outcome.folders.size() >= ctx.options.maxResults) {
        ctx.outcome.truncated = true;
        return;
    }
    ctx.outcome.folders.push_back(EmptyFolder{path, below});
}

// Lists `dir` and decides whether it is empty. Empty folders are reported by the
// first non-empty ancestor (or, for the root, always individually), so the
// topmost folder of an empty chain is listed exactly once.
Node Visit(const std::filesystem::path& dir, Context& ctx, bool isRoot) {
    if (ctx.outcome.cancelled) return {};
    if (ctx.cancelPoll && ctx.cancelPoll()) {
        ctx.outcome.cancelled = true;
        return {};
    }
    if (ctx.onProgress) ctx.onProgress(dir);

    const std::filesystem::path pattern = ToLongPath(dir) / L"*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        // No entries at all is ERROR_FILE_NOT_FOUND on some volumes; anything else
        // means the folder could not be listed: unknown, never "empty".
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return Node{true, 0};
        ++ctx.outcome.unreadable;
        return {};
    }

    bool hasContent = false;
    std::vector<std::pair<std::filesystem::path, Node>> children;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        const bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!isDir) {
            if (ctx.options.ignoreJunkFiles && IsJunkFile(fd.cFileName)) continue;
            hasContent = true;  // any file, hidden or not
            continue;
        }
        const bool isReparse = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        const bool isHidden = (fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
        if (isReparse || (isHidden && !ctx.options.includeHidden)) {
            hasContent = true;  // never entered, never reported, and it keeps its parent
            continue;
        }
        const std::filesystem::path child = dir / name;
        Node node = Visit(child, ctx, false);
        if (ctx.outcome.cancelled) {
            FindClose(h);
            return {};
        }
        children.emplace_back(child, node);
    } while (FindNextFileW(h, &fd));
    FindClose(h);

    bool allChildrenEmpty = true;
    for (const auto& c : children) allChildrenEmpty = allChildrenEmpty && c.second.empty;

    const bool selfEmpty =
        !hasContent && (children.empty() || (ctx.options.chainsCountAsEmpty && allChildrenEmpty));

    if (selfEmpty && !isRoot) {
        Node node{true, 0};
        for (const auto& c : children) node.below += c.second.below + 1;
        return node;  // the parent reports it
    }
    // This folder isn't empty (or is the root): report the empty children as they are.
    for (const auto& c : children) {
        if (c.second.empty) AddResult(ctx, c.first, c.second.below);
    }
    return Node{false, 0};
}

}  // namespace

EmptyFoldersOutcome FindEmptyFolders(const std::filesystem::path& root, const EmptyFolderOptions& options,
                                     const EmptyFoldersCancelPoll& cancelPoll, const EmptyFoldersProgress& onProgress) {
    EmptyFoldersOutcome outcome;
    const DWORD attrs = GetAttributesOrInvalid(root);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return outcome;  // SEC-006: a reparse point is not walked
    Context ctx{options, cancelPoll, onProgress, outcome};
    Visit(root, ctx, /*isRoot=*/true);
    return outcome;
}

bool IsFolderEmptyNow(const std::filesystem::path& folder, const EmptyFolderOptions& options) {
    const DWORD attrs = GetAttributesOrInvalid(folder);
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_DIRECTORY) == 0 ||
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0)
        return false;
    EmptyFoldersOutcome scratch;
    EmptyFolderOptions o = options;
    o.maxResults = 1;
    const EmptyFoldersCancelPoll noCancel;
    const EmptyFoldersProgress noProgress;
    Context ctx{o, noCancel, noProgress, scratch};
    Node node = Visit(folder, ctx, /*isRoot=*/false);
    return node.empty && scratch.unreadable == 0 && !scratch.cancelled;
}

}  // namespace mc
