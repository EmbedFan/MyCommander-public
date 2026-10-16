#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace mc {

// SEL-008: Copy Selected Names to Clipboard. The three ways to format a list
// of names, and the pure text builder (CON-004: the only Win32 clipboard code
// is CopyTextToClipboard below, so FormatNamesForClipboard is independently
// testable without a real clipboard).
enum class ClipboardNameFormat { Names, FullPaths, FullPathsWithDetails };

struct ClipboardFileDetail {
    std::wstring name;
    std::filesystem::path fullPath;
    uint64_t size = 0;
    std::wstring modifiedText;  // already formatted ("YYYY-MM-DD HH:MM"), so this stays Console-free
    bool isDirectory = false;
};

// One line per entry, CRLF-joined (a trailing CRLF, matching what Explorer's
// own "Copy as path" puts on the clipboard). FullPathsWithDetails is
// tab-separated: "<path>\t<size or \"<DIR>\">\t<modifiedText>".
std::wstring FormatNamesForClipboard(const std::vector<ClipboardFileDetail>& items, ClipboardNameFormat format);

// All Win32 clipboard access lives in Clipboard.cpp (CON-004). False on any
// failure (another process holding the clipboard open, etc.) -- never
// throws, never retries.
bool CopyTextToClipboard(const std::wstring& text);

// FOP-025: the files Explorer (or any program) put on the clipboard as a
// file list (CF_HDROP). `isCut` is true when the "Preferred DropEffect"
// format says move (Explorer's Cut). False, with `out` empty, when the
// clipboard holds no file list or cannot be opened. Read only on request.
bool ReadClipboardFileList(std::vector<std::filesystem::path>& out, bool& isCut);

} // namespace mc
