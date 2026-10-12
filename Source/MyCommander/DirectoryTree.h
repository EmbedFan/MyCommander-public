#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mc {

// UI-013, Part B: one directory-tree entry as returned by ListSubdirectories
// -- just enough to build a TreeNode from, without yet touching the
// filesystem a second time.
struct TreeDirEntry {
    std::wstring name;
    bool isReparsePoint = false;
};

// Direct subdirectories of `dir` only, sorted by name (case-insensitive).
// A reparse-point subdirectory (a junction/symlink) is listed, never
// silently hidden (SEC-006's own precedent for recursive operations
// extended to browsing) -- DirectoryTree below still never auto-expands
// through one; the user has to do that explicitly. Returns nullopt if
// `dir` itself can't be enumerated at all (permissions, a broken parent
// junction) -- distinct from a legitimately empty directory, which
// returns an empty vector.
std::optional<std::vector<TreeDirEntry>> ListSubdirectories(const std::filesystem::path& dir);

// The filesystem root (a drive root or UNC share root, PathUtil.h's
// IsFilesystemRoot) containing `path`. A DirectoryTree's own root is
// always one of these, so browsing it never crosses onto a different
// drive or share.
std::filesystem::path FindFilesystemRoot(std::filesystem::path path);

// The chain of directories from `root` down to `target`, inclusive
// (just `{root}` if `target` isn't under `root`, or can't be confirmed to
// be). Used to auto-expand a DirectoryTree down to a starting directory
// when it's first shown, the way Explorer's own navigation pane opens
// already expanded to the current folder.
std::vector<std::filesystem::path> ExpansionChain(const std::filesystem::path& root,
                                                   const std::filesystem::path& target);

// One node in a DirectoryTree. `childIndices`/`parentIndex` index into the
// owning tree's own flat node storage, not a separate allocation per node.
struct TreeNode {
    std::filesystem::path path;
    int depth = 0;
    bool expanded = false;
    bool childrenLoaded = false;
    bool loadFailed = false;      // ListSubdirectories(path) returned nullopt
    bool isReparsePoint = false;
    std::vector<size_t> childIndices;
    size_t parentIndex = SIZE_MAX;  // SIZE_MAX for the root
};

// IS-0017 Part 2: what a key press does inside a directory tree. Pure, so
// the key mapping is unit-testable without a real Console (TST-005).
enum class TreeKeyAction {
    None,
    MoveUp,
    MoveDown,
    PageUp,
    PageDown,
    Home,
    End,
    ExpandCursor,
    CollapseCursor,
    CollapseOrGoToParent,
    Activate,
    Leave,
};

// A typed '+' / '-' character expands/collapses the cursor folder on any
// keyboard layout (IS-0018: on the Hungarian layout '+' is Shift+3, whose
// virtual key is '3', not VK_OEM_PLUS); numpad +/- and the US main-keyboard
// =/+ and -/_ keys are also recognized by virtual key. Left collapses or
// goes to the parent; Right expands. `unicodeChar` is the key event's typed
// character, 0 when there is none.
TreeKeyAction ClassifyTreeKey(uint16_t virtualKeyCode, bool ctrlHeld, wchar_t unicodeChar = 0);

// A lazily-loaded directory tree rooted at one filesystem root, with a
// cursor over its currently-visible (expanded) nodes. Pure aside from the
// real filesystem calls Expand/the constructor make -- no Console/Panel
// dependency (TST-004 precedent: tested against a real TempDir, not a
// mocked filesystem). A child's subdirectories are only ever listed once,
// on its own first Expand() -- LIST-008/PER-003's "never more I/O than
// what's visibly asked for" precedent.
class DirectoryTree {
public:
    // Builds just the root node, then auto-expands ExpansionChain(root,
    // initialSelection) one level at a time, leaving the deepest matched
    // node (ideally initialSelection itself) as the cursor, collapsed --
    // its own children aren't scanned until the user expands it.
    DirectoryTree(std::filesystem::path root, const std::filesystem::path& initialSelection);

    void Expand(size_t nodeIndex);
    void Collapse(size_t nodeIndex);

    // The Left-arrow convention: collapses the cursor node if it's
    // expanded; otherwise moves the cursor to its parent (a no-op already
    // at the root).
    void CollapseCursorOrGoToParent(int visibleRows);
    // The Right-arrow convention: expands the cursor node (lazy-loading
    // its children on first expand).
    void ExpandCursor(int visibleRows);
    // IS-0017 Part 2: collapses the cursor folder but, unlike
    // CollapseCursorOrGoToParent, never moves the cursor.
    void CollapseCursor(int visibleRows);

    void MoveCursor(int delta, int visibleRows);
    void PageMove(int pageDelta, int visibleRows);
    void CursorHome();
    // IS-0017 Part 4: brings the cursor's row into the visible window, scrolling
    // only as far as needed. Called every frame so the selection is on screen
    // when the tree opens, however deep it is, and stays there on resize.
    void ScrollToCursor(int visibleRows);
    void CursorEnd(int visibleRows);
    // Moves the cursor to the node at on-screen row `visibleRow` (0-based,
    // relative to the current scroll position) -- a mouse click's row. A
    // no-op if out of range.
    void SelectVisibleRow(int visibleRow, int visibleRows);

    size_t CursorNodeIndex() const { return cursorNode_; }
    const std::filesystem::path& CursorPath() const { return nodes_[cursorNode_].path; }
    // Every currently-visible node, root first, in on-screen display
    // order -- a node's children appear right after it only while it's
    // expanded.
    std::vector<size_t> VisibleNodeIndices() const;
    const TreeNode& NodeAt(size_t index) const { return nodes_[index]; }
    size_t NodeCount() const { return nodes_.size(); }
    int TopVisiblePosition() const { return topVisiblePos_; }

private:
    std::vector<TreeNode> nodes_;
    size_t cursorNode_ = 0;
    int topVisiblePos_ = 0;

    void CollectVisible(size_t index, std::vector<size_t>& out) const;
    void ClampToVisible(int visibleRows);
};

} // namespace mc
