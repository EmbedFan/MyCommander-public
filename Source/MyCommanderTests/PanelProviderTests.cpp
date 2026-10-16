#include "TestFramework.h"
#include "TestFixtures.h"

#include "MemoryProvider.h"
#include "Panel.h"

using mc::MemoryProvider;
using mc::Panel;

namespace {

std::shared_ptr<MemoryProvider> SampleTree() {
    auto p = std::make_shared<MemoryProvider>(L"mem://test");
    p->AddFile(L"/b.txt", "bbbb", 200);
    p->AddFile(L"/a.log", "aaaaaaaaaa", 100);
    p->AddFile(L"/docs/readme.md", "r", 300);
    p->AddDirectory(L"/zeta");
    return p;
}

std::vector<std::wstring> Names(const Panel& panel) {
    std::vector<std::wstring> names;
    for (const auto& e : panel.Entries()) names.push_back(e.name);
    return names;
}

int IndexOf(const Panel& panel, const std::wstring& name) {
    for (int i = 0; i < static_cast<int>(panel.Entries().size()); ++i)
        if (panel.Entries()[i].name == name) return i;
    return -1;
}

void MoveTo(Panel& panel, const std::wstring& name) {
    panel.MoveCursor(IndexOf(panel, name) - panel.Cursor(), 20);
}

}  // namespace

TEST_CASE(PanelProvider_Open_ListsThroughTheSharedCode_DirectoriesFirst_EXT002) {
    test::TempDir dir;
    Panel panel(dir.Path());
    panel.OpenProvider(SampleTree(), L"/");
    CHECK(panel.InProvider());
    CHECK(panel.LocationText() == L"mem://test/");
    CHECK(Names(panel) == (std::vector<std::wstring>{L"..", L"docs", L"zeta", L"a.log", L"b.txt"}));
    CHECK(panel.Path() == dir.Path());  // the local folder underneath is kept
    CHECK(panel.ItemLocation(L"a.log") == L"/a.log");
    const auto& a = panel.Entries()[IndexOf(panel, L"a.log")];
    CHECK(a.sizeBytes == 10 && a.lastWriteTimeUtc == 100 && !a.isDirectory);
}

TEST_CASE(PanelProvider_SortFilterAndSelection_WorkAsOnDisk_EXT002) {
    test::TempDir dir;
    Panel panel(dir.Path());
    panel.OpenProvider(SampleTree(), L"/");
    panel.SetSort(mc::SortKey::Size, true);  // descending also reverses the name tie-break
    CHECK(Names(panel) == (std::vector<std::wstring>{L"..", L"zeta", L"docs", L"a.log", L"b.txt"}));
    panel.SetSort(mc::SortKey::ModifiedTime, false);
    CHECK(IndexOf(panel, L"a.log") < IndexOf(panel, L"b.txt"));
    panel.SetFilter(L"*.txt");
    CHECK(Names(panel) == (std::vector<std::wstring>{L"..", L"b.txt"}));
    panel.SetFilter(L"");
    panel.SelectByMask(L"*.log", true);
    CHECK(panel.SelectionOrCursor() == std::vector<std::wstring>{L"a.log"});
    CHECK(panel.SelectedSizeBytes() == 10);
}

TEST_CASE(PanelProvider_EnterAndParent_SelectTheFolderComeOutOf_EXT002) {
    test::TempDir dir;
    Panel panel(dir.Path());
    panel.OpenProvider(SampleTree(), L"/");
    MoveTo(panel, L"docs");
    panel.EnterSelected();
    CHECK(panel.ProviderPath() == L"/docs");
    CHECK(Names(panel) == (std::vector<std::wstring>{L"..", L"readme.md"}));
    panel.GoToParent();
    CHECK(panel.ProviderPath() == L"/");
    CHECK(panel.Entries()[panel.Cursor()].name == L"docs");
}

TEST_CASE(PanelProvider_DotDotAtTheRoot_LeavesAndSelectsTheReturnName_EXT002) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"archive.zip", "z");
    test::WriteFileContent(dir.Path() / L"other.txt", "o");
    Panel panel(dir.Path());
    panel.OpenProvider(SampleTree(), L"/", L"archive.zip");
    MoveTo(panel, L"..");
    panel.EnterSelected();
    CHECK(!panel.InProvider());
    CHECK(panel.Path() == dir.Path());
    CHECK(panel.Entries()[panel.Cursor()].name == L"archive.zip");
}

TEST_CASE(PanelProvider_LeaveHook_CanKeepThePanelInside_EXT002) {
    test::TempDir dir;
    Panel panel(dir.Path());
    int asked = 0;
    bool allow = false;
    panel.OpenProvider(SampleTree(), L"/", L"", [&](mc::IFileProvider&) {
        ++asked;
        return allow;
    });
    panel.GoToParent();
    CHECK(panel.InProvider() && asked == 1);
    panel.NavigateTo(dir.Path());
    CHECK(panel.InProvider() && asked == 2);
    CHECK(panel.CloseProvider(true));  // force skips the hook
    CHECK(!panel.InProvider() && asked == 2);

    panel.OpenProvider(SampleTree(), L"/", L"", [&](mc::IFileProvider&) {
        ++asked;
        return true;
    });
    panel.NavigateTo(dir.Path());
    CHECK(!panel.InProvider() && asked == 3);
}

TEST_CASE(PanelProvider_BackAndForward_UseProviderHistoryThenLeave_EXT002) {
    test::TempDir dir;
    Panel panel(dir.Path());
    panel.OpenProvider(SampleTree(), L"/");
    panel.NavigateProvider(L"/docs");
    CHECK(panel.NavigateBack());
    CHECK(panel.ProviderPath() == L"/");
    CHECK(panel.NavigateForward());
    CHECK(panel.ProviderPath() == L"/docs");
    CHECK(panel.NavigateBack());
    CHECK(panel.NavigateBack());  // no provider history left: leaves
    CHECK(!panel.InProvider());
}

TEST_CASE(PanelProvider_ListFailure_MarksTheLocationInaccessible_EXT002_LIST006) {
    test::TempDir dir;
    Panel panel(dir.Path());
    auto tree = SampleTree();
    panel.OpenProvider(tree, L"/");
    tree->DropConnectionAfter(1);
    std::wstring err;
    std::unique_ptr<mc::IReadStream> in;
    tree->OpenRead(L"/b.txt", in, {}, err);
    size_t got = 0;
    char buffer[8];
    in->Read(buffer, sizeof(buffer), got, err);  // drops the connection
    panel.Refresh();
    CHECK(panel.LocationInaccessible());
    CHECK(!panel.StatusMessage().empty());
    CHECK(Names(panel) == std::vector<std::wstring>{L".."});
}

TEST_CASE(PanelProvider_RefreshPreservingState_KeepsSelectionAndCursor_EXT002_LIST007) {
    test::TempDir dir;
    Panel panel(dir.Path());
    auto tree = SampleTree();
    panel.OpenProvider(tree, L"/");
    panel.SelectByMask(L"b.txt", true);
    MoveTo(panel, L"a.log");
    tree->AddFile(L"/c.txt", "c");
    panel.RefreshPreservingState();
    CHECK(IndexOf(panel, L"c.txt") > 0);
    CHECK(panel.SelectionOrCursor() == std::vector<std::wstring>{L"b.txt"});
    CHECK(panel.Entries()[panel.Cursor()].name == L"a.log");
}

TEST_CASE(PanelProvider_LinkToFolder_IsMarkedAndOpens_EXT002) {
    test::TempDir dir;
    Panel panel(dir.Path());
    auto tree = SampleTree();
    tree->AddLink(L"/shortcut", L"/docs", true);
    panel.OpenProvider(tree, L"/");
    const auto& link = panel.Entries()[IndexOf(panel, L"shortcut")];
    CHECK(link.isReparsePoint && link.isDirectory);
    MoveTo(panel, L"shortcut");
    panel.EnterSelected();
    CHECK(panel.ProviderPath() == L"/shortcut");
    CHECK(Names(panel) == (std::vector<std::wstring>{L"..", L"readme.md"}));
}
