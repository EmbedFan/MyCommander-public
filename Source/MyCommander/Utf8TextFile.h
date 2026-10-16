#pragma once

// Header-only UTF-8 text-file helpers shared by the small per-user stores
// that hold user-typed paths/names -- file-actions.conf (FEA-0009) and
// favorites.conf (NAV-007) -- so non-ASCII paths (C:\Dokumentumok\Árvíztűrő)
// round-trip and both stores follow exactly the same encoding rules.
// Extracted from FileActions.cpp unchanged.

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>

namespace mc {

// UTF-8 to UTF-16; a byte sequence that isn't valid UTF-8 (a file hand-
// edited in an ANSI editor) is decoded with the ANSI code page instead.
inline std::wstring Utf8ToWide(const std::string& bytes) {
    if (bytes.empty()) return L"";
    int len = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (len <= 0) {
        codePage = CP_ACP;
        flags = 0;
        len = MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
        if (len <= 0) return L"";
    }
    std::wstring out(static_cast<size_t>(len), L'\0');
    MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), out.data(), len);
    return out;
}

inline std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return "";
    int len = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>((std::max)(len, 0)), '\0');
    if (len > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), len, nullptr, nullptr);
    return out;
}

// The whole file as text (a UTF-8 BOM is stripped); empty if missing or unreadable.
inline std::wstring ReadUtf8TextFile(const std::filesystem::path& path) {
    if (path.empty()) return L"";
    std::ifstream in(path, std::ios::binary);
    if (!in) return L"";
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF &&
        static_cast<unsigned char>(bytes[1]) == 0xBB && static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.erase(0, 3);
    return Utf8ToWide(bytes);
}

// Writes `text` as UTF-8 (no BOM) via a temp file + MoveFileExW, the
// Session/CommandHistory atomic-write precedent, creating the parent folder
// if needed. `withBom` (LIST-012: a CSV that Excel must read as UTF-8) prefixes EF BB BF. False on any failure.
inline bool WriteUtf8TextFileAtomic(const std::filesystem::path& path, const std::wstring& text, bool withBom = false) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;
    const std::filesystem::path temporary = path.wstring() + L".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        std::string bytes = WideToUtf8(text);
        if (withBom) bytes.insert(0, "\xEF\xBB\xBF");
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    if (MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) return true;
    std::filesystem::remove(temporary, ec);
    return false;
}

}  // namespace mc
