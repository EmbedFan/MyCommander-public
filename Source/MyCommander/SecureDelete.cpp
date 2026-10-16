#include "SecureDelete.h"

#include "PathUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

#include <algorithm>
#include <vector>

#pragma comment(lib, "bcrypt.lib")

namespace fs = std::filesystem;

namespace mc {

namespace {

constexpr DWORD kChunk = 64 * 1024;

std::wstring RandomName() {
    unsigned char bytes[16] = {};
    BCryptGenRandom(nullptr, bytes, sizeof(bytes), BCRYPT_USE_SYSTEM_PREFERRED_RNG);
    static const wchar_t kAlphabet[] = L"abcdefghijklmnopqrstuvwxyz0123456789";
    std::wstring name;
    for (unsigned char b : bytes) name.push_back(kAlphabet[b % 36]);
    return name;
}

// One pass over the whole file. `random` false = zeros. Returns false on a
// write error (lastError set) or cancel (cancelled set).
bool Pass(HANDLE h, uint64_t size, bool random, const SecureHooks& hooks, bool& cancelled, DWORD& lastError) {
    LARGE_INTEGER zero{};
    if (!SetFilePointerEx(h, zero, nullptr, FILE_BEGIN)) {
        lastError = GetLastError();
        return false;
    }
    std::vector<unsigned char> buf(kChunk, 0);
    uint64_t done = 0;
    while (done < size) {
        if (hooks.cancel && hooks.cancel()) {
            cancelled = true;
            return false;
        }
        DWORD n = static_cast<DWORD>((std::min<uint64_t>)(kChunk, size - done));
        if (random && !BCRYPT_SUCCESS(BCryptGenRandom(nullptr, buf.data(), n, BCRYPT_USE_SYSTEM_PREFERRED_RNG))) {
            lastError = ERROR_GEN_FAILURE;
            return false;
        }
        DWORD written = 0;
        if (!WriteFile(h, buf.data(), n, &written, nullptr) || written != n) {
            lastError = GetLastError();
            return false;
        }
        done += n;
    }
    if (!FlushFileBuffers(h)) {
        lastError = GetLastError();
        return false;
    }
    return true;
}

}  // namespace

SecureCheck CheckSecureDelete(uint32_t attributes) {
    SecureCheck c;
    if (attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        c.verdict = SecureVerdict::Refused;
        c.reason = L"a link: only the link itself would be removed";
    } else if (attributes & FILE_ATTRIBUTE_COMPRESSED) {
        c.verdict = SecureVerdict::Refused;
        c.reason = L"compressed: an in-place overwrite does not reach all stored data";
    } else if (attributes & FILE_ATTRIBUTE_SPARSE_FILE) {
        c.verdict = SecureVerdict::Refused;
        c.reason = L"sparse: an in-place overwrite does not reach all stored data";
    } else if (attributes & FILE_ATTRIBUTE_ENCRYPTED) {
        c.verdict = SecureVerdict::Refused;
        c.reason = L"encrypted (EFS): overwriting does not reliably erase the encrypted blocks";
    } else if (attributes & FILE_ATTRIBUTE_READONLY) {
        c.verdict = SecureVerdict::ReadOnlyNeedsClearing;
    }
    return c;
}

SecureFileResult SecureDeleteFile(const fs::path& file, const SecureHooks& hooks) {
    SecureFileResult r;
    DWORD attrs = GetAttributesOrInvalid(file);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        r.message = FormatWinError(GetLastError());
        return r;
    }
    if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
        r.message = L"not a file";
        return r;
    }
    SecureCheck check = CheckSecureDelete(attrs);
    if (check.verdict == SecureVerdict::Refused) {
        r.status = SecureStatus::Refused;
        r.message = check.reason;
        return r;
    }
    const bool wasReadOnly = check.verdict == SecureVerdict::ReadOnlyNeedsClearing;
    if (wasReadOnly && !SetFileAttributesW(ToLongPath(file).c_str(), attrs & ~FILE_ATTRIBUTE_READONLY)) {
        r.message = FormatWinError(GetLastError());
        return r;
    }

    // The random rename first, so the original name does not survive in the
    // directory entry; undone if the overwrite or the delete does not finish.
    fs::path renamed = file.parent_path() / RandomName();
    if (!MoveFileExW(ToLongPath(file).c_str(), ToLongPath(renamed).c_str(), 0)) {
        r.message = FormatWinError(GetLastError());
        if (wasReadOnly) SetFileAttributesW(ToLongPath(file).c_str(), attrs);
        return r;
    }
    auto putBack = [&]() {
        MoveFileExW(ToLongPath(renamed).c_str(), ToLongPath(file).c_str(), 0);
        if (wasReadOnly) SetFileAttributesW(ToLongPath(file).c_str(), attrs);
    };

    HANDLE h = CreateFileW(ToLongPath(renamed).c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_FLAG_WRITE_THROUGH, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        r.message = FormatWinError(GetLastError());
        putBack();
        return r;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    bool cancelled = false;
    DWORD err = 0;
    bool ok = true;
    if (size.QuadPart > 0) {
        for (int pass = 1; pass <= 2 && ok; ++pass) {
            ok = Pass(h, static_cast<uint64_t>(size.QuadPart), pass == 2, hooks, cancelled, err);
            if (ok && hooks.afterPass) hooks.afterPass(pass, renamed);
        }
    }
    CloseHandle(h);
    if (!ok) {
        putBack();
        if (cancelled) {
            r.status = SecureStatus::Cancelled;
            r.message = L"cancelled: partially overwritten and not deleted";
        } else {
            r.message = L"overwrite failed (" + FormatWinError(err) + L"): partially overwritten and not deleted";
        }
        return r;
    }
    if (!DeleteFileW(ToLongPath(renamed).c_str())) {
        DWORD e = GetLastError();
        putBack();
        r.message = L"overwritten but could not be deleted: " + FormatWinError(e);
        return r;
    }
    r.status = SecureStatus::Erased;
    return r;
}

namespace {

void Walk(const fs::path& path, const SecureHooks& hooks, const std::function<void(const fs::path&)>& onFile,
          SecureTreeOutcome& out) {
    if (out.cancelled) return;
    DWORD attrs = GetAttributesOrInvalid(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        out.problems.push_back({path.wstring(), FormatWinError(GetLastError())});
        return;
    }
    const bool isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (attrs & FILE_ATTRIBUTE_REPARSE_POINT) {  // the link itself, never its target (SEC-006)
        SetFileAttributesW(ToLongPath(path).c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
        BOOL ok = isDir ? RemoveDirectoryW(ToLongPath(path).c_str()) : DeleteFileW(ToLongPath(path).c_str());
        if (ok)
            ++out.links;
        else
            out.problems.push_back({path.wstring(), FormatWinError(GetLastError())});
        return;
    }
    if (!isDir) {
        if (onFile) onFile(path);
        SecureFileResult r = SecureDeleteFile(path, hooks);
        switch (r.status) {
            case SecureStatus::Erased: ++out.erased; break;
            case SecureStatus::Refused:
                ++out.refused;
                out.problems.push_back({path.wstring(), r.message});
                break;
            case SecureStatus::Cancelled:
                out.cancelled = true;
                out.problems.push_back({path.wstring(), r.message});
                break;
            case SecureStatus::Failed: out.problems.push_back({path.wstring(), r.message}); break;
        }
        return;
    }
    std::vector<fs::path> children;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((ToLongPath(path) / L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                0);
    if (h == INVALID_HANDLE_VALUE) {
        out.problems.push_back({path.wstring(), FormatWinError(GetLastError())});
        return;
    }
    do {
        std::wstring n = fd.cFileName;
        if (n != L"." && n != L"..") children.push_back(path / n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    for (const fs::path& c : children) {
        Walk(c, hooks, onFile, out);
        if (out.cancelled) return;
    }
    // Only an empty folder is removed; one that kept a refused/failed file stays.
    if (!RemoveDirectoryW(ToLongPath(path).c_str()) && (attrs & FILE_ATTRIBUTE_READONLY)) {
        SetFileAttributesW(ToLongPath(path).c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
        if (!RemoveDirectoryW(ToLongPath(path).c_str())) SetFileAttributesW(ToLongPath(path).c_str(), attrs);
    }
}

}  // namespace

SecureTreeOutcome SecureDeleteTree(const fs::path& root, const SecureHooks& hooks,
                                   const std::function<void(const fs::path&)>& onFile) {
    SecureTreeOutcome out;
    Walk(root, hooks, onFile, out);
    return out;
}

}  // namespace mc
