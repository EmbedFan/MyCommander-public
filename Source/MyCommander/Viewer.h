#pragma once

#include <filesystem>

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

} // namespace mc
