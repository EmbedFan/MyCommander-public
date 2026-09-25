#pragma once

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <filesystem>
#include <optional>
#include <string>
#include <system_error>

namespace mc {

// Rewrites an absolute path with the \\?\ prefix so Win32 calls bypass
// MAX_PATH and work for long paths and UNC shares. Relative paths are
// returned unchanged; callers are expected to resolve to absolute form
// first (see main.cpp's ResolveStartPath).
inline std::filesystem::path ToLongPath(const std::filesystem::path& p) {
    const std::wstring& s = p.native();
    if (s.rfind(L"\\\\?\\", 0) == 0) {
        return p; // already prefixed
    }
    if (s.rfind(L"\\\\", 0) == 0) {
        return std::filesystem::path(L"\\\\?\\UNC\\" + s.substr(2)); // \\server\share -> \\?\UNC\server\share
    }
    if (s.size() >= 2 && s[1] == L':') {
        return std::filesystem::path(L"\\\\?\\" + s); // C:\... -> \\?\C:\...
    }
    return p;
}

// Returns the raw Win32 file attributes for `p`, or INVALID_FILE_ATTRIBUTES
// if it doesn't exist or can't be queried. Shared by Panel (attribute
// display, CON-005) and FileOps (attribute toggling).
inline DWORD GetAttributesOrInvalid(const std::filesystem::path& p) {
    return GetFileAttributesW(ToLongPath(p).c_str());
}

// Returns the raw Win32 last-write FILETIME for `p` as a single 64-bit
// value (100ns ticks since 1601-01-01 UTC), or 0 if it doesn't exist or
// can't be queried. Used for Panel's modification-time column (UI-004).
inline uint64_t GetLastWriteTimeOrZero(const std::filesystem::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(ToLongPath(p).c_str(), GetFileExInfoStandard, &data)) return 0;
    ULARGE_INTEGER t;
    t.LowPart = data.ftLastWriteTime.dwLowDateTime;
    t.HighPart = data.ftLastWriteTime.dwHighDateTime;
    return t.QuadPart;
}

// True if `p` is a symlink, junction, or other reparse point. Recursive
// copy/move/delete must never traverse into one of these (SEC-006).
inline bool IsReparsePoint(const std::filesystem::path& p) {
    DWORD attrs = GetAttributesOrInvalid(p);
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
}

// True if `p` is the topmost navigable level for its root: a drive root
// (C:\), or a UNC share root (\\server\share) — the level at which "go to
// parent directory" should stop (CON-005's "UNC paths ... shall be
// supported"). std::filesystem's own root_name() for a UNC path covers only
// the server ("\\server"), not the share, so a plain `parent_path() != p`
// check alone stops one level too late — at a bare "\\server\" that isn't a
// real, listable directory — rather than at the share itself.
inline bool IsFilesystemRoot(const std::filesystem::path& p) {
    if (p.parent_path() == p) return true;  // drive root, or an already-topmost UNC host path

    std::filesystem::path rootNamePath = p.root_name();  // keep it alive by value: root_name() returns
                                                          // a temporary, and binding a reference straight
                                                          // to its .native() would dangle past this line
    const std::wstring& rootName = rootNamePath.native();
    bool isUnc = rootName.size() > 2 && rootName[0] == L'\\' && rootName[1] == L'\\';
    if (!isUnc) return false;

    std::filesystem::path shareRoot = p.root_path();  // "\\server\" for a UNC path
    auto rel = p.relative_path();
    auto it = rel.begin();
    if (it != rel.end()) shareRoot /= *it;  // "\\server\share"
    return p == shareRoot;
}

// True if `candidate` is the same path as `base`, or nested inside it.
// Used to refuse copying/moving a directory into its own subtree (SEC-005).
inline bool IsSameOrWithin(const std::filesystem::path& base, const std::filesystem::path& candidate) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path baseAbs = fs::weakly_canonical(base, ec);
    if (ec) baseAbs = base;
    ec.clear();
    fs::path candAbs = fs::weakly_canonical(candidate, ec);
    if (ec) candAbs = candidate;

    auto toLower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(),
                        [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return s;
    };
    std::wstring baseStr = toLower(baseAbs.native());
    std::wstring candStr = toLower(candAbs.native());
    if (!baseStr.empty() && baseStr.back() == L'\\') baseStr.pop_back();
    if (!candStr.empty() && candStr.back() == L'\\') candStr.pop_back();

    if (candStr == baseStr) return true;
    return candStr.size() > baseStr.size() &&
           candStr.compare(0, baseStr.size(), baseStr) == 0 &&
           candStr[baseStr.size()] == L'\\';
}

// NAV-003: resolves what a user typed or pasted into the command line to an
// existing directory, or nullopt if it isn't one. Trims surrounding
// whitespace and a matching pair of quotes (the shape a path takes when
// pasted from Explorer's address bar or a shortcut's Properties dialog),
// resolves a relative path against `basePath`, and normalizes any "."/".."
// segments via weakly_canonical — required before ToLongPath's `\\?\`
// prefix, which (unlike a plain Win32 path) cannot resolve those itself.
// Lets the command line tell "go to this path" apart from "run this as a
// shell command" for the same typed line.
inline std::optional<std::filesystem::path> ResolveExistingDirectory(const std::filesystem::path& basePath,
                                                                      const std::wstring& typed) {
    size_t start = typed.find_first_not_of(L" \t");
    if (start == std::wstring::npos) return std::nullopt;
    size_t end = typed.find_last_not_of(L" \t");
    std::wstring trimmed = typed.substr(start, end - start + 1);
    if (trimmed.size() >= 2 && trimmed.front() == L'"' && trimmed.back() == L'"') {
        trimmed = trimmed.substr(1, trimmed.size() - 2);
    }
    if (trimmed.empty()) return std::nullopt;

    std::filesystem::path candidate(trimmed);
    if (!candidate.is_absolute()) candidate = basePath / candidate;

    std::error_code ec;
    std::filesystem::path resolved = std::filesystem::weakly_canonical(candidate, ec);
    if (ec) resolved = candidate;

    if (!std::filesystem::is_directory(ToLongPath(resolved), ec) || ec) return std::nullopt;
    return resolved;
}

namespace detail {

// Classic recursive glob matcher: '*' consumes any run of characters
// (including none), '?' consumes exactly one, everything else compares
// case-insensitively. Shared implementation behind WildcardMatch below.
inline bool GlobMatchCaseInsensitive(const wchar_t* n, const wchar_t* p) {
    if (*p == L'\0') return *n == L'\0';
    if (*p == L'*') {
        for (const wchar_t* s = n;; ++s) {
            if (GlobMatchCaseInsensitive(s, p + 1)) return true;
            if (*s == L'\0') return false;
        }
    }
    if (*n == L'\0') return false;
    if (*p == L'?' || std::towlower(*n) == std::towlower(*p)) return GlobMatchCaseInsensitive(n + 1, p + 1);
    return false;
}

}  // namespace detail

// True if `name` matches the case-insensitive `pattern`. A pattern
// containing '*' or '?' is matched as a classic anchored glob; a pattern
// with neither is treated as a substring match (e.g. "log" matches
// "app.log.txt") — the behavior both Panel's quick filter (SRC-003) and the
// recursive file search (SRC-001) expect from a plain name typed by hand.
// An empty pattern matches everything.
inline bool WildcardMatch(const std::wstring& name, const std::wstring& pattern) {
    if (pattern.empty()) return true;
    if (pattern.find_first_of(L"*?") == std::wstring::npos) {
        auto toLower = [](std::wstring s) {
            std::transform(s.begin(), s.end(), s.begin(),
                            [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
            return s;
        };
        return toLower(name).find(toLower(pattern)) != std::wstring::npos;
    }
    return detail::GlobMatchCaseInsensitive(name.c_str(), pattern.c_str());
}

// Formats a Win32 error code the way GetLastError() returns it, trimmed of
// trailing newlines. Falls back to a generic message if FormatMessage fails.
inline std::wstring FormatWinError(DWORD err) {
    LPWSTR buf = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                    FORMAT_MESSAGE_IGNORE_INSERTS,
                                nullptr, err, 0, reinterpret_cast<LPWSTR>(&buf), 0, nullptr);
    std::wstring msg = (len && buf) ? std::wstring(buf, len) : L"unknown error";
    if (buf) LocalFree(buf);
    while (!msg.empty() && (msg.back() == L'\r' || msg.back() == L'\n')) msg.pop_back();
    return msg;
}

// Widens a std::error_code's narrow message() to a std::wstring. Deliberately
// stores message() in a local before taking iterators from it: message()
// returns by value, so writing std::wstring(ec.message().begin(),
// ec.message().end()) calls it twice, and each call constructs a distinct
// temporary std::string — the range constructor then receives a begin and
// an end from two different objects, which is undefined behavior that the
// debug STL's iterator checking (_ITERATOR_DEBUG_LEVEL, on by default in a
// Debug build) reliably catches as "string iterators in range are from
// different containers", surfacing as a blocking Debug Assertion dialog.
inline std::wstring ToWideMessage(const std::error_code& ec) {
    std::string msg = ec.message();
    return std::wstring(msg.begin(), msg.end());
}

} // namespace mc
