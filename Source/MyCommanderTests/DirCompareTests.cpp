#include "TestFixtures.h"
#include "TestFramework.h"

#include "DirCompare.h"

#include <algorithm>
#include <cstdio>

namespace fs = std::filesystem;

using mc::ClassifyPair;
using mc::CompareDirectories;
using mc::CompareItem;
using mc::CompareMethod;
using mc::CompareOptions;
using mc::CompareOutcome;
using mc::CompareSide;
using mc::CompareStatus;

namespace {

constexpr uint64_t kSecond = 10'000'000ULL;
constexpr uint64_t kBase = 133'000'000'000'000'000ULL;  // some time in 2022

CompareSide File(uint64_t size, uint64_t time) {
    CompareSide s;
    s.size = size;
    s.lastWriteUtc = time;
    return s;
}

CompareSide Folder() {
    CompareSide s;
    s.isDirectory = true;
    s.lastWriteUtc = kBase;
    return s;
}

CompareOptions WithMethod(CompareMethod method) {
    CompareOptions o;
    o.method = method;
    return o;
}

void SetWriteTime(const fs::path& p, uint64_t ticks) {
    HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    FILETIME ft{static_cast<DWORD>(ticks & 0xFFFFFFFF), static_cast<DWORD>(ticks >> 32)};
    SetFileTime(h, nullptr, nullptr, &ft);
    CloseHandle(h);
}

void WriteAt(const fs::path& p, const std::string& content, uint64_t ticks) {
    test::WriteFileContent(p, content);
    SetWriteTime(p, ticks);
}

const CompareItem* Find(const CompareOutcome& out, const std::wstring& relativePath) {
    auto it = std::find_if(out.items.begin(), out.items.end(),
                           [&](const CompareItem& i) { return i.relativePath == relativePath; });
    return it == out.items.end() ? nullptr : &*it;
}

bool HasStatus(const CompareOutcome& out, const std::wstring& relativePath, CompareStatus status) {
    const CompareItem* item = Find(out, relativePath);
    return item != nullptr && item->status == status;
}

}  // namespace

// ---- ClassifyPair (pure) ---------------------------------------------------

TEST_CASE(DirCompare_Classify_EqualPairIsIdenticalUnderEveryMethod) {
    auto a = File(10, kBase), b = File(10, kBase);
    CHECK(ClassifyPair(a, b, WithMethod(CompareMethod::Name)) == CompareStatus::Identical);
    CHECK(ClassifyPair(a, b, WithMethod(CompareMethod::Size)) == CompareStatus::Identical);
    CHECK(ClassifyPair(a, b, WithMethod(CompareMethod::Timestamp)) == CompareStatus::Identical);
    CHECK(ClassifyPair(a, b, WithMethod(CompareMethod::Content), true) == CompareStatus::Identical);
}

TEST_CASE(DirCompare_Classify_OneSideMissing) {
    CompareOptions o;
    CHECK(ClassifyPair(File(1, kBase), std::nullopt, o) == CompareStatus::LeftOnly);
    CHECK(ClassifyPair(std::nullopt, Folder(), o) == CompareStatus::RightOnly);
}

TEST_CASE(DirCompare_Classify_FileVersusFolderIsTypeMismatch) {
    CHECK(ClassifyPair(File(1, kBase), Folder(), CompareOptions{}) == CompareStatus::TypeMismatch);
    CHECK(ClassifyPair(Folder(), Folder(), CompareOptions{}) == CompareStatus::Identical);
}

TEST_CASE(DirCompare_Classify_SizeDifference) {
    auto a = File(10, kBase), b = File(11, kBase);
    CHECK(ClassifyPair(a, b, WithMethod(CompareMethod::Name)) == CompareStatus::Identical);
    CHECK(ClassifyPair(a, b, WithMethod(CompareMethod::Size)) == CompareStatus::Different);
    CHECK(ClassifyPair(a, b, WithMethod(CompareMethod::Timestamp)) == CompareStatus::Different);
    CHECK(ClassifyPair(a, b, WithMethod(CompareMethod::Content)) == CompareStatus::Different);
}

TEST_CASE(DirCompare_Classify_TimestampToleranceAndNewerSide) {
    CompareOptions o = WithMethod(CompareMethod::Timestamp);
    CHECK(ClassifyPair(File(1, kBase + 2 * kSecond), File(1, kBase), o) == CompareStatus::Identical);
    CHECK(ClassifyPair(File(1, kBase + 3 * kSecond), File(1, kBase), o) == CompareStatus::LeftNewer);
    CHECK(ClassifyPair(File(1, kBase), File(1, kBase + 3 * kSecond), o) == CompareStatus::RightNewer);
    o.toleranceSeconds = 0;
    CHECK(ClassifyPair(File(1, kBase + 1), File(1, kBase), o) == CompareStatus::LeftNewer);
    // Name+size never looks at the time.
    CHECK(ClassifyPair(File(1, kBase + 99 * kSecond), File(1, kBase), WithMethod(CompareMethod::Size)) ==
          CompareStatus::Identical);
}

TEST_CASE(DirCompare_Classify_IgnoreExactOneHour) {
    CompareOptions o = WithMethod(CompareMethod::Timestamp);
    auto a = File(1, kBase + 3600 * kSecond), b = File(1, kBase);
    CHECK(ClassifyPair(a, b, o) == CompareStatus::LeftNewer);
    o.ignoreOneHour = true;
    CHECK(ClassifyPair(a, b, o) == CompareStatus::Identical);
    CHECK(ClassifyPair(File(1, kBase + 3601 * kSecond), b, o) == CompareStatus::Identical);  // within 2 s
    CHECK(ClassifyPair(File(1, kBase + 3605 * kSecond), b, o) == CompareStatus::LeftNewer);
}

TEST_CASE(DirCompare_Classify_ContentDecidesTimeOnlyGivesDirection) {
    CompareOptions o = WithMethod(CompareMethod::Content);
    // Equal content: identical even though the times differ.
    CHECK(ClassifyPair(File(5, kBase + 99 * kSecond), File(5, kBase), o, true) == CompareStatus::Identical);
    CHECK(ClassifyPair(File(5, kBase), File(5, kBase), o, false) == CompareStatus::Different);
    CHECK(ClassifyPair(File(5, kBase + 99 * kSecond), File(5, kBase), o, false) == CompareStatus::LeftNewer);
    CHECK(ClassifyPair(File(5, kBase), File(5, kBase), o, std::nullopt) == CompareStatus::Unreadable);
}

TEST_CASE(DirCompare_MarksSides) {
    CHECK(mc::MarksLeft(CompareStatus::LeftOnly) && !mc::MarksRight(CompareStatus::LeftOnly));
    CHECK(mc::MarksRight(CompareStatus::RightNewer) && !mc::MarksLeft(CompareStatus::RightNewer));
    CHECK(mc::MarksLeft(CompareStatus::Different) && mc::MarksRight(CompareStatus::Different));
    CHECK(!mc::MarksLeft(CompareStatus::Identical) && !mc::MarksRight(CompareStatus::Identical));
}

// ---- CompareDirectories (real filesystem) -----------------------------------

TEST_CASE(DirCompare_TopLevel_AllStatuses) {
    test::TempDir l, r;
    WriteAt(l.Path() / L"same.txt", "abc", kBase);
    WriteAt(r.Path() / L"same.txt", "abc", kBase);
    WriteAt(l.Path() / L"onlyleft.txt", "x", kBase);
    WriteAt(r.Path() / L"onlyright.txt", "x", kBase);
    WriteAt(l.Path() / L"newer.txt", "x", kBase + 100 * kSecond);
    WriteAt(r.Path() / L"newer.txt", "x", kBase);
    WriteAt(l.Path() / L"size.txt", "xx", kBase);
    WriteAt(r.Path() / L"size.txt", "x", kBase);
    WriteAt(l.Path() / L"Case.TXT", "x", kBase);
    WriteAt(r.Path() / L"case.txt", "x", kBase);
    fs::create_directory(l.Path() / L"dir");
    fs::create_directory(r.Path() / L"dir");
    fs::create_directory(l.Path() / L"mixed");
    WriteAt(r.Path() / L"mixed", "x", kBase);

    CompareOutcome out = CompareDirectories(l.Path(), r.Path(), CompareOptions{});
    CHECK(!out.cancelled && !out.truncated);
    CHECK(HasStatus(out, L"same.txt", CompareStatus::Identical));
    CHECK(HasStatus(out, L"onlyleft.txt", CompareStatus::LeftOnly));
    CHECK(HasStatus(out, L"onlyright.txt", CompareStatus::RightOnly));
    CHECK(HasStatus(out, L"newer.txt", CompareStatus::LeftNewer));
    CHECK(HasStatus(out, L"size.txt", CompareStatus::Different));
    CHECK(HasStatus(out, L"Case.TXT", CompareStatus::Identical));  // left spelling wins
    CHECK(Find(out, L"case.txt") == nullptr);
    CHECK(HasStatus(out, L"dir", CompareStatus::Identical));
    CHECK(HasStatus(out, L"mixed", CompareStatus::TypeMismatch));
    CHECK(out.items.size() == 8);
    // Folders first.
    CHECK(out.items[0].left && out.items[0].left->isDirectory);
    CHECK(out.items[1].left && out.items[1].left->isDirectory);
}

TEST_CASE(DirCompare_HiddenFilesOnlyWhenThatSideIncludesThem) {
    test::TempDir l, r;
    WriteAt(l.Path() / L"secret.txt", "x", kBase);
    SetFileAttributesW((l.Path() / L"secret.txt").c_str(), FILE_ATTRIBUTE_HIDDEN);

    CompareOutcome out = CompareDirectories(l.Path(), r.Path(), CompareOptions{});
    CHECK(out.items.empty());

    CompareOptions o;
    o.includeHiddenLeft = true;
    out = CompareDirectories(l.Path(), r.Path(), o);
    CHECK(HasStatus(out, L"secret.txt", CompareStatus::LeftOnly));
    SetFileAttributesW((l.Path() / L"secret.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
}

TEST_CASE(DirCompare_MaskFiltersFilesNotFolders) {
    test::TempDir l, r;
    WriteAt(l.Path() / L"a.cpp", "x", kBase);
    WriteAt(l.Path() / L"a.txt", "x", kBase);
    fs::create_directory(l.Path() / L"src");
    CompareOptions o;
    o.fileMaskLeft = L"*.cpp";
    CompareOutcome out = CompareDirectories(l.Path(), r.Path(), o);
    CHECK(HasStatus(out, L"a.cpp", CompareStatus::LeftOnly));
    CHECK(Find(out, L"a.txt") == nullptr);
    CHECK(HasStatus(out, L"src", CompareStatus::LeftOnly));
}

TEST_CASE(DirCompare_Recursive_DeepDifferenceRollsUpToTopFolder) {
    test::TempDir l, r;
    fs::create_directories(l.Path() / L"a" / L"b" / L"c");
    fs::create_directories(r.Path() / L"a" / L"b" / L"c");
    fs::create_directories(l.Path() / L"same" / L"x");
    fs::create_directories(r.Path() / L"same" / L"x");
    WriteAt(l.Path() / L"same" / L"x" / L"f.txt", "1", kBase);
    WriteAt(r.Path() / L"same" / L"x" / L"f.txt", "1", kBase);
    WriteAt(l.Path() / L"a" / L"b" / L"c" / L"f.txt", "12", kBase);
    WriteAt(r.Path() / L"a" / L"b" / L"c" / L"f.txt", "1", kBase);

    CompareOptions o;
    o.recursive = true;
    CompareOutcome out = CompareDirectories(l.Path(), r.Path(), o);
    CHECK(HasStatus(out, L"a\\b\\c\\f.txt", CompareStatus::Different));
    CHECK(HasStatus(out, L"a\\b\\c", CompareStatus::Different));
    CHECK(HasStatus(out, L"a", CompareStatus::Different));
    CHECK(HasStatus(out, L"same", CompareStatus::Identical));
    CHECK(HasStatus(out, L"same\\x\\f.txt", CompareStatus::Identical));
    const CompareItem* deep = Find(out, L"a\\b\\c\\f.txt");
    CHECK(deep != nullptr && deep->depth == 3 && deep->name == L"f.txt");
    // Depth-first: "a" comes right before its own children.
    CHECK(out.items[0].relativePath == L"a" && out.items[1].relativePath == L"a\\b");

    o.recursive = false;
    out = CompareDirectories(l.Path(), r.Path(), o);
    CHECK(HasStatus(out, L"a", CompareStatus::Identical));
    CHECK(out.items.size() == 2);
}

TEST_CASE(DirCompare_Recursive_JunctionNeverDescended_SEC006) {
    test::TempDir l, r, target;
    WriteAt(target.Path() / L"inside.txt", "x", kBase);
    if (!test::CreateJunction(l.Path() / L"link", target.Path()) || !test::CreateJunction(r.Path() / L"link", l.Path())) {
        std::printf("    (note: junction creation not possible here; skipped)\n");
        return;
    }
    CompareOptions o;
    o.recursive = true;
    CompareOutcome out = CompareDirectories(l.Path(), r.Path(), o);
    CHECK(HasStatus(out, L"link", CompareStatus::Identical));
    CHECK(Find(out, L"link\\inside.txt") == nullptr);
    CHECK(out.items.size() == 1);
}

TEST_CASE(DirCompare_Content_DetectsSingleByteChanges) {
    test::TempDir l, r;
    std::string big(200 * 1024, 'q');
    auto make = [&](const std::wstring& name, std::string rightContent) {
        WriteAt(l.Path() / name, big, kBase);
        WriteAt(r.Path() / name, rightContent, kBase);
    };
    make(L"equal.bin", big);
    std::string first = big, last = big, boundary = big;
    first[0] = 'Z';
    last[last.size() - 1] = 'Z';
    boundary[64 * 1024] = 'Z';
    make(L"first.bin", first);
    make(L"last.bin", last);
    make(L"boundary.bin", boundary);

    CompareOutcome out = CompareDirectories(l.Path(), r.Path(), WithMethod(CompareMethod::Content));
    CHECK(HasStatus(out, L"equal.bin", CompareStatus::Identical));
    CHECK(HasStatus(out, L"first.bin", CompareStatus::Different));
    CHECK(HasStatus(out, L"last.bin", CompareStatus::Different));
    CHECK(HasStatus(out, L"boundary.bin", CompareStatus::Different));
    CHECK(out.filesCompared == 4);
    CHECK(out.bytesRead > 0);
}

TEST_CASE(DirCompare_Content_DifferentSizesNeverRead) {
    test::TempDir l, r;
    WriteAt(l.Path() / L"f.bin", "abc", kBase);
    WriteAt(r.Path() / L"f.bin", "abcd", kBase);
    CompareOutcome out = CompareDirectories(l.Path(), r.Path(), WithMethod(CompareMethod::Content));
    CHECK(HasStatus(out, L"f.bin", CompareStatus::Different));
    CHECK(out.bytesRead == 0);
}

TEST_CASE(DirCompare_Content_LockedFileIsUnreadable) {
    test::TempDir l, r;
    WriteAt(l.Path() / L"f.bin", "abc", kBase);
    WriteAt(r.Path() / L"f.bin", "abc", kBase);
    HANDLE lock = CreateFileW((r.Path() / L"f.bin").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    CompareOutcome out = CompareDirectories(l.Path(), r.Path(), WithMethod(CompareMethod::Content));
    if (lock != INVALID_HANDLE_VALUE) CloseHandle(lock);
    CHECK(HasStatus(out, L"f.bin", CompareStatus::Unreadable));
    CHECK(out.unreadable == 1);
}

TEST_CASE(DirCompare_FilesHaveEqualContent_Direct) {
    test::TempDir d;
    test::WriteFileContent(d.Path() / L"a", "hello");
    test::WriteFileContent(d.Path() / L"b", "hello");
    test::WriteFileContent(d.Path() / L"c", "hellO");
    CHECK(mc::FilesHaveEqualContent(d.Path() / L"a", d.Path() / L"b") == std::optional<bool>(true));
    CHECK(mc::FilesHaveEqualContent(d.Path() / L"a", d.Path() / L"c") == std::optional<bool>(false));
    CHECK(!mc::FilesHaveEqualContent(d.Path() / L"a", d.Path() / L"missing").has_value());
}

TEST_CASE(DirCompare_CancelMidContentRead) {
    test::TempDir l, r;
    std::string big(1024 * 1024, 'q');
    WriteAt(l.Path() / L"f.bin", big, kBase);
    WriteAt(r.Path() / L"f.bin", big, kBase);
    int polls = 0;
    // Let the per-item poll pass, then cancel inside the content read.
    CompareOutcome out = CompareDirectories(l.Path(), r.Path(), WithMethod(CompareMethod::Content),
                                            [&] { return ++polls > 2; });
    CHECK(out.cancelled);
    CHECK(out.items.empty());
    CHECK(out.bytesRead < 2 * big.size());
}

TEST_CASE(DirCompare_CapSetsTruncated) {
    test::TempDir l, r;
    for (int i = 0; i < 10; ++i) WriteAt(l.Path() / (L"f" + std::to_wstring(i)), "x", kBase);
    CompareOptions o;
    o.maxItems = 4;
    CompareOutcome out = CompareDirectories(l.Path(), r.Path(), o);
    CHECK(out.truncated);
    CHECK(out.items.size() == 4);
}

TEST_CASE(DirCompare_UnlistableRootReported) {
    test::TempDir l;
    CompareOutcome out = CompareDirectories(l.Path(), l.Path() / L"does-not-exist", CompareOptions{});
    CHECK(out.leftListed);
    CHECK(!out.rightListed);
    CHECK(out.items.empty());
}
