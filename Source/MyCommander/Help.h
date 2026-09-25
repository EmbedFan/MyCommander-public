#pragma once

#include "Console.h"

namespace mc {

// KEY-004: a full-screen, scrollable in-application reference for every
// keyboard shortcut MyCommander recognizes (KEY-001/002/003), grouped into
// categories. Read-only — Up/Down/Page Up/Page Down/Home/End scroll, same
// shape as the built-in text viewer (VEE-003); the mouse wheel also scrolls
// it, 3 rows per notch, matching the dual-pane panels' own wheel granularity
// (IS-0008). Blocks until the user presses Esc, F1, or F10.
void ShowShortcutReference(Console& console);

} // namespace mc
