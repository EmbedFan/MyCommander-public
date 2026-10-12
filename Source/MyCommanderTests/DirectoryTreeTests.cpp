#include "TestFixtures.h"
#include "TestFramework.h"

#include "DirectoryTree.h"

#include <filesystem>

namespace fs = std::filesystem;

using mc::ClassifyTreeKey;
using mc::DirectoryTree;
using mc::TreeKeyAction;
using mc::ExpansionChain;
using mc::FindFilesystemRoot;
using mc::ListSubdirectories;
using mc::TreeDirEntry;
using test::CreateJunction;
using test::TempDir;

namespace {

void MakeDir(const fs::path& p) { fs::create_directories(p); }

} // namespace

// --- ListSubdirectories ------------------------------------------------------

TEST_CASE(ListSubdirectories_OnlyDirectoriesSortedCaseInsensitive) {
    TempDir dir;
    MakeDir(dir.Path() / L"beta");
    MakeDir(dir.Path() / L"Alpha");
    MakeDir(dir.Path() / L"gamma");
    test::WriteFileContent(dir.Path() / L"not_a_dir.txt", "x");

    auto listed = ListSubdirectories(dir.Path());
    CHECK(listed.has_value());
    if (listed) {
        CHECK(listed->size() == 3);
        if (listed->size() == 3) {
            CHECK((*listed)[0].name == L"Alpha");
            CHECK((*listed)[1].name == L"beta");
            CHECK((*listed)[2].name == L"gamma");
        }
    }
}

TEST_CASE(ListSubdirectories_EmptyForLeafDirectory) {
    TempDir dir;
    auto listed = ListSubdirectories(dir.Path());
    CHECK(listed.has_value());
    if (listed) CHECK(listed->empty());
}

TEST_CASE(ListSubdirectories_ReparsePointChildIsListedAndMarked) {
    TempDir dir;
    fs::path target = dir.Path() / L"real_target";
    MakeDir(target);
    fs::path link = dir.Path() / L"link_to_target";
    if (!CreateJunction(link, target)) return;  // skip gracefully if junctions aren't possible here

    auto listed = ListSubdirectories(dir.Path());
    CHECK(listed.has_value());
    if (listed) {
        CHECK(listed->size() == 2);
        bool foundLink = false;
        for (const auto& entry : *listed) {
            if (entry.name == L"link_to_target") {
                foundLink = true;
                CHECK(entry.isReparsePoint);
            }
        }
        CHECK(foundLink);
    }
}

TEST_CASE(ListSubdirectories_NonexistentDirectoryReturnsNullopt) {
    TempDir dir;
    CHECK(!ListSubdirectories(dir.Path() / L"does_not_exist").has_value());
}

// --- FindFilesystemRoot -------------------------------------------------------

TEST_CASE(FindFilesystemRoot_ReturnsTheContainingDriveRoot) {
    TempDir dir;
    fs::path expectedRoot = dir.Path().root_path();
    CHECK(FindFilesystemRoot(dir.Path()) == expectedRoot);
    CHECK(FindFilesystemRoot(expectedRoot) == expectedRoot);  // already a root
}

// --- ExpansionChain ------------------------------------------------------------

TEST_CASE(ExpansionChain_TargetUnderRootReturnsFullChain) {
    TempDir dir;
    fs::path a = dir.Path() / L"a";
    fs::path ab = a / L"b";
    MakeDir(ab);

    auto chain = ExpansionChain(dir.Path(), ab);
    CHECK(chain.size() == 3);
    if (chain.size() == 3) {
        CHECK(chain[0] == dir.Path());
        CHECK(chain[1] == a);
        CHECK(chain[2] == ab);
    }
}

TEST_CASE(ExpansionChain_TargetEqualsRootReturnsSingleEntry) {
    TempDir dir;
    auto chain = ExpansionChain(dir.Path(), dir.Path());
    CHECK(chain.size() == 1);
    if (chain.size() == 1) CHECK(chain[0] == dir.Path());
}

TEST_CASE(ExpansionChain_TargetNotUnderRootReturnsRootOnly) {
    TempDir dirA;
    TempDir dirB;
    auto chain = ExpansionChain(dirA.Path(), dirB.Path());
    CHECK(chain.size() == 1);
    if (chain.size() == 1) CHECK(chain[0] == dirA.Path());
}

// --- DirectoryTree ---------------------------------------------------------------

TEST_CASE(DirectoryTree_ConstructorAutoExpandsDownToInitialSelection) {
    TempDir dir;
    fs::path a = dir.Path() / L"a";
    fs::path ab = a / L"b";
    MakeDir(ab);

    DirectoryTree tree(dir.Path(), ab);
    CHECK(tree.CursorPath() == ab);

    auto visible = tree.VisibleNodeIndices();
    // root, "a" (expanded to reveal "b"), and "b" itself should all be visible.
    CHECK(visible.size() == 3);
}

TEST_CASE(DirectoryTree_ExpandLoadsChildrenOnlyOnce) {
    TempDir dir;
    MakeDir(dir.Path() / L"child1");
    DirectoryTree tree(dir.Path(), dir.Path());

    tree.ExpandCursor(10);
    CHECK(tree.VisibleNodeIndices().size() == 2);  // root + child1

    MakeDir(dir.Path() / L"child2");  // added after the first expand
    tree.CollapseCursorOrGoToParent(10);
    tree.ExpandCursor(10);
    // Still 2 -- the second Expand() on an already-loaded node must not rescan.
    CHECK(tree.VisibleNodeIndices().size() == 2);
}

TEST_CASE(DirectoryTree_CollapseHidesDescendantsFromVisibleList) {
    TempDir dir;
    fs::path a = dir.Path() / L"a";
    MakeDir(a / L"b");

    DirectoryTree tree(dir.Path(), a / L"b");
    CHECK(tree.VisibleNodeIndices().size() == 3);

    // Move the cursor up to "a" and collapse it.
    tree.MoveCursor(-1, 10);
    tree.CollapseCursorOrGoToParent(10);
    CHECK(tree.VisibleNodeIndices().size() == 2);  // root + "a", "b" now hidden
}

TEST_CASE(DirectoryTree_CollapseCursorOrGoToParent_MovesToParentWhenAlreadyCollapsed) {
    TempDir dir;
    fs::path a = dir.Path() / L"a";
    MakeDir(a);
    DirectoryTree tree(dir.Path(), a);
    CHECK(tree.CursorPath() == a);

    // "a" has no children loaded/expanded yet -- Left should go to its parent.
    tree.CollapseCursorOrGoToParent(10);
    CHECK(tree.CursorPath() == dir.Path());

    // Already at the root: the root has no parent, so this collapses it
    // instead (root.parentIndex == SIZE_MAX) -- it doesn't crash, and the
    // cursor stays put either way.
    tree.CollapseCursorOrGoToParent(10);
    CHECK(tree.CursorPath() == dir.Path());
}

TEST_CASE(DirectoryTree_MoveCursor_ClampsAtBothEnds) {
    TempDir dir;
    MakeDir(dir.Path() / L"a");
    MakeDir(dir.Path() / L"b");
    DirectoryTree tree(dir.Path(), dir.Path());
    tree.ExpandCursor(10);
    CHECK(tree.VisibleNodeIndices().size() == 3);

    tree.MoveCursor(-5, 10);
    CHECK(tree.CursorPath() == dir.Path());

    tree.MoveCursor(5, 10);
    CHECK(tree.CursorPath() == dir.Path() / L"b");
}

TEST_CASE(DirectoryTree_PageMoveAndHomeEnd) {
    TempDir dir;
    for (int i = 0; i < 5; ++i) MakeDir(dir.Path() / (L"d" + std::to_wstring(i)));
    DirectoryTree tree(dir.Path(), dir.Path());
    tree.ExpandCursor(3);  // visibleRows smaller than the 6 total nodes (root + 5 children)

    tree.CursorEnd(3);
    CHECK(tree.CursorPath() == dir.Path() / L"d4");

    tree.CursorHome();
    CHECK(tree.CursorPath() == dir.Path());

    tree.PageMove(1, 3);
    CHECK(tree.CursorPath() == dir.Path() / L"d2");  // root(0) + 3 rows down -> index 3 -> "d2"
}

TEST_CASE(DirectoryTree_SelectVisibleRow_MovesCursorToClickedRow) {
    TempDir dir;
    MakeDir(dir.Path() / L"a");
    MakeDir(dir.Path() / L"b");
    DirectoryTree tree(dir.Path(), dir.Path());
    tree.ExpandCursor(10);

    tree.SelectVisibleRow(2, 10);  // root(0), a(1), b(2)
    CHECK(tree.CursorPath() == dir.Path() / L"b");

    tree.SelectVisibleRow(99, 10);  // out of range -- no-op
    CHECK(tree.CursorPath() == dir.Path() / L"b");
}

TEST_CASE(DirectoryTree_RootAlwaysExpandedSoItsChildrenShow) {
    TempDir dir;
    MakeDir(dir.Path() / L"a");
    MakeDir(dir.Path() / L"b");
    DirectoryTree tree(dir.Path(), dir.Path());
    CHECK(tree.CursorPath() == dir.Path());
    CHECK(tree.VisibleNodeIndices().size() == 3);  // root plus both children
}

TEST_CASE(DirectoryTree_EmptyRootHasExactlyOneVisibleNode) {
    TempDir dir;
    DirectoryTree tree(dir.Path(), dir.Path());
    CHECK(tree.VisibleNodeIndices().size() == 1);
    CHECK(tree.CursorPath() == dir.Path());
}

// --- IS-0017 Part 4: scroll to the selected folder ----------------------------

TEST_CASE(DirectoryTree_ScrollToCursorShowsDeepSelectionInWindow) {
    TempDir dir;
    for (int i = 0; i < 30; ++i) {
        wchar_t name[16];
        swprintf_s(name, L"d%02d", i);
        MakeDir(dir.Path() / name);
    }
    DirectoryTree tree(dir.Path(), dir.Path() / L"d24");
    tree.ScrollToCursor(10);
    int top = tree.TopVisiblePosition();
    CHECK(top >= 16);  // d24 is at position 25 among the visible nodes (root is 0)
    CHECK(top <= 25 && 25 < top + 10);
}

TEST_CASE(DirectoryTree_ScrollToCursorIsNoOpWhenAlreadyVisible) {
    TempDir dir;
    MakeDir(dir.Path() / L"a");
    MakeDir(dir.Path() / L"b");
    DirectoryTree tree(dir.Path(), dir.Path() / L"b");
    tree.ScrollToCursor(10);
    CHECK(tree.TopVisiblePosition() == 0);
}

TEST_CASE(DirectoryTree_ScrollToCursorAfterResizeKeepsCursorVisible) {
    TempDir dir;
    for (int i = 0; i < 30; ++i) {
        wchar_t name[16];
        swprintf_s(name, L"d%02d", i);
        MakeDir(dir.Path() / name);
    }
    DirectoryTree tree(dir.Path(), dir.Path() / L"d24");
    tree.ScrollToCursor(20);
    tree.ScrollToCursor(5);  // terminal shrank
    int top = tree.TopVisiblePosition();
    CHECK(top <= 25 && 25 < top + 5);
}

// --- IS-0017 Part 2: +/- keys and the key mapping -----------------------------

TEST_CASE(ClassifyTreeKey_PlusAndMinusOnBothKeyboards) {
    CHECK(ClassifyTreeKey(VK_ADD, false) == TreeKeyAction::ExpandCursor);
    CHECK(ClassifyTreeKey(VK_OEM_PLUS, false) == TreeKeyAction::ExpandCursor);
    CHECK(ClassifyTreeKey(VK_SUBTRACT, false) == TreeKeyAction::CollapseCursor);
    CHECK(ClassifyTreeKey(VK_OEM_MINUS, false) == TreeKeyAction::CollapseCursor);
}

// IS-0018: on the Hungarian layout '+' is Shift+3 (virtual key '3'), so the
// typed character decides, whatever the physical key.
TEST_CASE(ClassifyTreeKey_TypedPlusAndMinusOnAnyLayout_IS0018) {
    CHECK(ClassifyTreeKey('3', false, L'+') == TreeKeyAction::ExpandCursor);
    CHECK(ClassifyTreeKey(VK_OEM_MINUS, false, L'-') == TreeKeyAction::CollapseCursor);
    CHECK(ClassifyTreeKey(VK_ADD, false, L'+') == TreeKeyAction::ExpandCursor);  // numpad, with its char
    CHECK(ClassifyTreeKey('3', false, L'3') == TreeKeyAction::None);              // a plain digit still does nothing
    CHECK(ClassifyTreeKey(VK_SUBTRACT, false, 0) == TreeKeyAction::CollapseCursor);
}

TEST_CASE(ClassifyTreeKey_LeftAndRightKeepTheirMeaning) {
    CHECK(ClassifyTreeKey(VK_LEFT, false) == TreeKeyAction::CollapseOrGoToParent);
    CHECK(ClassifyTreeKey(VK_RIGHT, false) == TreeKeyAction::ExpandCursor);
}

TEST_CASE(ClassifyTreeKey_EnterLeaveAndCtrlTToggle) {
    CHECK(ClassifyTreeKey(VK_RETURN, false) == TreeKeyAction::Activate);
    CHECK(ClassifyTreeKey(VK_ESCAPE, false) == TreeKeyAction::Leave);
    CHECK(ClassifyTreeKey('T', true) == TreeKeyAction::Leave);
    CHECK(ClassifyTreeKey('T', false) == TreeKeyAction::None);
}

TEST_CASE(ClassifyTreeKey_UnusedKeyIsNone) {
    CHECK(ClassifyTreeKey(VK_F5, false) == TreeKeyAction::None);
}

TEST_CASE(DirectoryTree_CollapseCursorKeepsCursorWhereItIs) {
    TempDir dir;
    fs::path a = dir.Path() / L"a";
    MakeDir(a / L"b");
    DirectoryTree tree(dir.Path(), a / L"b");
    tree.MoveCursor(-1, 10);            // cursor on "a", which is expanded
    CHECK(tree.CursorPath() == a);
    tree.CollapseCursor(10);
    CHECK(tree.CursorPath() == a);      // unlike Left, it does not move to the parent
    CHECK(tree.VisibleNodeIndices().size() == 2);
}

TEST_CASE(DirectoryTree_CollapseCursorOnCollapsedNodeIsNoOp) {
    TempDir dir;
    MakeDir(dir.Path() / L"a");
    DirectoryTree tree(dir.Path(), dir.Path());
    tree.MoveCursor(1, 10);             // cursor on "a", never expanded
    size_t before = tree.VisibleNodeIndices().size();
    tree.CollapseCursor(10);
    CHECK(tree.CursorPath() == dir.Path() / L"a");
    CHECK(tree.VisibleNodeIndices().size() == before);
}

