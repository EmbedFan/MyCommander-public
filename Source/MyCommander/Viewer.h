#pragma once

#include <filesystem>
#include <string>

#include "Console.h"

namespace mc {

// A minimal full-screen, scrollable text viewer (VEE-003/VEE-004): vertical
// scrolling (arrows/PageUp/PageDown/Home/End), tab expansion, best-effort
// encoding detection, and a forward, wrapping, case-insensitive substring
// search ('/' to search, 'n' to repeat). Pages through the file via
// TextFile.h's IndexTextFileForViewing/ReadIndexedLine, so file size is
// never capped — only the lines actually scrolled into view or matched by a
// search are ever decoded. Blocks until the user presses Esc, F3, or F10.
void ShowTextFileViewer(Console& console, const std::filesystem::path& file);

// SRC-004: where the viewer opens -- `line` (1-based; 0 = the top) scrolled
// into view, and `searchTerm` pre-loaded so 'n' jumps to the next match
// (Find files' F3 on a content result).
struct ViewerStart {
    size_t line = 0;
    std::wstring searchTerm;
    // EXT-003: the header text instead of the file's path -- a plugin
    // viewer's output is a temporary file whose own name means nothing.
    std::wstring title;
};
void ShowTextFileViewer(Console& console, const std::filesystem::path& file, const ViewerStart& start);

} // namespace mc
