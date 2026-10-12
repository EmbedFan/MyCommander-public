#include "DirectoryTree.h"

#include "PathUtil.h"

#include <algorithm>
#include <cwctype>

namespace mc {

std::optional<std::vector<TreeDirEntry>> ListSubdirectories(const std::filesystem::path& dir) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::directory_iterator it(ToLongPath(dir), fs::directory_options::skip_permission_denied, ec);
    if (ec) return std::nullopt;

    std::vector<TreeDirEntry> result;
    while (it != fs::directory_iterator{}) {
        DWORD attrs = GetAttributesOrInvalid(it->path());
        // Win32 attributes, not std::filesystem's own (symlink-following)
        // is_directory(): a junction/symlink's attributes are always
        // readable even when its target is missing or inaccessible, so a
        // broken one is still listed (marked), matching LIST-006's
        // precedent for an ordinary panel rather than silently vanishing.
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY)) {
            TreeDirEntry item;
            item.name = it->path().filename().wstring();
            item.isReparsePoint = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
            result.push_back(std::move(item));
        }
        it.increment(ec);
        if (ec) break;
    }

    std::sort(result.begin(), result.end(), [](const TreeDirEntry& a, const TreeDirEntry& b) {
        auto toLower = [](std::wstring s) {
            std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return s;
        };
        return toLower(a.name) < toLower(b.name);
    });
    return result;
}

std::filesystem::path FindFilesystemRoot(std::filesystem::path path) {
    while (!IsFilesystemRoot(path)) {
        std::filesystem::path parent = path.parent_path();
        if (parent == path) break;  // safety net; IsFilesystemRoot should already cover this
        path = parent;
    }
    return path;
}

std::vector<std::filesystem::path> ExpansionChain(const std::filesystem::path& root,
                                                   const std::filesystem::path& target) {
    namespace fs = std::filesystem;
    std::vector<fs::path> chain;
    fs::path current = target;
    bool foundRoot = false;
    while (true) {
        chain.push_back(current);
        if (PathsEquivalent(current, root)) {
            foundRoot = true;
            break;
        }
        fs::path parent = current.parent_path();
        if (parent == current) break;  // reached a real fs root without matching `root`
        current = parent;
    }
    if (!foundRoot) return {root};
    std::reverse(chain.begin(), chain.end());
    return chain;
}

DirectoryTree::DirectoryTree(std::filesystem::path root, const std::filesystem::path& initialSelection) {
    TreeNode rootNode;
    rootNode.path = std::move(root);
    rootNode.depth = 0;
    nodes_.push_back(std::move(rootNode));
    Expand(0);  // the root's own children are always visible, even when the cursor starts on the root

    std::vector<std::filesystem::path> chain = ExpansionChain(nodes_[0].path, initialSelection);
    size_t current = 0;
    for (size_t i = 1; i < chain.size(); ++i) {
        Expand(current);  // indices only, safe across the push_backs inside
        const auto& children = nodes_[current].childIndices;
        auto found = std::find_if(children.begin(), children.end(),
                                  [&](size_t childIdx) { return PathsEquivalent(nodes_[childIdx].path, chain[i]); });
        if (found == children.end()) break;  // chain and the real listing disagree -- stop at the deepest match
        current = *found;
    }
    cursorNode_ = current;
    topVisiblePos_ = 0;
}

void DirectoryTree::Expand(size_t nodeIndex) {
    if (!nodes_[nodeIndex].childrenLoaded) {
        std::filesystem::path parentPath = nodes_[nodeIndex].path;
        int childDepth = nodes_[nodeIndex].depth + 1;
        auto listed = ListSubdirectories(parentPath);
        // Re-index nodes_[nodeIndex] fresh after the filesystem call --
        // nothing resized nodes_ above, but this keeps the discipline
        // explicit: never hold a TreeNode& across a nodes_.push_back below,
        // which can reallocate and dangle any reference taken before it.
        nodes_[nodeIndex].childrenLoaded = true;
        if (!listed) {
            nodes_[nodeIndex].loadFailed = true;
        } else {
            std::vector<size_t> newChildIndices;
            for (const auto& child : *listed) {
                TreeNode childNode;
                childNode.path = parentPath / child.name;
                childNode.depth = childDepth;
                childNode.isReparsePoint = child.isReparsePoint;
                childNode.parentIndex = nodeIndex;
                nodes_.push_back(std::move(childNode));
                newChildIndices.push_back(nodes_.size() - 1);
            }
            nodes_[nodeIndex].childIndices = std::move(newChildIndices);
        }
    }
    nodes_[nodeIndex].expanded = true;
}

void DirectoryTree::Collapse(size_t nodeIndex) { nodes_[nodeIndex].expanded = false; }

void DirectoryTree::CollapseCursorOrGoToParent(int visibleRows) {
    if (nodes_[cursorNode_].expanded) {
        Collapse(cursorNode_);
    } else if (nodes_[cursorNode_].parentIndex != SIZE_MAX) {
        cursorNode_ = nodes_[cursorNode_].parentIndex;
    }
    ClampToVisible(visibleRows);
}

TreeKeyAction ClassifyTreeKey(uint16_t virtualKeyCode, bool ctrlHeld, wchar_t unicodeChar) {
    // IS-0018: the typed character first -- layout-independent.
    if (unicodeChar == L'+') return TreeKeyAction::ExpandCursor;
    if (unicodeChar == L'-') return TreeKeyAction::CollapseCursor;
    switch (virtualKeyCode) {
        case VK_UP: return TreeKeyAction::MoveUp;
        case VK_DOWN: return TreeKeyAction::MoveDown;
        case VK_PRIOR: return TreeKeyAction::PageUp;
        case VK_NEXT: return TreeKeyAction::PageDown;
        case VK_HOME: return TreeKeyAction::Home;
        case VK_END: return TreeKeyAction::End;
        case VK_LEFT: return TreeKeyAction::CollapseOrGoToParent;
        case VK_RIGHT: return TreeKeyAction::ExpandCursor;
        case VK_ADD:
        case VK_OEM_PLUS: return TreeKeyAction::ExpandCursor;
        case VK_SUBTRACT:
        case VK_OEM_MINUS: return TreeKeyAction::CollapseCursor;
        case VK_RETURN: return TreeKeyAction::Activate;
        case VK_ESCAPE: return TreeKeyAction::Leave;
        case 'T': return ctrlHeld ? TreeKeyAction::Leave : TreeKeyAction::None;
        default: return TreeKeyAction::None;
    }
}

void DirectoryTree::CollapseCursor(int visibleRows) {
    if (nodes_[cursorNode_].expanded) Collapse(cursorNode_);
    ClampToVisible(visibleRows);
}

void DirectoryTree::ExpandCursor(int visibleRows) {
    Expand(cursorNode_);
    ClampToVisible(visibleRows);
}

void DirectoryTree::CollectVisible(size_t index, std::vector<size_t>& out) const {
    out.push_back(index);
    if (nodes_[index].expanded) {
        for (size_t child : nodes_[index].childIndices) CollectVisible(child, out);
    }
}

std::vector<size_t> DirectoryTree::VisibleNodeIndices() const {
    std::vector<size_t> result;
    CollectVisible(0, result);
    return result;
}

void DirectoryTree::ClampToVisible(int visibleRows) {
    std::vector<size_t> visible = VisibleNodeIndices();
    if (visible.empty()) {
        cursorNode_ = 0;
        topVisiblePos_ = 0;
        return;
    }
    auto it = std::find(visible.begin(), visible.end(), cursorNode_);
    while (it == visible.end() && nodes_[cursorNode_].parentIndex != SIZE_MAX) {
        cursorNode_ = nodes_[cursorNode_].parentIndex;
        it = std::find(visible.begin(), visible.end(), cursorNode_);
    }
    if (it == visible.end()) {
        cursorNode_ = visible.front();
        it = visible.begin();
    }
    int pos = static_cast<int>(std::distance(visible.begin(), it));
    if (pos < topVisiblePos_) topVisiblePos_ = pos;
    if (pos >= topVisiblePos_ + visibleRows) topVisiblePos_ = pos - visibleRows + 1;
    topVisiblePos_ = std::max(0, topVisiblePos_);
}

void DirectoryTree::MoveCursor(int delta, int visibleRows) {
    std::vector<size_t> visible = VisibleNodeIndices();
    if (visible.empty()) return;
    auto it = std::find(visible.begin(), visible.end(), cursorNode_);
    int pos = it != visible.end() ? static_cast<int>(std::distance(visible.begin(), it)) : 0;
    pos = std::clamp(pos + delta, 0, static_cast<int>(visible.size()) - 1);
    cursorNode_ = visible[static_cast<size_t>(pos)];
    ClampToVisible(visibleRows);
}

void DirectoryTree::PageMove(int pageDelta, int visibleRows) { MoveCursor(pageDelta * visibleRows, visibleRows); }

void DirectoryTree::ScrollToCursor(int visibleRows) { ClampToVisible(visibleRows); }

void DirectoryTree::CursorHome() {
    std::vector<size_t> visible = VisibleNodeIndices();
    if (!visible.empty()) cursorNode_ = visible.front();
    topVisiblePos_ = 0;
}

void DirectoryTree::CursorEnd(int visibleRows) {
    std::vector<size_t> visible = VisibleNodeIndices();
    if (!visible.empty()) cursorNode_ = visible.back();
    ClampToVisible(visibleRows);
}

void DirectoryTree::SelectVisibleRow(int visibleRow, int visibleRows) {
    std::vector<size_t> visible = VisibleNodeIndices();
    int absolute = topVisiblePos_ + visibleRow;
    if (absolute < 0 || absolute >= static_cast<int>(visible.size())) return;
    cursorNode_ = visible[static_cast<size_t>(absolute)];
    ClampToVisible(visibleRows);
}

} // namespace mc
