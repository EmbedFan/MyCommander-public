#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mc {

// FOP-021: Generate Check Code / Verify. Pure core (no Console/Dialog
// dependency, matching DiskUsage.h's own precedent -- real file I/O, nothing
// UI-facing) with its own tests; main.cpp's DoGenerateCheckCode/DoVerifyCheckSum
// own the form, progress, and results list.

enum class HashAlgorithm { MD5, SHA1, SHA256, SHA512 };

// "md5"/"sha1"/"sha256"/"sha512" -- the file extension and the name shown in
// the algorithm choice field.
std::wstring HashAlgorithmName(HashAlgorithm algorithm);
// The algorithm whose hex digest is exactly this many characters long, or
// nullopt if no supported algorithm's digest has that length (VerifyPlan
// uses this to tell a .sha256 file's entries from a .sha1 file's apart,
// since the file extension alone isn't authoritative once lines are mixed).
std::optional<HashAlgorithm> HashAlgorithmFromHexLength(size_t hexLength);

using CheckSumCancelPoll = std::function<bool()>;
struct CheckSumProgress {
    uint64_t bytesSoFar = 0;
    uint64_t bytesTotal = 0;
};
using CheckSumProgressCallback = std::function<void(const CheckSumProgress&)>;

// Streams the file through CNG (BCryptHashData) in 1 MB chunks. Lower-case
// hex digest; nullopt on failure (can't open the file, a CNG call failed, or
// cancelPoll fired), with `err` set to why.
std::optional<std::wstring> HashFile(const std::filesystem::path& file, HashAlgorithm algorithm, std::wstring& err,
                                     const CheckSumCancelPoll& cancelPoll = {},
                                     const CheckSumProgressCallback& onProgress = {});

// The GNU/Total Commander text line for one file: "<hex> *<relativeName>"
// (the '*' marks binary mode, read by sha256sum -c and Total Commander
// alike). No trailing newline -- callers join lines themselves.
std::wstring FormatSumLine(const std::wstring& hashHex, const std::wstring& relativeName);

struct SumEntry {
    std::wstring hash;  // as written in the file, lower- or upper-case
    std::wstring name;  // relative path, '*' binary marker already stripped
};
// Accepts both "<hex> *name" and "<hex>  name" (GNU's text-mode two-space
// form), and both CRLF and LF line endings. Blank lines and lines starting
// with '#' (a comment, some tools' convention) are skipped.
std::vector<SumEntry> ParseSumFile(const std::wstring& text);

enum class VerifyStatus { Ok, Failed, Missing };
struct VerifyResult {
    std::wstring name;
    VerifyStatus status = VerifyStatus::Missing;
};
struct VerifyOutcome {
    std::vector<VerifyResult> results;
    size_t ok = 0;
    size_t failed = 0;
    size_t missing = 0;
    bool cancelled = false;
};

// Re-hashes every entry's file (resolved relative to `baseDir`) and compares,
// case-insensitively, against its recorded hash. The algorithm is inferred
// per entry from its hash's own hex length (HashAlgorithmFromHexLength), so a
// file mixing several algorithms' lines (unusual, but the format allows it)
// still verifies each one correctly.
VerifyOutcome VerifyPlan(const std::filesystem::path& baseDir, const std::vector<SumEntry>& entries,
                         const CheckSumCancelPoll& cancelPoll = {},
                         const CheckSumProgressCallback& onProgress = {});

// IS-0020: a directory in Generate Check Code's selection is walked
// recursively instead of being skipped. `relativePaths` are rooted at
// `root` itself ("sub\\file.txt", no leading separator) -- DoGenerateCheckCode
// prefixes each with the selected folder's own name before hashing, the same
// way a top-level file's bare name is used as-is. The walk never descends
// into a reparse point (SEC-006, matching DiskUsage.cpp's own walk); a
// subfolder that can't be listed is skipped and counted, not treated as a
// whole-operation error.
struct FileListResult {
    std::vector<std::wstring> relativePaths;
    uint64_t unreadableFolders = 0;
    bool cancelled = false;
};
FileListResult EnumerateFilesRecursively(const std::filesystem::path& root,
                                         const CheckSumCancelPoll& cancelPoll = {});

} // namespace mc
