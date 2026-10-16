#pragma once

// FOP-025: paste files copied or cut in Windows Explorer. The pure parts: the
// parser for the clipboard's CF_HDROP (DROPFILES) data and the plan that
// groups the pasted items by source folder and drops the ones that cannot be
// pasted. No clipboard or Console dependency; the one place that reads the
// real clipboard is Clipboard.cpp's ReadClipboardFileList.

#include <cstddef>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace mc {

// Parses a DROPFILES block (header: DWORD pFiles; POINT pt; BOOL fNC; BOOL
// fWide, then the double-NUL-terminated list). Returns the paths; an
// ANSI list is decoded with the ANSI code page. Malformed or empty data gives
// an empty list.
std::vector<std::filesystem::path> ParseDropFiles(const void* data, size_t size);

struct PasteGroup {
    std::filesystem::path sourceDir;
    std::vector<std::wstring> names;
};

struct PastePlan {
    bool isMove = false;
    std::vector<PasteGroup> groups;                                  // in clipboard order
    std::vector<std::pair<std::filesystem::path, std::wstring>> dropped;  // (source, reason)
    size_t TotalItems() const {
        size_t n = 0;
        for (const auto& g : groups) n += g.names.size();
        return n;
    }
};

// Groups `sources` by their parent folder (first-seen order, names in order).
// A source that is already in `destination`, or is `destination` itself or
// one of its ancestors (a folder pasted into itself, FOP-010), is dropped
// with a reason. A cut is a move, otherwise a copy.
PastePlan PlanPaste(const std::vector<std::filesystem::path>& sources, const std::filesystem::path& destination,
                    bool isCut);

}  // namespace mc
