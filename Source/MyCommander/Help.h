#pragma once

#include "Console.h"

#include <string>
#include <vector>

namespace mc {

// FEA-0002 (MC Plugin), Phase 2, §7: one kMcPluginCapHelp plugin's own
// content, ready for ShowShortcutReference to render -- `name` is the
// plugin's own display name (rendered as a sub-heading), `markdown` its raw
// Markdown source (parsed via MarkdownRenderer.h). Owned here, not in
// PluginManager.h, since Help.cpp is the type's only consumer;
// PluginManager.h includes this header to produce it (PluginManager::
// CollectHelpContent), avoiding a heavier reverse dependency the other way.
struct PluginHelpContent {
    std::wstring name;
    std::wstring version;  // IS-0011: McPluginMetadata::version, shown next to `name`
    std::wstring markdown;
};

// KEY-004: a full-screen, scrollable in-application reference for every
// keyboard shortcut MyCommander recognizes (KEY-001/002/003), grouped into
// categories. Read-only — Up/Down/Page Up/Page Down/Home/End scroll, same
// shape as the built-in text viewer (VEE-003); the mouse wheel also scrolls
// it, 3 rows per notch, matching the dual-pane panels' own wheel granularity
// (IS-0008). Blocks until the user presses Esc, F1, or F10.
//
// `pluginContent` (FEA-0002 Phase 2, §7): each entry becomes a sub-heading
// in a new "Plugins" section appended after the built-in categories, its
// Markdown rendered through MarkdownRenderer.h's deliberately minimal
// parser. Empty by default (the ordinary case, no plugins loaded or none
// declaring kMcPluginCapHelp) -- `main.cpp` passes `pluginManager.
// CollectHelpContent()` at the one call site that has a PluginManager in
// scope.
//
// `keyMap` (KEY-005): when given, a row whose keys belong to a command the
// user rebound in keys.conf shows the command's current keys instead.
class KeyMap;
void ShowShortcutReference(Console& console, const std::vector<PluginHelpContent>& pluginContent = {},
                           const KeyMap* keyMap = nullptr);

} // namespace mc
