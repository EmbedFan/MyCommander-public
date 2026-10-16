#include "TestFixtures.h"
#include "TestFramework.h"

#include "PathCompletion.h"

using namespace mc;
namespace fs = std::filesystem;

TEST_CASE(PathCompletion_ExtractToken_SplitsDirAndName_NAV018) {
    PathToken t = ExtractPathToken(L"C:\\a\\b");
    CHECK(t.dir == L"C:\\a\\");
    CHECK(t.name == L"b");
    CHECK(t.start == 0);
    t = ExtractPathToken(L"C:\\a\\");
    CHECK(t.dir == L"C:\\a\\");
    CHECK(t.name.empty());
    t = ExtractPathToken(L"copy src\\fi");
    CHECK(t.start == 5);
    CHECK(t.dir == L"src\\");
    CHECK(t.name == L"fi");
    t = ExtractPathToken(L"b");
    CHECK(t.dir.empty());
    CHECK(t.name == L"b");
    t = ExtractPathToken(L"type \"C:\\Program F");  // inside an open quote the space belongs to the path
    CHECK(t.quoted);
    CHECK(t.dir == L"C:\\");
    CHECK(t.name == L"Program F");
    t = ExtractPathToken(L"C:/x/y");
    CHECK(t.dir == L"C:/x/");
}

static std::vector<CompletionEntry> Sample() {
    return {{L"Documents", true, false}, {L"docs.txt", false, false}, {L"Downloads", true, false},
            {L".git", true, true},       {L"desktop.ini", false, true}, {L"Music", true, false}};
}

TEST_CASE(PathCompletion_Filter_PrefixCaseHiddenOrder_NAV018) {
    auto c = FilterCompletions(Sample(), L"do", false);
    CHECK(c.size() == 3);
    if (c.size() == 3) {
        CHECK(c[0].name == L"docs.txt");  // case-insensitive order
        CHECK(c[1].name == L"Documents");
        CHECK(c[2].name == L"Downloads");
    }
    CHECK(FilterCompletions(Sample(), L"de", false).empty());  // hidden/system excluded
    CHECK(FilterCompletions(Sample(), L"de", true).size() == 1);
    auto dot = FilterCompletions(Sample(), L".", false);  // a leading dot asks for hidden entries
    CHECK(dot.size() == 1);
    CHECK(FilterCompletions(Sample(), L"", false).size() == 4);
}

TEST_CASE(PathCompletion_CommonPrefix_NAV018) {
    CHECK(CommonCompletionPrefix(FilterCompletions(Sample(), L"do", false)) == L"do");
    CHECK(CommonCompletionPrefix({{L"Document", true, false}, {L"docs", false, false}}) == L"Doc");  // first spelling
    CHECK(CommonCompletionPrefix({{L"Only", false, false}}) == L"Only");
    CHECK(CommonCompletionPrefix({}).empty());
}

TEST_CASE(PathCompletion_Cycle_NAV018) {
    CHECK(NextCompletionIndex(-1, 3) == 0);
    CHECK(NextCompletionIndex(0, 3) == 1);
    CHECK(NextCompletionIndex(2, 3) == 0);
    CHECK(NextCompletionIndex(-1, 1) == 0);
    CHECK(NextCompletionIndex(0, 1) == 0);
    CHECK(NextCompletionIndex(0, 0) == -1);
}

TEST_CASE(PathCompletion_Apply_KeepsPrecedingTextAndAddsSeparatorForFolders_NAV018) {
    const std::wstring text = L"copy C:\\Users\\att";
    PathToken t = ExtractPathToken(text);
    CHECK(ApplyCompletion(text, t, {L"attila", true, false}, true) == L"copy C:\\Users\\attila\\");
    CHECK(ApplyCompletion(text, t, {L"attrib.txt", false, false}, true) == L"copy C:\\Users\\attrib.txt");
    CHECK(ApplyCompletion(text, t, {L"attila", true, false}, false) == L"copy C:\\Users\\attila");
    CHECK(ApplyCompletionPrefix(text, t, L"atti") == L"copy C:\\Users\\atti");
}

TEST_CASE(PathCompletion_ListFolder_RealDirectory_NAV018) {
    test::TempDir dir;
    fs::create_directories(dir.Path() / L"Alpha");
    test::WriteFileContent(dir.Path() / L"alps.txt", "x");
    test::WriteFileContent(dir.Path() / L"beta.txt", "x");
    auto all = ListFolderForCompletion(dir.Path());
    CHECK(all.size() == 3);
    auto c = FilterCompletions(all, L"AL", false);
    CHECK(c.size() == 2);
    if (c.size() == 2) {
        CHECK(c[0].name == L"Alpha");  // "alpha" < "alps"
        CHECK(c[0].isDirectory);
        CHECK(c[1].name == L"alps.txt");
    }
    CHECK(ListFolderForCompletion(dir.Path() / L"missing").empty());
}
