#pragma once

// SRC-008: the full-screen text difference viewer (File > Compare contents,
// Alt+F3, and Alt+F3 in the folder-compare results). Side by side -- left
// file | right file, aligned, with line numbers -- or unified (one column,
// automatic below 100 columns); a marker column ('<' removed, '>' added,
// '~' changed) carries the meaning without colour (UI-010). The diff itself
// is TextDiff.h; this file only draws and handles keys. Blocks until Esc,
// F10 or q.

#include "Console.h"
#include "TextDiff.h"

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mc {

struct DiffDocument {
    std::filesystem::path leftPath, rightPath;
    std::vector<std::wstring> left, right;  // the files' lines (decoded, tabs not expanded)
    std::wstring leftEncoding, rightEncoding;  // shown in the status line when they differ
};

// Recomputes the diff for new options (I / W keys), with the caller's own
// progress display and Esc handling; a Cancelled or TooLarge result keeps
// the current one on screen with a note.
using DiffRecompute = std::function<DiffResult(const DiffOptions&)>;

void ShowTextDiffViewer(Console& console, const DiffDocument& document, DiffOptions options, DiffResult result,
                        const DiffRecompute& recompute);

}  // namespace mc
