#include "TestFramework.h"
#include "TestFixtures.h"

#include "CheckSum.h"

#include <algorithm>

using mc::EnumerateFilesRecursively;
using mc::FormatSumLine;
using mc::HashAlgorithm;
using mc::HashAlgorithmFromHexLength;
using mc::HashAlgorithmName;
using mc::HashFile;
using mc::ParseSumFile;
using mc::SumEntry;
using mc::VerifyPlan;
using mc::VerifyStatus;

namespace {

std::wstring HashOf(const std::filesystem::path& file, HashAlgorithm algo) {
    std::wstring err;
    auto result = HashFile(file, algo, err);
    return result ? *result : (L"ERROR:" + err);
}

}  // namespace

// --- Known test vectors (empty string and "abc"), every algorithm ---------

TEST_CASE(CheckSum_MD5_EmptyAndAbc_MatchKnownVectors_FOP021) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"empty", "");
    test::WriteFileContent(dir.Path() / L"abc", "abc");
    CHECK(HashOf(dir.Path() / L"empty", HashAlgorithm::MD5) == L"d41d8cd98f00b204e9800998ecf8427e");
    CHECK(HashOf(dir.Path() / L"abc", HashAlgorithm::MD5) == L"900150983cd24fb0d6963f7d28e17f72");
}

TEST_CASE(CheckSum_SHA1_EmptyAndAbc_MatchKnownVectors_FOP021) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"empty", "");
    test::WriteFileContent(dir.Path() / L"abc", "abc");
    CHECK(HashOf(dir.Path() / L"empty", HashAlgorithm::SHA1) == L"da39a3ee5e6b4b0d3255bfef95601890afd80709");
    CHECK(HashOf(dir.Path() / L"abc", HashAlgorithm::SHA1) == L"a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST_CASE(CheckSum_SHA256_EmptyAndAbc_MatchKnownVectors_FOP021) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"empty", "");
    test::WriteFileContent(dir.Path() / L"abc", "abc");
    CHECK(HashOf(dir.Path() / L"empty", HashAlgorithm::SHA256) ==
         L"e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(HashOf(dir.Path() / L"abc", HashAlgorithm::SHA256) ==
         L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE(CheckSum_SHA512_EmptyAndAbc_MatchKnownVectors_FOP021) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"empty", "");
    test::WriteFileContent(dir.Path() / L"abc", "abc");
    CHECK(HashOf(dir.Path() / L"empty", HashAlgorithm::SHA512) ==
         L"cf83e1357eefb8bdf1542850d66d8007d620e4050b5715dc83f4a921d36ce9ce47d0d13c5d85f2b0ff8318d2877eec2f63b931bd4"
         L"7417a81a538327af927da3e");
    CHECK(HashOf(dir.Path() / L"abc", HashAlgorithm::SHA512) ==
         L"ddaf35a193617abacc417349ae20413112e6fa4e89a97ea20a9eeee64b55d39a2192992a274fc1a836ba3c23a3feebbd454d4423"
         L"643ce80e2a9ac94fa54ca49f");
}

// --- Name/length mapping ----------------------------------------------------

TEST_CASE(CheckSum_HashAlgorithmName_MatchesExtensionConvention_FOP021) {
    CHECK(HashAlgorithmName(HashAlgorithm::MD5) == L"md5");
    CHECK(HashAlgorithmName(HashAlgorithm::SHA1) == L"sha1");
    CHECK(HashAlgorithmName(HashAlgorithm::SHA256) == L"sha256");
    CHECK(HashAlgorithmName(HashAlgorithm::SHA512) == L"sha512");
}

TEST_CASE(CheckSum_HashAlgorithmFromHexLength_MapsEachDigestSize_FOP021) {
    CHECK(HashAlgorithmFromHexLength(32) == HashAlgorithm::MD5);
    CHECK(HashAlgorithmFromHexLength(40) == HashAlgorithm::SHA1);
    CHECK(HashAlgorithmFromHexLength(64) == HashAlgorithm::SHA256);
    CHECK(HashAlgorithmFromHexLength(128) == HashAlgorithm::SHA512);
    CHECK(!HashAlgorithmFromHexLength(7).has_value());
}

// --- Format / parse round trip ----------------------------------------------

TEST_CASE(CheckSum_FormatSumLine_UsesGnuBinaryMarker_FOP021) {
    CHECK(FormatSumLine(L"deadbeef", L"a.txt") == L"deadbeef *a.txt");
}

TEST_CASE(CheckSum_ParseSumFile_RoundTripsFormattedLines_FOP021) {
    std::wstring text = FormatSumLine(L"aaa111", L"a.txt") + L"\r\n" + FormatSumLine(L"bbb222", L"sub\\b.txt") + L"\n";
    auto entries = ParseSumFile(text);
    CHECK(entries.size() == 2);
    CHECK(entries[0].hash == L"aaa111");
    CHECK(entries[0].name == L"a.txt");
    CHECK(entries[1].hash == L"bbb222");
    CHECK(entries[1].name == L"sub\\b.txt");
}

TEST_CASE(CheckSum_ParseSumFile_AcceptsTextModeTwoSpaceForm_FOP021) {
    auto entries = ParseSumFile(L"aaa111  a.txt\n");
    CHECK(entries.size() == 1);
    CHECK(entries[0].hash == L"aaa111");
    CHECK(entries[0].name == L"a.txt");
}

TEST_CASE(CheckSum_ParseSumFile_SkipsBlankAndCommentLines_FOP021) {
    auto entries = ParseSumFile(L"# header comment\n\naaa111 *a.txt\n");
    CHECK(entries.size() == 1);
    CHECK(entries[0].name == L"a.txt");
}

// --- Verify plan -------------------------------------------------------------

TEST_CASE(CheckSum_VerifyPlan_MatchingHashIsOk_FOP021) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"a.txt", "abc");
    std::vector<SumEntry> entries = {{L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", L"a.txt"}};
    auto outcome = VerifyPlan(dir.Path(), entries);
    CHECK(outcome.ok == 1);
    CHECK(outcome.failed == 0);
    CHECK(outcome.missing == 0);
    CHECK(outcome.results[0].status == VerifyStatus::Ok);
}

TEST_CASE(CheckSum_VerifyPlan_MismatchedHashIsFailed_FOP021) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"a.txt", "abc");
    std::vector<SumEntry> entries = {{L"0000000000000000000000000000000000000000000000000000000000000000", L"a.txt"}};
    auto outcome = VerifyPlan(dir.Path(), entries);
    CHECK(outcome.failed == 1);
    CHECK(outcome.results[0].status == VerifyStatus::Failed);
}

TEST_CASE(CheckSum_VerifyPlan_MissingFileIsMissing_FOP021) {
    test::TempDir dir;
    std::vector<SumEntry> entries = {{L"ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", L"gone.txt"}};
    auto outcome = VerifyPlan(dir.Path(), entries);
    CHECK(outcome.missing == 1);
    CHECK(outcome.results[0].status == VerifyStatus::Missing);
}

TEST_CASE(CheckSum_VerifyPlan_CaseInsensitiveHashComparison_FOP021) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"a.txt", "abc");
    std::vector<SumEntry> entries = {{L"BA7816BF8F01CFEA414140DE5DAE2223B00361A396177A9CB410FF61F20015AD", L"a.txt"}};
    auto outcome = VerifyPlan(dir.Path(), entries);
    CHECK(outcome.ok == 1);
}

// --- EnumerateFilesRecursively (IS-0020: Generate Check Code on directories) ---

namespace {
bool Contains(const std::vector<std::wstring>& v, const std::wstring& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}
}  // namespace

TEST_CASE(CheckSum_EnumerateFilesRecursively_FlatDirectory_ListsEveryFile_IS0020) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"a.txt", "1");
    test::WriteFileContent(dir.Path() / L"b.txt", "2");
    auto result = EnumerateFilesRecursively(dir.Path());
    CHECK(result.relativePaths.size() == 2);
    CHECK(Contains(result.relativePaths, L"a.txt"));
    CHECK(Contains(result.relativePaths, L"b.txt"));
    CHECK(!result.cancelled);
    CHECK(result.unreadableFolders == 0);
}

TEST_CASE(CheckSum_EnumerateFilesRecursively_NestedDirectory_PrefixesWithTheSubfolderPath_IS0020) {
    test::TempDir dir;
    std::filesystem::create_directories(dir.Path() / L"sub" / L"deeper");
    test::WriteFileContent(dir.Path() / L"top.txt", "x");
    test::WriteFileContent(dir.Path() / L"sub" / L"mid.txt", "y");
    test::WriteFileContent(dir.Path() / L"sub" / L"deeper" / L"d.txt", "z");
    auto result = EnumerateFilesRecursively(dir.Path());
    CHECK(result.relativePaths.size() == 3);
    CHECK(Contains(result.relativePaths, L"top.txt"));
    CHECK(Contains(result.relativePaths, L"sub\\mid.txt"));
    CHECK(Contains(result.relativePaths, L"sub\\deeper\\d.txt"));
}

TEST_CASE(CheckSum_EnumerateFilesRecursively_EmptyDirectory_IsEmptyWithoutError_IS0020) {
    test::TempDir dir;
    auto result = EnumerateFilesRecursively(dir.Path());
    CHECK(result.relativePaths.empty());
    CHECK(result.unreadableFolders == 0);
    CHECK(!result.cancelled);
}

TEST_CASE(CheckSum_EnumerateFilesRecursively_ReparsePoint_IsNeverDescendedInto_IS0020_SEC006) {
    test::TempDir dir;
    std::filesystem::create_directories(dir.Path() / L"real");
    test::WriteFileContent(dir.Path() / L"real" / L"inside.txt", "x");
    if (!test::CreateJunction(dir.Path() / L"link", dir.Path() / L"real")) return;  // skip gracefully (TST-004)
    auto result = EnumerateFilesRecursively(dir.Path());
    // "real\\inside.txt" is found; nothing from "link" (not descended into).
    CHECK(result.relativePaths.size() == 1);
    CHECK(Contains(result.relativePaths, L"real\\inside.txt"));
}

TEST_CASE(CheckSum_EnumerateFilesRecursively_CancelPoll_StopsEarlyAndMarksCancelled_IS0020) {
    test::TempDir dir;
    for (int i = 0; i < 20; ++i) {
        test::WriteFileContent(dir.Path() / (L"f" + std::to_wstring(i) + L".txt"), "x");
    }
    int seen = 0;
    auto cancelPoll = [&]() { return ++seen > 3; };
    auto result = EnumerateFilesRecursively(dir.Path(), cancelPoll);
    CHECK(result.cancelled);
    CHECK(result.relativePaths.size() < 20);
}

TEST_CASE(CheckSum_EnumerateFilesRecursively_NonexistentPath_ReturnsEmptyWithoutCrashing_IS0020) {
    test::TempDir dir;
    auto result = EnumerateFilesRecursively(dir.Path() / L"does-not-exist");
    CHECK(result.relativePaths.empty());
    CHECK(!result.cancelled);
}
