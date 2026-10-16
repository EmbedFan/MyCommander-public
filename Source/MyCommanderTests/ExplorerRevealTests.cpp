#include "TestFramework.h"

#include "ExplorerReveal.h"

using namespace mc;

TEST_CASE(RevealTargetFor_CursorEntryOrFirstSelected_NAV019) {
    RevealTarget t = RevealTargetFor(L"C:\\data", {L"file.txt"});
    CHECK(t.folder == L"C:\\data");
    CHECK(t.item.has_value() && *t.item == L"file.txt");
    CHECK(!t.moreThanOne);

    t = RevealTargetFor(L"C:\\data", {L"b.txt", L"c.txt"});  // listing order: the first one wins
    CHECK(t.item.has_value() && *t.item == L"b.txt");
    CHECK(t.moreThanOne);
}

TEST_CASE(RevealTargetFor_ParentOnlyOrEmpty_OpensFolderWithoutSelection_NAV019) {
    RevealTarget t = RevealTargetFor(L"C:\\data", {L".."});
    CHECK(!t.item.has_value());
    CHECK(!t.moreThanOne);
    t = RevealTargetFor(L"C:\\data", {});
    CHECK(!t.item.has_value());
    t = RevealTargetFor(L"C:\\data", {L"..", L"x.txt"});  // ".." is ignored, not counted
    CHECK(t.item.has_value() && *t.item == L"x.txt");
    CHECK(!t.moreThanOne);
}

TEST_CASE(BuildExplorerSelectArguments_QuotesPathWithSpaceAmpersandUnicode_NAV019) {
    const std::wstring args = BuildExplorerSelectArguments(L"C:\\My Files\\Tom & Jerry \u00c1rv\u00edz.txt");
    CHECK(args == L"/select,\"C:\\My Files\\Tom & Jerry \u00c1rv\u00edz.txt\"");
}
