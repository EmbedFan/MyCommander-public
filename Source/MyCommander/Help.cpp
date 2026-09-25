#include "Help.h"

#include "Strings.h"
#include "TextWidth.h"
#include "Theme.h"
#include "Version.h"

#include <windows.h>

#include <algorithm>
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
            {L"Navigation",
             {
                 {L"Tab", L"Switch active panel"},
                 {L"Up / Down", L"Move cursor"},
                 {L"Page Up / Page Down", L"Move a page at a time"},
                 {L"Home / End", L"Jump to first / last entry"},
                 {L"Alt+Left / Alt+Right", L"Go back / forward through the active panel's location history"},
                 {L"Enter", L"Open a directory, run the command line, or run the default file action"},
                 {L"Backspace", L"Parent directory, or edit the command line if it has text"},
                 {L"Alt+F1 / Alt+F2", L"Choose a drive for the left / right panel"},
             }},
            {L"Selection",
             {
                 {L"Insert", L"Toggle selection on the cursor entry and move down"},
                 {L"Numpad + / -", L"Select / deselect entries matching a wildcard mask"},
                 {L"Ctrl+Numpad + / -", L"Select all / clear selection, unconditionally"},
                 {L"Numpad *", L"Invert selection"},
             }},
            {L"File operations",
             {
                 {L"F2", L"Rename the cursor entry"},
                 {L"F5", L"Copy the selection (or cursor entry) to the other panel"},
                 {L"F6", L"Move the selection (or cursor entry) to the other panel"},
                 {L"F7", L"Create a new directory"},
                 {L"Shift+F7", L"Create a new, empty file"},
                 {L"F8", L"Delete the selection (or cursor entry) \x2014 Recycle Bin"},
                 {L"Shift+F8", L"Delete permanently (bypasses the Recycle Bin)"},
                 {L"Ctrl+A", L"Toggle a Windows attribute (read-only/hidden/system/archive)"},
             }},
            {L"Viewing & editing",
             {
                 {L"F3", L"View the cursor file (built-in viewer)"},
                 {L"F4", L"Edit the cursor file in an external editor"},
                 {L"F9", L"Open a separate interactive shell in the active directory"},
             }},
            {L"Sorting & filtering",
             {
                 {L"Ctrl+F3", L"Sort the active panel by name"},
                 {L"Ctrl+F4", L"Sort the active panel by extension/type"},
                 {L"Ctrl+F5", L"Sort the active panel by size"},
                 {L"Ctrl+F6", L"Sort the active panel by modification time"},
                 {L"Ctrl+F", L"Filter the active panel by name/wildcard (empty clears it)"},
                 {L"Ctrl+H", L"Toggle hidden and system entries in the active panel"},
                 {L"Alt+F7", L"Find files recursively (name/wildcard) under the active panel"},
             }},
            {L"Command line",
             {
                 {L"Ctrl+Enter", L"Insert the active/selected name(s) into the command line"},
                 {L"Ctrl+Shift+Enter", L"Insert the active/selected full path(s) into the command line"},
                 {L"Ctrl+Up / Ctrl+Down", L"Browse command history into the command line"},
                 {L"Esc", L"Clear the command line if it has text"},
             }},
            {L"Other",
             {
                 {L"F1", L"Show this shortcut reference"},
                 {L"F10", L"Quit"},
                 {L"Ctrl+F11", L"Open the config file in the external editor; changes apply live once you save"},
             }},
        };
        return result;
    }();
    return sections;
}

// A single display row: either a section title (item == nullptr) or one
// shortcut within the section above it.
struct DisplayRow {
    const wchar_t* sectionTitle = nullptr; // set only for a section-header row
    const ShortcutItem* item = nullptr;    // set only for a shortcut row
};

std::vector<DisplayRow> BuildRows() {
    std::vector<DisplayRow> rows;
    bool first = true;
    for (const auto& section : Sections()) {
        if (!first) rows.push_back(DisplayRow{});  // blank separator row
        first = false;
        rows.push_back(DisplayRow{section.title, nullptr});
        for (const auto& item : section.items) {
            rows.push_back(DisplayRow{nullptr, &item});
        }
    }
    return rows;
}

constexpr size_t kKeyColumnWidth = 24;

} // namespace

void ShowShortcutReference(Console& console) {
    const std::vector<DisplayRow> rows = BuildRows();
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
            if (r.sectionTitle) {
                console.PutText(0, y, PadOrTrim(r.sectionTitle, width), kAttrSection);
            } else if (r.item) {
                console.PutText(0, y, PadOrTrim(L"", width), kAttrNormal);  // clear the row first
                std::wstring keyCol = L"  " + PadToDisplayWidth(r.item->key, kKeyColumnWidth);
                console.PutText(0, y, keyCol, kAttrKey);
                SHORT descX = static_cast<SHORT>(StringDisplayWidth(keyCol));
                if (descX < width) {
                    console.PutText(descX, y, PadOrTrim(r.item->description, static_cast<size_t>(width - descX)),
                                    kAttrNormal);
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
