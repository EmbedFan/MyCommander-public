#include "TestFixtures.h"
#include "TestFramework.h"

#include "ClipboardPaste.h"
#include "FileOps.h"

#include <cstring>
#include <windows.h>

namespace fs = std::filesystem;

namespace {

std::vector<unsigned char> DropBuffer(const std::vector<std::wstring>& paths, bool wide = true) {
    std::vector<unsigned char> buf(20, 0);
    uint32_t offset = 20, w = wide ? 1 : 0;
    std::memcpy(buf.data(), &offset, 4);
    std::memcpy(buf.data() + 16, &w, 4);
    for (const std::wstring& p : paths) {
        if (wide) {
            const unsigned char* b = reinterpret_cast<const unsigned char*>(p.c_str());
            buf.insert(buf.end(), b, b + (p.size() + 1) * 2);
        } else {
            for (wchar_t c : p) buf.push_back(static_cast<unsigned char>(c));
            buf.push_back(0);
        }
    }
    buf.insert(buf.end(), wide ? 2 : 1, 0);  // list terminator
    return buf;
}

}  // namespace

TEST_CASE(ParseDropFiles_WideList_FOP025) {
    auto buf = DropBuffer({L"C:\\a\\one.txt", L"D:\\b\\\x6587\x4ef6.txt", L"C:\\a\\folder"});
    auto paths = mc::ParseDropFiles(buf.data(), buf.size());
    CHECK(paths.size() == 3);
    CHECK(paths[0] == fs::path(L"C:\\a\\one.txt"));
    CHECK(paths[1].filename() == fs::path(L"\x6587\x4ef6.txt"));
    CHECK(paths[2] == fs::path(L"C:\\a\\folder"));
}

TEST_CASE(ParseDropFiles_AnsiListAndBadInput_FOP025) {
    auto buf = DropBuffer({L"C:\\x\\a.txt", L"C:\\x\\b.txt"}, /*wide=*/false);
    auto paths = mc::ParseDropFiles(buf.data(), buf.size());
    CHECK(paths.size() == 2);
    CHECK(paths[1] == fs::path(L"C:\\x\\b.txt"));

    CHECK(mc::ParseDropFiles(nullptr, 0).empty());
    unsigned char tiny[8] = {};
    CHECK(mc::ParseDropFiles(tiny, sizeof(tiny)).empty());
    auto bad = DropBuffer({L"C:\\a.txt"});
    uint32_t hugeOffset = 4096;
    std::memcpy(bad.data(), &hugeOffset, 4);
    CHECK(mc::ParseDropFiles(bad.data(), bad.size()).empty());
    auto truncated = DropBuffer({L"C:\\a\\long.txt"});
    truncated.resize(truncated.size() - 6);  // cut into the string and its terminators
    CHECK(mc::ParseDropFiles(truncated.data(), truncated.size()).empty());
}

TEST_CASE(PlanPaste_GroupsBySourceFolderAndKeepsOrder_FOP025) {
    std::vector<fs::path> src{L"C:\\a\\1.txt", L"C:\\b\\2.txt", L"C:\\a\\3.txt", L"C:\\A\\4.txt"};
    mc::PastePlan p = mc::PlanPaste(src, L"C:\\dest", /*isCut=*/false);
    CHECK(!p.isMove);
    CHECK(p.groups.size() == 2);
    CHECK(p.groups[0].sourceDir == fs::path(L"C:\\a"));
    CHECK(p.groups[0].names.size() == 3);  // case-insensitive folder match, order kept
    CHECK(p.groups[0].names[0] == L"1.txt");
    CHECK(p.groups[0].names[1] == L"3.txt");
    CHECK(p.groups[0].names[2] == L"4.txt");
    CHECK(p.groups[1].sourceDir == fs::path(L"C:\\b"));
    CHECK(p.TotalItems() == 4);
    CHECK(p.dropped.empty());
}

TEST_CASE(PlanPaste_CutIsMove_FOP025) {
    mc::PastePlan p = mc::PlanPaste({L"C:\\a\\1.txt"}, L"C:\\dest", /*isCut=*/true);
    CHECK(p.isMove);
    CHECK(p.TotalItems() == 1);
}

TEST_CASE(PlanPaste_DropsSameFolderAndIntoItself_FOP025) {
    mc::PastePlan p = mc::PlanPaste({L"C:\\dest\\here.txt", L"C:\\data\\folder", L"C:\\data", L"C:\\other\\ok.txt"},
                                    L"C:\\data\\folder\\sub", false);
    // here.txt is not in the destination; folder and data are ancestors of the destination.
    CHECK(p.dropped.size() == 2);
    CHECK(p.TotalItems() == 2);
    mc::PastePlan q = mc::PlanPaste({L"C:\\dest\\Same.txt", L"C:\\x\\y.txt"}, L"c:\\DEST", false);
    CHECK(q.dropped.size() == 1);
    CHECK(q.dropped[0].second.find(L"already") != std::wstring::npos);
    CHECK(q.TotalItems() == 1);
    CHECK(mc::PlanPaste({}, L"C:\\d", false).groups.empty());
    CHECK(mc::PlanPaste({L"C:\\"}, L"C:\\d", false).dropped.size() == 1);
}

TEST_CASE(PlanPaste_ThroughCopyAndMoveEngines_FOP025) {
    test::TempDir dir;
    fs::path from = dir.Path() / L"from";
    fs::path to = dir.Path() / L"to";
    fs::create_directories(from / L"folder");
    fs::create_directories(to);
    test::WriteFileContent(from / L"a.txt", "aaa");
    test::WriteFileContent(from / L"folder" / L"b.txt", "bbb");

    auto refuse = [](const fs::path&) {
        mc::ConflictAskResult r;
        r.choice = mc::ConflictChoice::Cancel;
        return r;
    };
    // copy: source stays
    mc::PastePlan copy = mc::PlanPaste({from / L"a.txt", from / L"folder"}, to, false);
    CHECK(copy.groups.size() == 1);
    mc::OperationOutcome o = mc::CopyItems(copy.groups[0].sourceDir, copy.groups[0].names, to, refuse, nullptr, nullptr);
    CHECK(o.succeeded == 2);
    CHECK(fs::exists(from / L"a.txt"));
    CHECK(test::ReadFileContent(to / L"folder" / L"b.txt") == "bbb");

    // a conflict is asked, not overwritten
    mc::OperationOutcome again = mc::CopyItems(copy.groups[0].sourceDir, {L"a.txt"}, to, refuse, nullptr, nullptr);
    CHECK(again.cancelled || again.succeeded == 0);

    // cut: source removed
    fs::path other = dir.Path() / L"to2";
    fs::create_directories(other);
    mc::PastePlan cut = mc::PlanPaste({from / L"a.txt"}, other, true);
    CHECK(cut.isMove);
    mc::OperationOutcome m = mc::MoveItems(cut.groups[0].sourceDir, cut.groups[0].names, other, refuse, nullptr, nullptr);
    CHECK(m.succeeded == 1);
    CHECK(!fs::exists(from / L"a.txt"));
    CHECK(fs::exists(other / L"a.txt"));
}
