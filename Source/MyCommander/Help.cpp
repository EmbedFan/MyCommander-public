#include "Help.h"

#include "KeyMap.h"
#include "MarkdownRenderer.h"
#include "Strings.h"
#include "TextWidth.h"
#include "Theme.h"
#include "Version.h"

#include <windows.h>

#include <algorithm>
#include <iterator>
#include <vector>

#pragma comment(lib, "version.lib")

namespace mc {

namespace {

// The running exe's own embedded VERSIONINFO resource (MyCommander.rc's
// StringFileInfo block) — the same data Explorer's Properties > Details tab
// and `--version` draw from, read once at runtime rather than duplicating
// those strings a second time by hand in this file.
struct FileVersionMetadata {
    std::wstring productName;
    std::wstring fileVersion;
    std::wstring fileDescription;
    std::wstring companyName;
    std::wstring legalCopyright;
};

FileVersionMetadata ReadFileVersionMetadata() {
    FileVersionMetadata info;

    wchar_t exePath[MAX_PATH];
    DWORD pathLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
    if (pathLen == 0 || pathLen >= MAX_PATH) return info;

    DWORD ignoredHandle = 0;
    DWORD size = GetFileVersionInfoSizeW(exePath, &ignoredHandle);
    if (size == 0) return info;

    std::vector<BYTE> buffer(size);
    if (!GetFileVersionInfoW(exePath, ignoredHandle, size, buffer.data())) return info;

    // "040904B0" matches MyCommander.rc's StringFileInfo language/codepage
    // block (US English, Unicode) exactly — the two must stay in sync.
    auto queryString = [&](const wchar_t* field) -> std::wstring {
        wchar_t subBlock[64];
        swprintf_s(subBlock, L"\\StringFileInfo\\040904B0\\%s", field);
        LPVOID value = nullptr;
        UINT valueLen = 0;
        if (VerQueryValueW(buffer.data(), subBlock, &value, &valueLen) && value && valueLen > 0) {
            return std::wstring(static_cast<const wchar_t*>(value));
        }
        return L"";
    };

    info.productName = queryString(L"ProductName");
    info.fileVersion = queryString(L"FileVersion");
    info.fileDescription = queryString(L"FileDescription");
    info.companyName = queryString(L"CompanyName");
    info.legalCopyright = queryString(L"LegalCopyright");

    // Defensive fallback only — Version.h's compile-time macro, in case the
    // running exe's own resource couldn't be read (e.g. a stripped binary).
    if (info.fileVersion.empty()) {
        wchar_t verBuf[64];
        swprintf_s(verBuf, L"%S", MC_VERSION_STR);
        info.fileVersion = verBuf;
    }
    return info;
}

// Read once; the returned reference's backing strings live for the rest of
// the process (a function-local static), so raw c_str() pointers taken from
// it and stored into ShortcutItem below stay valid for the program's life.
const FileVersionMetadata& Metadata() {
    static const FileVersionMetadata info = ReadFileVersionMetadata();
    return info;
}

// UI-011: display-width-aware, so this still aligns correctly if a
// description ever needs a non-ASCII character.
std::wstring PadOrTrim(const std::wstring& text, size_t width) {
    return PadToDisplayWidth(text, width);
}

struct ShortcutItem {
    const wchar_t* key;
    const wchar_t* description;
};

struct ShortcutSection {
    const wchar_t* title;
    std::vector<ShortcutItem> items;
};

// KEY-002/KEY-003: this mirrors README.md's keybindings table exactly, so
// the two never contradict each other — update both together. The "About"
// section is the one exception — it's runtime file-version metadata, not a
// shortcut, and has no README counterpart to stay in sync with.
const std::vector<ShortcutSection>& Sections() {
    static const std::vector<ShortcutSection> sections = [] {
        const FileVersionMetadata& info = Metadata();
        std::vector<ShortcutSection> result = {
            {L"About",
             {
                 {L"Product", info.productName.c_str()},
                 {L"Version", info.fileVersion.c_str()},
                 {L"Description", info.fileDescription.c_str()},
                 {L"Company", info.companyName.c_str()},
                 {L"Copyright", info.legalCopyright.c_str()},
             }},
            {L"Menu bar",
             {
                 {L"Alt+F", L"Open the File menu"},
                 {L"Alt+S", L"Open the Selection menu"},
                 {L"Alt+C", L"Open the Commands menu"},
                 {L"Alt+N", L"Open the Network menu"},
                 {L"Alt+V", L"Open the View menu"},
                 {L"Alt+E", L"Open the Settings menu"},
                 {L"Left / Right", L"Switch to the adjacent menu, while one is open"},
                 {L"Up / Down", L"Move within the open menu"},
                 {L"Enter", L"Choose the highlighted item"},
                 {L"Esc", L"Close the menu without choosing anything"},
             }},
            {L"Navigation",
             {
                 {L"Tab", L"Switch active panel"},
                 {L"Up / Down", L"Move cursor"},
                 {L"Page Up / Page Down", L"Move a page at a time"},
                 {L"Home / End", L"Jump to first / last entry, or move the command-line cursor if it has text"},
                 {L"Alt+Left / Alt+Right", L"Go back / forward through the active panel's location history"},
                 {L"Enter", L"Open a directory, run the command line, or run the default file action"},
                 {L"Backspace", L"Parent directory, or erase before the command-line cursor if it has text"},
                 {L"Alt+F1 / Alt+F2", L"Choose a drive for the left / right panel"},
                 {L"Ctrl+D", L"Directory hotlist: go to a favorite folder (Enter), add the current one, remove (Del), rename (F2)"},
                 {L"Ctrl+L / Ctrl+Shift+L", L"Colour label: cycle the selection's label (none, 1-6) / open the label chooser (set, clear, select entries with the same label, remove labels of missing files)"},
                 {L"Ctrl+F12", L"Largest files below the current folder: the N biggest files (form: count, minimum size, mask, subfolders, hidden), largest first; Enter goes to one, F3 views it"},
                 {L"Alt+Shift+F7", L"Find empty folders below the current one; go to one (Enter), delete one (Del) or all (A) to the Recycle Bin after confirmation"},
                 {L"Alt+F12", L"Recently visited folders (either panel, newest first): go (Enter), remove (Del), add to the hotlist (F2)"},
                 {L"Ctrl+Z", L"Undo the last rename, move, copy, attribute change or new folder/file (only while the items are exactly as it left them; session only)"},
                 {L"Ctrl+U", L"Swap the left and right panels (folder, history, sort, filter, selection, cursor); the active side stays"},
                 {L"Ctrl+R", L"Refresh (re-read) the active panel"},
             }},
            {L"Selection",
             {
                 {L"Insert", L"Toggle selection on the cursor entry and move down"},
                 {L"Shift+Up / Shift+Down", L"Toggle selection on each entry as the cursor moves over it"},
                 {L"Numpad + / -", L"Select / deselect entries matching a wildcard mask"},
                 {L"Ctrl+Numpad + / -", L"Select all / clear selection, unconditionally"},
                 {L"Numpad *", L"Invert selection"},
                 {L"Ctrl+Shift+C", L"Copy the selected names (or the cursor entry) to the clipboard: names, full paths, or full paths with size/date"},
             }},
            {L"File operations",
             {
                 {L"F2", L"Rename the cursor entry"},
                 {L"Ctrl+M", L"Group rename the selection: masks [N] [E] [C] [YMD]..., search/replace, case, preview first"},
                 {L"F5", L"Copy the selection (or cursor entry) to the other panel"},
                 {L"Shift+F5", L"Copy the cursor entry under a new name in the same folder (or to a full path)"},
                 {L"F6", L"Move the selection (or cursor entry) to the other panel"},
                 {L"F7", L"Create a new directory"},
                 {L"Shift+F7", L"Create a new, empty file"},
                 {L"F8 / Delete", L"Delete the selection (or cursor entry) \x2014 Recycle Bin; Delete forward-deletes command-line text instead if it has any"},
                 {L"Shift+F8 / Shift+Delete", L"Delete permanently (bypasses the Recycle Bin)"},
                 {L"Ctrl+A", L"Toggle a Windows attribute (read-only/hidden/system/archive)"},
                 {L"Alt+Shift+Enter", L"Used Disk Area: recursive size of the selection (or cursor entry)"},
                 {L"Alt+Shift+V", L"Paste files copied or cut in Windows Explorer into the active folder (as a copy or a move)"},
                 {L"Menu: File > Secure delete", L"Overwrite then permanently delete files (limits are shown; not undoable)"},
                 {L"Menu: File > Generate Check Code", L"Create or verify MD5/SHA-1/SHA-256/SHA-512 checksum files for the selection"},
             }},
            {L"Panel view & network",
             {
                 {L"Ctrl+F7 / Ctrl+F8", L"Full (detailed rows) / Brief (names only, several columns) panel view"},
                 {L"Left / Right (Brief view)", L"Move one column back / forward"},
                 {L"Page Up / Page Down (Brief view)", L"Move one screenful of columns"},
                 {L"Menu: Commands > Disk Label", L"Show/change the active panel's volume label"},
                 {L"Menu: Network > Network Connection", L"List, map, disconnect, and open network drives"},
                 {L"Menu: Network > Share Folder / Unshare Folder", L"Share or unshare the active panel's folder (needs administrator rights)"},
             }},
            {L"Viewing & editing",
             {
                 {L"F3", L"View the cursor file (its default view action; built-in viewer if none)"},
                 {L"Shift+F3", L"Choose one of the cursor file's registered view/open actions"},
                 {L"Shift+Enter", L"Open the cursor file (its default open action; Windows association if none)"},
                 {L"F4", L"Edit the cursor file in an external editor"},
                 {L"Alt+Enter", L"Show file properties (size, version info, digital signature)"},
                 {L"F9", L"Open a separate interactive shell in the active directory"},
             }},
            {L"Sorting & filtering",
             {
                 {L"Ctrl+F3", L"Sort the active panel by name"},
                 {L"Ctrl+F4", L"Sort the active panel by extension (the type)"},
                 {L"Ctrl+F5", L"Sort the active panel by size"},
                 {L"Ctrl+F6", L"Sort the active panel by modification time"},
                 {L"Ctrl+F9", L"Sort the active panel by attributes (read-only, hidden, system, archive, link groups)"},
                 {L"Ctrl+F", L"Filter the active panel by name/wildcard (empty clears it)"},
                 {L"Ctrl+H", L"Toggle hidden and system entries in the active panel"},
                 {L"Alt+F7", L"Find files under the active panel: name mask and optional containing text (plain or regex)"},
                 {L"Shift+F2", L"Compare the two panels' folders and select the differences (name/size/time/content)"},
                 {L"Ctrl+Shift+F2", L"Synchronize the two panels' folders (preview, then confirm)"},
                 {L"Alt+F3", L"Show the differences between two text files (File > Compare contents)"},
                 {L"Shift+Alt+F3", L"The same, always in the built-in viewer (even with diffProgram set)"},
             }},
            {L"Compare contents (Alt+F3)",
             {
                 {L"Up / Down, Page Up / Page Down, Home / End", L"Scroll both files together"},
                 {L"Left / Right", L"Scroll sideways"},
                 {L"F7 / Shift+F7 (n / p)", L"Next / previous difference"},
                 {L"U", L"Side by side <-> unified (one column)"},
                 {L"I / W", L"Ignore case on/off / whitespace: exact, trailing ignored, all ignored"},
                 {L"F3 / Shift+F3", L"View the left / right file at this line"},
                 {L"Esc / F10 / q", L"Close"},
             }},
            {L"Command line",
             {
                 {L"Left / Right", L"Move the command-line cursor one character, if it has text"},
                 {L"Ctrl+Enter", L"Insert the active/selected name(s) into the command line"},
                 {L"Ctrl+Shift+Enter", L"Insert the active/selected full path(s) into the command line"},
                 {L"Ctrl+Up / Ctrl+Down", L"Browse command history into the command line"},
                 {L"Ctrl+Space", L"Complete the path being typed on the command line: extends to the common prefix, press again to list and cycle (Tab still switches panels; local folders only)"},
                 {L"Alt+R", L"Reverse history search: type a fragment, Alt+R steps to older matches, Enter puts the match on the command line (not run), Esc cancels"},
                 {L"Esc", L"Clear the command line if it has text"},
             }},
            {L"Built-in viewer (F3)",
             {
                 {L"Up / Down, Page Up / Page Down, Home / End", L"Scroll (text) or move the hex cursor by row/page/file start-end"},
                 {L"/", L"Search: plain text, or #hex bytes such as #4D 5A"},
                 {L"n", L"Repeat the last search"},
                 {L"H", L"Switch between text and hex view"},
                 {L"W", L"Hex view: cycle items per row 8 / 16 / 32 / fit to the window"},
                 {L"Left / Right", L"Hex view: move the cursor one byte"},
                 {L"Esc / F3 / F10", L"Close the viewer"},
             }},
            {L"Single-panel layout (Ctrl+F1)",
             {
                 {L"Ctrl+F1", L"Show only the active panel, full width; Ctrl+F1 again shows both"},
                 {L"Tab", L"Single-panel layout: switch which panel is shown"},
             }},
            {L"Directory tree (Ctrl+T)",
             {
                 {L"Ctrl+T", L"Show the active panel as a directory tree; Ctrl+T again leaves it"},
                 {L"Up / Down, Page Up / Page Down, Home / End", L"Move the tree cursor"},
                 {L"Right", L"Expand the cursor directory (loads its subdirectories)"},
                 {L"+ / -", L"Expand / collapse the cursor directory (numpad or main keyboard, any layout)"},
                 {L"Left", L"Collapse the cursor directory, or move to its parent if already collapsed"},
                 {L"Enter", L"Go to the cursor directory and leave the tree"},
                 {L"Esc", L"Leave the tree without navigating"},
             }},
            {L"Background jobs (FOP-014)",
             {
                 {L"Alt+F5 / Alt+F6", L"Queue the selection as a background copy / move"},
                 {L"Alt+F8 / Alt+Shift+F8", L"Queue the selection as a background delete (Recycle Bin / permanent)"},
                 {L"Alt+Q", L"Show the background job queue; Enter or Del on a job cancels it, P pauses/resumes it, A pauses all, R resumes all"},
                 {L"P / Pause", L"While a progress box or scan is running: pause / resume it (Esc still cancels)"},
                 {L"F10", L"Quit; asks first when jobs are still running"},
             }},
            {L"Other",
             {
                 {L"F1", L"Show this shortcut reference"},
                 {L"F10", L"Quit"},
                 {L"Ctrl+F11", L"Open the config file in the external editor; changes apply live once you save"},
                 {L"Menu: Edit key bindings", L"Settings menu: change these shortcuts (keys.conf); this list shows your keys"},
             }},
        };
        return result;
    }();
    return sections;
}

// A single display row: a built-in section title (item == nullptr) or a
// built-in shortcut (sectionTitle == nullptr, item != nullptr) within the
// section above it, or -- FEA-0002 Phase 2, §7 -- a row of plugin-supplied
// Markdown content (isPluginRow == true; unlike the two raw-pointer fields
// above, which point into Sections()'s process-lifetime static data,
// `pluginText` owns its string, since plugin Markdown is fetched and parsed
// fresh every time this screen opens), or -- IS-0009 -- a plugin "selector"
// row (isPluginSelector == true): a full-width rule marking "a new plugin's
// help text starts here," distinct from every Markdown heading style this
// screen otherwise uses, so scrolling past several plugins' own content
// doesn't leave their boundaries indistinguishable from an ordinary `#`
// heading inside one of them.
struct DisplayRow {
    const wchar_t* sectionTitle = nullptr; // set only for a built-in section-header row
    const ShortcutItem* item = nullptr;    // set only for a built-in shortcut row

    bool isPluginRow = false;
    MarkdownLineKind pluginKind = MarkdownLineKind::Blank; // meaningful only if isPluginRow
    std::wstring pluginText;

    bool isPluginSelector = false; // IS-0009: a full-width rule preceding a plugin's own block
};

std::vector<DisplayRow> BuildRows() {
    std::vector<DisplayRow> rows;
    bool first = true;
    for (const auto& section : Sections()) {
        if (!first) rows.push_back(DisplayRow{});  // blank separator row
        first = false;
        rows.push_back(DisplayRow{section.title, nullptr});
        std::transform(section.items.begin(), section.items.end(), std::back_inserter(rows),
                       [](const auto& item) { return DisplayRow{nullptr, &item}; });
    }
    return rows;
}

// FEA-0002 Phase 2, §7: appends one more section, "Plugins", after every
// built-in category -- one sub-heading per entry in `pluginContent` (its
// plugin's own display name, rendered the same way a Markdown heading is),
// followed by that plugin's Markdown source parsed via
// MarkdownRenderer.h's deliberately minimal renderer. A no-op (returns
// `rows` unchanged) when no plugin declared kMcPluginCapHelp -- the
// ordinary case -- so the screen looks exactly as it did before this
// feature existed whenever no such plugin is loaded.
std::vector<DisplayRow> AppendPluginRows(std::vector<DisplayRow> rows, const std::vector<PluginHelpContent>& pluginContent) {
    if (pluginContent.empty()) return rows;

    rows.push_back(DisplayRow{});  // blank separator, matching the between-section spacing above
    rows.push_back(DisplayRow{L"Plugins", nullptr});

    bool firstPlugin = true;
    for (const auto& content : pluginContent) {
        if (!firstPlugin) rows.push_back(DisplayRow{});
        firstPlugin = false;

        // IS-0009: the selector row -- rendered via kAttrHeader (this
        // screen's own title-row color, used nowhere else in its body
        // content), so it can never be mistaken for a built-in section
        // title (kAttrSection) or a Markdown heading from inside the
        // plugin's own content (also kAttrSection, by design -- see
        // MarkdownLineKind::Heading's rendering below).
        DisplayRow selectorRow;
        selectorRow.isPluginSelector = true;
        rows.push_back(std::move(selectorRow));

        DisplayRow nameRow;
        nameRow.isPluginRow = true;
        nameRow.pluginKind = MarkdownLineKind::Heading;
        // IS-0011: show the plugin's own version (McPluginMetadata::version)
        // next to its name -- omitted entirely if a plugin left it empty,
        // rather than showing a dangling "v".
        nameRow.pluginText = content.version.empty() ? content.name : content.name + L" v" + content.version;
        rows.push_back(std::move(nameRow));

        for (const auto& line : ParseMinimalMarkdown(content.markdown)) {
            DisplayRow row;
            row.isPluginRow = true;
            row.pluginKind = line.kind;
            row.pluginText = line.text;
            rows.push_back(std::move(row));
        }
    }
    return rows;
}

constexpr size_t kKeyColumnWidth = 24;

} // namespace

void ShowShortcutReference(Console& console, const std::vector<PluginHelpContent>& pluginContent,
                           const KeyMap* keyMap) {
    const std::vector<DisplayRow> rows = AppendPluginRows(BuildRows(), pluginContent);
    int topLine = 0;

    auto contentHeight = [&]() { return std::max<int>(1, console.Height() - 2); };

    auto clampTop = [&]() {
        int maxTop = std::max<int>(0, static_cast<int>(rows.size()) - contentHeight());
        topLine = std::clamp(topLine, 0, maxTop);
    };

    auto paint = [&]() {
        console.Clear(kAttrNormal);
        SHORT width = console.Width();

        console.PutText(0, 0, PadOrTrim(kStrShortcutReferenceTitle, width), kAttrHeader);

        int rowsVisible = contentHeight();
        for (int row = 0; row < rowsVisible; ++row) {
            int index = topLine + row;
            SHORT y = static_cast<SHORT>(1 + row);
            if (index >= static_cast<int>(rows.size())) {
                console.PutText(0, y, PadOrTrim(L"", width), kAttrNormal);
                continue;
            }
            const DisplayRow& r = rows[index];
            if (r.isPluginSelector) {
                // IS-0009: a full-width rule, plain ASCII (no ACC-006
                // basic-symbol-mode dependency), styled with this screen's
                // own title-row color so it can never be confused with a
                // section title or a Markdown heading inside a plugin's
                // own content (both kAttrSection).
                console.PutText(0, y, std::wstring(static_cast<size_t>(width), L'='), kAttrHeader);
            } else if (r.sectionTitle) {
                console.PutText(0, y, PadOrTrim(r.sectionTitle, width), kAttrSection);
            } else if (r.item) {
                console.PutText(0, y, PadOrTrim(L"", width), kAttrNormal);  // clear the row first
                // KEY-005: a rebound command shows its current keys.
                std::wstring keyText = r.item->key;
                if (keyMap) {
                    if (auto relabeled = keyMap->RelabelKeyText(keyText)) keyText = *relabeled;
                }
                std::wstring keyCol = L"  " + PadToDisplayWidth(keyText, kKeyColumnWidth);
                console.PutText(0, y, keyCol, kAttrKey);
                SHORT descX = static_cast<SHORT>(StringDisplayWidth(keyCol));
                if (descX < width) {
                    console.PutText(descX, y, PadOrTrim(r.item->description, static_cast<size_t>(width - descX)),
                                    kAttrNormal);
                }
            } else if (r.isPluginRow) {
                switch (r.pluginKind) {
                    case MarkdownLineKind::Heading:
                        console.PutText(0, y, PadOrTrim(r.pluginText, width), kAttrSection);
                        break;
                    case MarkdownLineKind::Bullet:
                        console.PutText(0, y, PadOrTrim(L"  - " + r.pluginText, width), kAttrNormal);
                        break;
                    case MarkdownLineKind::Paragraph:
                        console.PutText(0, y, PadOrTrim(L"  " + r.pluginText, width), kAttrNormal);
                        break;
                    case MarkdownLineKind::Blank:
                    default:
                        console.PutText(0, y, PadOrTrim(L"", width), kAttrNormal);
                        break;
                }
            } else {
                console.PutText(0, y, PadOrTrim(L"", width), kAttrNormal);  // blank separator row
            }
        }

        std::wstring status = L" Line " + std::to_wstring(std::min<int>(topLine + 1, static_cast<int>(rows.size()))) +
                              L"-" + std::to_wstring(std::min<int>(topLine + rowsVisible, static_cast<int>(rows.size()))) +
                              L"/" + std::to_wstring(rows.size()) + kStrShortcutReferenceHintSuffix;
        console.PutText(0, static_cast<SHORT>(console.Height() - 1), PadOrTrim(status, width), kAttrStatusBar);
        console.Present();
    };

    clampTop();
    paint();

    bool running = true;
    while (running) {
        INPUT_RECORD record;
        DWORD read = 0;
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;

        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            console.UpdateSize();
            clampTop();
            paint();
            continue;
        }
        // IS-0008: same sign convention and 3-row granularity as main.cpp's
        // dual-pane wheel handling (positive delta = scrolled toward the
        // user = up), so both screens feel identical to use. Mouse position
        // is irrelevant to a wheel event and isn't read; every other mouse
        // event (click/move/drag) still falls through unhandled — this
        // screen is read-only reference text with nothing a click could act
        // on, and the issue only asked for scrolling.
        if (record.EventType == MOUSE_EVENT && record.Event.MouseEvent.dwEventFlags == MOUSE_WHEELED) {
            short wheelDelta = static_cast<short>(HIWORD(record.Event.MouseEvent.dwButtonState));
            topLine += (wheelDelta > 0) ? -3 : 3;
            clampTop();
            paint();
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

        switch (record.Event.KeyEvent.wVirtualKeyCode) {
            case VK_UP:
                --topLine;
                clampTop();
                paint();
                break;
            case VK_DOWN:
                ++topLine;
                clampTop();
                paint();
                break;
            case VK_PRIOR:
                topLine -= contentHeight();
                clampTop();
                paint();
                break;
            case VK_NEXT:
                topLine += contentHeight();
                clampTop();
                paint();
                break;
            case VK_HOME:
                topLine = 0;
                paint();
                break;
            case VK_END:
                topLine = std::max<int>(0, static_cast<int>(rows.size()) - contentHeight());
                paint();
                break;
            case VK_ESCAPE:
            case VK_F1:
            case VK_F10:
                running = false;
                break;
            default:
                break;
        }
    }
}

} // namespace mc
