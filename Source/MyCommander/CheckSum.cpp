#include "CheckSum.h"

#include "PathUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <cwctype>

#pragma comment(lib, "bcrypt.lib")

namespace mc {

namespace {

constexpr uint32_t kChunkBytes = 1 * 1024 * 1024;

LPCWSTR AlgorithmId(HashAlgorithm algorithm) {
    switch (algorithm) {
        case HashAlgorithm::MD5: return BCRYPT_MD5_ALGORITHM;
        case HashAlgorithm::SHA1: return BCRYPT_SHA1_ALGORITHM;
        case HashAlgorithm::SHA256: return BCRYPT_SHA256_ALGORITHM;
        case HashAlgorithm::SHA512: return BCRYPT_SHA512_ALGORITHM;
    }
    return BCRYPT_SHA256_ALGORITHM;
}

size_t DigestLength(HashAlgorithm algorithm) {
    switch (algorithm) {
        case HashAlgorithm::MD5: return 16;
        case HashAlgorithm::SHA1: return 20;
        case HashAlgorithm::SHA256: return 32;
        case HashAlgorithm::SHA512: return 64;
    }
    return 32;
}

std::wstring ToHex(const std::vector<BYTE>& bytes) {
    static const wchar_t kDigits[] = L"0123456789abcdef";
    std::wstring out;
    out.reserve(bytes.size() * 2);
    for (BYTE b : bytes) {
        out += kDigits[b >> 4];
        out += kDigits[b & 0x0F];
    }
    return out;
}

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

}  // namespace

std::wstring HashAlgorithmName(HashAlgorithm algorithm) {
    switch (algorithm) {
        case HashAlgorithm::MD5: return L"md5";
        case HashAlgorithm::SHA1: return L"sha1";
        case HashAlgorithm::SHA256: return L"sha256";
        case HashAlgorithm::SHA512: return L"sha512";
    }
    return L"sha256";
}

std::optional<HashAlgorithm> HashAlgorithmFromHexLength(size_t hexLength) {
    switch (hexLength) {
        case 32: return HashAlgorithm::MD5;
        case 40: return HashAlgorithm::SHA1;
        case 64: return HashAlgorithm::SHA256;
        case 128: return HashAlgorithm::SHA512;
        default: return std::nullopt;
    }
}

std::optional<std::wstring> HashFile(const std::filesystem::path& file, HashAlgorithm algorithm, std::wstring& err,
                                     const CheckSumCancelPoll& cancelPoll,
                                     const CheckSumProgressCallback& onProgress) {
    HANDLE fileHandle = CreateFileW(ToLongPath(file).c_str(), GENERIC_READ,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                                    FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (fileHandle == INVALID_HANDLE_VALUE) {
        err = FormatWinError(GetLastError());
        return std::nullopt;
    }

    LARGE_INTEGER fileSize{};
    GetFileSizeEx(fileHandle, &fileSize);
    uint64_t totalBytes = static_cast<uint64_t>(fileSize.QuadPart);

    BCRYPT_ALG_HANDLE algHandle = nullptr;
    std::optional<std::wstring> result;
    if (BCRYPT_SUCCESS(BCryptOpenAlgorithmProvider(&algHandle, AlgorithmId(algorithm), nullptr, 0))) {
        BCRYPT_HASH_HANDLE hashHandle = nullptr;
        if (BCRYPT_SUCCESS(BCryptCreateHash(algHandle, &hashHandle, nullptr, 0, nullptr, 0, 0))) {
            std::vector<BYTE> buffer(kChunkBytes);
            uint64_t readSoFar = 0;
            bool ok = true;
            bool cancelled = false;
            for (;;) {
                DWORD got = 0;
                if (!ReadFile(fileHandle, buffer.data(), static_cast<DWORD>(buffer.size()), &got, nullptr)) {
                    err = FormatWinError(GetLastError());
                    ok = false;
                    break;
                }
                if (got == 0) break;  // end of file
                if (!BCRYPT_SUCCESS(BCryptHashData(hashHandle, buffer.data(), got, 0))) {
                    err = L"CNG hashing failed.";
                    ok = false;
                    break;
                }
                readSoFar += got;
                if (onProgress) onProgress(CheckSumProgress{readSoFar, totalBytes});
                if (cancelPoll && cancelPoll()) {
                    cancelled = true;
                    break;
                }
            }
            if (ok && !cancelled) {
                std::vector<BYTE> digest(DigestLength(algorithm));
                if (BCRYPT_SUCCESS(
                        BCryptFinishHash(hashHandle, digest.data(), static_cast<ULONG>(digest.size()), 0))) {
                    result = ToHex(digest);
                } else {
                    err = L"CNG hashing failed.";
                }
            } else if (cancelled) {
                err = L"Cancelled.";
            }
            BCryptDestroyHash(hashHandle);
        } else {
            err = L"CNG hashing failed.";
        }
        BCryptCloseAlgorithmProvider(algHandle, 0);
    } else {
        err = L"CNG algorithm provider unavailable.";
    }
    CloseHandle(fileHandle);
    return result;
}

std::wstring FormatSumLine(const std::wstring& hashHex, const std::wstring& relativeName) {
    return hashHex + L" *" + relativeName;
}

std::vector<SumEntry> ParseSumFile(const std::wstring& text) {
    std::vector<SumEntry> entries;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring::npos) end = text.size();
        std::wstring line = text.substr(pos, end - pos);
        pos = end + 1;
        if (!line.empty() && line.back() == L'\r') line.pop_back();
        std::wstring trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == L'#') {
            if (end == text.size()) break;
            continue;
        }
        size_t sep = trimmed.find_first_of(L" \t");
        if (sep == std::wstring::npos) {
            if (end == text.size()) break;
            continue;  // not a recognizable "<hash> <name>" line; skipped
        }
        std::wstring hash = trimmed.substr(0, sep);
        std::wstring rest = Trim(trimmed.substr(sep + 1));
        if (!rest.empty() && rest.front() == L'*') rest.erase(rest.begin());  // GNU binary-mode marker
        entries.push_back({hash, rest});
        if (end == text.size()) break;
    }
    return entries;
}

VerifyOutcome VerifyPlan(const std::filesystem::path& baseDir, const std::vector<SumEntry>& entries,
                         const CheckSumCancelPoll& cancelPoll, const CheckSumProgressCallback& onProgress) {
    VerifyOutcome outcome;
    for (const SumEntry& entry : entries) {
        if (cancelPoll && cancelPoll()) {
            outcome.cancelled = true;
            break;
        }
        std::filesystem::path path = baseDir / entry.name;
        std::error_code ec;
        if (!std::filesystem::exists(path, ec) || ec) {
            outcome.results.push_back({entry.name, VerifyStatus::Missing});
            ++outcome.missing;
            continue;
        }
        auto algorithm = HashAlgorithmFromHexLength(entry.hash.size());
        if (!algorithm) {
            outcome.results.push_back({entry.name, VerifyStatus::Failed});
            ++outcome.failed;
            continue;
        }
        std::wstring err;
        auto actual = HashFile(path, *algorithm, err, cancelPoll, onProgress);
        if (!actual) {
            outcome.results.push_back({entry.name, VerifyStatus::Failed});
            ++outcome.failed;
            continue;
        }
        std::wstring expectedLower = entry.hash, actualLower = *actual;
        std::transform(expectedLower.begin(), expectedLower.end(), expectedLower.begin(),
                       [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        bool ok = expectedLower == actualLower;
        outcome.results.push_back({entry.name, ok ? VerifyStatus::Ok : VerifyStatus::Failed});
        if (ok) ++outcome.ok; else ++outcome.failed;
    }
    return outcome;
}

namespace {

// Same FindFirstFileExW walk shape as DiskUsage.cpp's WalkDirectory -- SEC-006
// (never descends into a reparse point), an unreadable subfolder counted and
// skipped rather than aborting the whole walk.
void WalkForFiles(const std::filesystem::path& dir, const std::wstring& relativePrefix,
                  const CheckSumCancelPoll& cancelPoll, FileListResult& result) {
    if (cancelPoll && cancelPoll()) {
        result.cancelled = true;
        return;
    }
    std::filesystem::path pattern = ToLongPath(dir) / L"*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) ++result.unreadableFolders;  // an empty dir is not an error
        return;
    }
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring relative = relativePrefix.empty() ? name : relativePrefix + L"\\" + name;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool isReparsePoint = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (isDir) {
            if (!isReparsePoint) {  // SEC-006: never descend into a reparse point
                WalkForFiles(dir / name, relative, cancelPoll, result);
                if (result.cancelled) {
                    FindClose(h);
                    return;
                }
            }
        } else {
            result.relativePaths.push_back(relative);
        }
        if (cancelPoll && cancelPoll()) {
            result.cancelled = true;
            FindClose(h);
            return;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

}  // namespace

FileListResult EnumerateFilesRecursively(const std::filesystem::path& root, const CheckSumCancelPoll& cancelPoll) {
    FileListResult result;
    WalkForFiles(root, L"", cancelPoll, result);
    return result;
}

} // namespace mc
