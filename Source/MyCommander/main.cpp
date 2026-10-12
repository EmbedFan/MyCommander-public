#include "CommandHistory.h"
#include "Config.h"
#include "Console.h"
#include "ContentSearch.h"
#include "Dialog.h"
#include "DiffViewer.h"
#include "DirCompare.h"
#include "DirSync.h"
#include "DirectoryTree.h"
#include "Elevation.h"
#include "Favorites.h"
#include "MultiRename.h"
#include "FileActions.h"
#include "FileInfo.h"
#include "FileOps.h"
#include "Help.h"
#include "JobWorker.h"
#include "HintBar.h"
#include "Log.h"
#include "MenuBar.h"
#include "Navigation.h"
#include "Panel.h"
#include "PathUtil.h"
#include "PluginManager.h"
#include "Process.h"
#include "Search.h"
#include "Session.h"
#include "Strings.h"
#include "TextDiff.h"
#include "TextFile.h"
#include "TextWidth.h"
#include "Theme.h"
#include "Utf8TextFile.h"
#include "Version.h"
#include "Viewer.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <filesystem>
#include <map>
#include <numeric>
#include <optional>
#include <set>

#pragma comment(lib, "ole32.lib")

using mc::CheckCopyMoveDestination;
using mc::ConflictAskResult;
using mc::ColorTheme;
using mc::ColumnId;
using mc::ConflictChoice;
using mc::Config;
using mc::ConfigFilePath;
using mc::AppendMissingConfigKeys;
using mc::Console;
using mc::CopyItems;
using mc::CreateDestinationDirectory;
using mc::CreateNewDirectory;
using mc::CreateNewFile;
using mc::DeleteItems;
using mc::DestinationState;
using mc::DialogOption;
using mc::DirectoryTree;
using mc::FindFilesystemRoot;
using mc::IPanelContent;
using mc::TreeKeyAction;
using mc::ClassifyTreeKey;
using mc::TreeNode;
using mc::Job;
using mc::JobKind;
using mc::JobState;
using mc::JobWorker;
using mc::PendingPrompt;
using mc::PromptKind;
using mc::ElevatedAction;
using mc::ElevatedRequest;
using mc::EnsureConfigFileExists;
using mc::Entry;
using mc::EnterFileAction;
using mc::ErrorAskResult;
using mc::ErrorChoice;
using mc::FileAttributeFlag;
using mc::FormatWinError;
using mc::IsRunnableExtension;
using mc::ReadFileVersionFields;
using mc::FileVersionFields;
using mc::CheckDigitalSignature;
using mc::SignatureInfo;
using mc::SignatureStatus;
using mc::HintContext;
using mc::kMaxCommandHistoryEntries;
using mc::LoadCommandHistory;
using mc::LoadConfig;
using mc::ClassifyNavigationKey;
using mc::LogLevel;
using mc::Logger;
using mc::NavIntent;
using mc::NavKeyContext;
using mc::HitTestPanel;
using mc::MoveItems;
using mc::DetachedProcessResult;
using mc::OperationOutcome;
using mc::Panel;
using mc::PanelHit;
using mc::PathsEquivalent;
using mc::ClickableRegion;
using mc::MatchClickRegion;
using mc::HintAction;
using mc::HintSegment;
using mc::HintBarRowCount;
using mc::LayOutHintBar;
using mc::PlacedHintSegment;
using mc::ProcessRunResult;
using mc::RenameItem;
using mc::RenameResult;
using mc::ResolveExistingDirectory;
using mc::RunAndWait;
using mc::RunAsElevatedHelperIfRequested;
using mc::RunDetached;
using mc::RunElevated;
using mc::SearchFiles;
using mc::SearchOutcome;
using mc::SearchResult;
using mc::LoadSession;
using mc::SaveCommandHistory;
using mc::SaveSession;
using mc::SessionState;
using mc::ShowChoiceDialog;
using mc::ShowListChoice;
using mc::ShowMessage;
using mc::ShowShortcutReference;
using mc::ShowTextFileViewer;
using mc::SortKey;
using mc::ToggleAttributes;
using mc::ShowTextPrompt;
using mc::StringDisplayWidth;
using mc::UpdateConfigValue;
using mc::LineEdit;
using mc::ApplyLineEditKey;
using mc::MenuBarState;
using mc::MenuItem;
using mc::MenuItemKind;
using mc::TopMenu;
using mc::PlacedMenuEntry;
using mc::DropdownLayout;
using mc::BuildMenus;
using mc::OpenMenu;
using mc::CloseMenu;
using mc::MoveToAdjacentTopMenu;
using mc::MoveHighlight;
using mc::FindTopMenuByMnemonic;
using mc::FindItemByMnemonic;
using mc::LayOutMenuBarRow;
using mc::LayOutOpenDropdown;
using mc::PluginManager;
// RegisterThemeChangeListener (Theme.h) is already at global scope, like
// every other symbol in that header (see its own top comment) -- no
// `using` declaration needed or possible.

namespace {

// UI-005: right-aligned to a caller-supplied width (Config::sizeColumnWidth)
// rather than a hardcoded 10, so the size column's configured width is
// honored for real byte counts, not just the panel-row layout math.
std::wstring FormatSize(uint64_t bytes, int width) {
    wchar_t buf[32];
    swprintf_s(buf, L"%*llu", width, static_cast<unsigned long long>(bytes));
    return buf;
}

// FOP-009: compact human-readable byte count for the copy/move progress
// line ("3.2 MB"), unlike FormatSize's fixed-width raw-byte panel column.
std::wstring FormatHumanBytes(double bytes) {
    static const wchar_t* kUnits[] = {L"B", L"KB", L"MB", L"GB", L"TB"};
    size_t unit = 0;
    while (bytes >= 1024.0 && unit + 1 < sizeof(kUnits) / sizeof(kUnits[0])) {
        bytes /= 1024.0;
        ++unit;
    }
    wchar_t buf[32];
    if (unit == 0) {
        swprintf_s(buf, L"%.0f %s", bytes, kUnits[unit]);
    } else {
        swprintf_s(buf, L"%.1f %s", bytes, kUnits[unit]);
    }
    return buf;
}

// UI-011: delegates to the shared display-width-aware helper so panel rows,
// dialogs, and status/hint lines all stay column-aligned even when a name
// or path contains full-width (CJK) characters â€” plain code-unit counting
// (the old std::wstring::size()-based version of this function) drifts out
// of alignment as soon as one appears.
std::wstring PadOrTrim(const std::wstring& text, size_t width) {
    return mc::PadToDisplayWidth(text, width);
}

// CON-005: a compact Total-Commander-style attribute column â€” one letter per
// toggleable Windows attribute (Read-only/Hidden/System/Archive), '-' where
// unset. Directory-ness is already shown via color and "<DIR>", so that bit
// isn't repeated here.
std::wstring FormatAttributes(uint32_t attrs) {
    std::wstring s = L"----";
    if (attrs & FILE_ATTRIBUTE_READONLY) s[0] = L'R';
    if (attrs & FILE_ATTRIBUTE_HIDDEN) s[1] = L'H';
    if (attrs & FILE_ATTRIBUTE_SYSTEM) s[2] = L'S';
    if (attrs & FILE_ATTRIBUTE_ARCHIVE) s[3] = L'A';
    return s;
}

// UI-004's "type": with no file-extension-association database (EXT-*, and
// the CFG-* system it would need, aren't built), the honest minimal
// interpretation is the extension itself, without the leading dot.
// Directories show nothing here â€” "<DIR>" already marks them in the size
// column, so this isn't repeated.
std::wstring FormatType(const Entry& entry) {
    // NAV-009: the explicit label makes a link/junction distinguishable from
    // an ordinary directory or file even though Windows exposes both through
    // the same directory iterator.
    if (entry.isReparsePoint) return kStrTypeLink;
    if (entry.isDirectory) return L"";
    size_t dot = entry.name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return L"";  // no extension, or a dotfile like ".gitignore"
    std::wstring ext = entry.name.substr(dot + 1);
    if (ext.size() > 6) ext.resize(6);
    return ext;
}

// UI-004's modification time, from the raw Win32 FILETIME Panel stores.
// Converted to local time since that's what a user browsing files expects
// to see, not UTC.
std::wstring FormatModifiedTime(uint64_t filetimeRaw) {
    if (filetimeRaw == 0) return L"";
    FILETIME ft;
    ft.dwLowDateTime = static_cast<DWORD>(filetimeRaw & 0xFFFFFFFFu);
    ft.dwHighDateTime = static_cast<DWORD>(filetimeRaw >> 32);
    FILETIME localFt;
    SYSTEMTIME st;
    if (!FileTimeToLocalFileTime(&ft, &localFt) || !FileTimeToSystemTime(&localFt, &st)) return L"";
    wchar_t buf[24];
    swprintf_s(buf, L"%04u-%02u-%02u %02u:%02u", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

std::wstring BuildStatusLine(const Panel& panel) {
    int count = panel.SelectedCount();
    if (count > 0) {
        wchar_t buf[64];
        swprintf_s(buf, L"%d selected, %llu bytes", count,
                   static_cast<unsigned long long>(panel.SelectedSizeBytes()));
        return buf;
    }
    return panel.StatusMessage();
}

std::filesystem::path ResolveStartPath(const std::filesystem::path& p) {
    std::error_code ec;
    std::filesystem::path abs = std::filesystem::absolute(p, ec);
    return ec ? p : abs;
}

// No configuration system exists yet (CFG-*), so VEE-006's "configured
// external editor" is approximated with the conventional %EDITOR% variable,
// falling back to notepad.exe â€” always present, needs no setup, and safe as
// a zero-config default.
std::wstring ResolveEditorCommand() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"EDITOR", buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) return buf;
    return L"notepad.exe";
}

// Same zero-config approximation as ResolveEditorCommand, for CLI-005's
// "separate shell in the active directory": %COMSPEC% is what Windows
// itself already uses to record the configured command interpreter, so
// this needs no configuration system to be correct.
std::wstring ResolveShellCommand() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"COMSPEC", buf, MAX_PATH);
    if (len > 0 && len < MAX_PATH) return buf;
    return L"cmd.exe";
}

// Prints a pause prompt directly to the real console screen buffer (the one
// SuspendForChildProcess() just handed back) and blocks for one keypress,
// so a quick command's output is actually visible before ResumeAfterChildProcess()
// hands the screen back to our own double-buffered display and overwrites it.
void PauseForOutput(Console& console) {
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    const wchar_t* msg = L"\r\n-- command finished, press any key to continue --";
    DWORD written = 0;
    WriteConsoleW(out, msg, static_cast<DWORD>(wcslen(msg)), &written, nullptr);

    INPUT_RECORD record;
    DWORD read = 0;
    while (true) {
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;
        if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown) break;
    }
}

// CLI-003: appends the active entry's name (or every selected entry's name,
// space-separated) to `commandLine`, quoting any that contain a space â€”
// or, with `fullPath`, the same but as an absolute path ("active path").
void InsertActiveReference(const Panel& panel, bool fullPath, std::wstring& commandLine) {
    std::vector<std::wstring> names = panel.SelectionOrCursor();
    if (names.empty()) return;

    if (!commandLine.empty() && commandLine.back() != L' ') commandLine += L' ';
    for (size_t i = 0; i < names.size(); ++i) {
        if (i) commandLine += L' ';
        std::wstring value = fullPath ? (panel.Path() / names[i]).wstring() : names[i];
        if (value.find(L' ') != std::wstring::npos) {
            commandLine += L'"' + value + L'"';
        } else {
            commandLine += value;
        }
    }
}

// NAV-003: if `text` names an existing directory (per PathUtil.h's
// ResolveExistingDirectory), navigates the active panel there and returns
// true. Returns false for anything else, so the caller falls back to
// running the text as a shell command exactly as before.
bool TryNavigateToTypedPath(Panel& target, const std::wstring& text) {
    auto resolved = ResolveExistingDirectory(target.Path(), text);
    if (!resolved) return false;
    target.NavigateTo(*resolved);
    return true;
}

// NAV-005/LIST-002/LIST-003: shown in each panel's own header so its
// current (independent) sort key/direction is visible at a glance.
std::wstring SortIndicator(SortKey key, bool descending) {
    std::wstring label;
    switch (key) {
        case SortKey::Name:
            label = kStrSortName;
            break;
        case SortKey::Extension:
            label = kStrSortExtension;
            break;
        case SortKey::Size:
            label = kStrSortSize;
            break;
        case SortKey::ModifiedTime:
            label = kStrSortDate;
            break;
    }
    return label + (descending ? L"\x2193" : L"\x2191");  // down/up arrow
}

std::wstring PanelDriveTypeLabel(uint32_t type) {
    switch (type) {
        case DRIVE_FIXED:
            return kStrDriveTypeFixed;
        case DRIVE_REMOVABLE:
            return kStrDriveTypeRemovable;
        case DRIVE_REMOTE:
            return kStrDriveTypeNetwork;
        case DRIVE_CDROM:
            return kStrDriveTypeOptical;
        case DRIVE_RAMDISK:
            return kStrDriveTypeRamDisk;
        default:
            return kStrUnknown;
    }
}

std::wstring FormatPanelSize(uint64_t bytes) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

// UI-003: unlike the Alt+F1/F2 picker, this compact form is always visible
// in each pane and tracks that pane's own current directory/volume.
std::wstring FormatPanelVolumeLine(const Panel& panel) {
    const mc::VolumeInfo& volume = panel.Volume();
    if (!volume.available) return kStrVolumeInfoUnavailable;

    std::wstring line = volume.root;
    if (!volume.label.empty()) line += L" [" + volume.label + L"]";
    line += L" " + PanelDriveTypeLabel(volume.driveType);
    if (volume.spaceKnown) {
        line += L"  " + FormatPanelSize(volume.freeBytes) + kStrVolumeFreeSlash + FormatPanelSize(volume.totalBytes);
    } else {
        line += kStrVolumeSpaceUnavailable;
    }
    return line;
}

HintContext GetHintContext(const Panel& panel, const std::wstring& commandLine) {
    if (!commandLine.empty()) return HintContext::CommandLine;
    if (panel.LocationInaccessible()) return HintContext::InaccessibleLocation;
    if (!panel.Filter().empty()) return HintContext::FilteredListing;
    if (panel.SelectedCount() > 0) return HintContext::Selection;

    const auto& entries = panel.Entries();
    int cursor = panel.Cursor();
    if (cursor < 0 || cursor >= static_cast<int>(entries.size())) return HintContext::EmptyListing;
    return entries[cursor].isDirectory ? HintContext::Directory : HintContext::File;
}

// UI-005: the currently-configured panel column layout. Set once from
// Config right after it loads in wmain (before the input loop starts) and
// never modified again â€” Config has no live-reload/in-app-settings path
// yet, so this is safe as module state rather than threading it through
// every one of DrawFrame's ~20 call sites (most of them dialog
// "redrawBackground" callbacks with no natural place to carry a Config
// reference through).
std::vector<ColumnId> gVisibleColumns = {ColumnId::Type, ColumnId::Attributes, ColumnId::ModifiedTime,
                                         ColumnId::Size};
int gTypeColumnWidth = 6;
int gMtimeColumnWidth = 16;
int gSizeColumnWidth = 10;

// IS-0016: the command line's horizontal scroll position (first visible
// character of the typed command), carried between DrawFrame calls the same
// way as the column settings above, so ComputeLineEditViewOffset can scroll
// minimally rather than recentering on every frame.
size_t gCommandLineViewOffset = 0;

// UI-013 Part A: the single-panel layout (Ctrl+F1 / View > Single panel;
// remembered in the Session file). A file-scope flag that DrawFrame and
// HandleMouseEvent read, rather than a DrawFrame parameter, so every one of
// the many "redraw the frame behind this dialog" calls keeps the layout too.
bool gSinglePanelLayout = false;

// FEA-0002 Phase 1: the built-in menu set merged with every currently
// loaded plugin's own menu overlay (PluginManager::MergedMenus) --
// refreshed once per main-loop iteration (right where PollPendingEdits/
// PollConfigWatch already run) rather than threaded as a parameter through
// DrawFrame/DrawMenuBar's own ~20 call sites, the same module-state
// trade-off gVisibleColumns above already established and for the exact
// same reason: most of those call sites are dialog "redrawBackground"
// callbacks with no natural place to carry a PluginManager reference
// through. Read (not just drawn) by HandleMouseEvent's and wmain's own
// keyboard dispatch's menu hit-testing/mnemonic lookups too, which is
// correct precisely because both only ever run *after* this same
// iteration's own refresh already happened.
std::vector<TopMenu> gMergedMenus;

// KEY-005: the live key map (keys.conf over the defaults), reloaded when the
// file's timestamp or size changes. Its address never changes, so HintBar's
// ActiveHintKeyMap and PluginManager can keep pointing at it.
struct KeyBindingsState {
    mc::KeyMap map = mc::KeyMap::Defaults();
    std::filesystem::file_time_type lastWrite{};
    uintmax_t lastSize = 0;
    bool fileSeen = false;
    ULONGLONG lastCheck = 0;
};
KeyBindingsState gKeys;

std::filesystem::path KeysFilePath() {
    std::filesystem::path config = ConfigFilePath();
    return config.empty() ? config : config.parent_path() / L"keys.conf";
}

// KEY-005: each menu item's current shortcut, right-aligned in its dropdown.
void ApplyMenuKeyTexts(std::vector<TopMenu>& menus, const PluginManager& pluginManager) {
    for (TopMenu& menu : menus) {
        for (MenuItem& item : menu.items) {
            item.keyText.clear();
            if (item.kind == MenuItemKind::SimulatedKey) {
                mc::KeyChord chord{item.action.virtualKeyCode, item.action.ctrlHeld, item.action.shiftHeld,
                                   item.action.altHeld};
                if (auto command = mc::CommandForDefaultChord(chord)) {
                    const auto& keys = gKeys.map.KeysFor(*command);
                    if (!keys.empty()) item.keyText = mc::FormatKeyChord(keys.front());
                } else {
                    item.keyText = mc::FormatKeyChord(chord);
                }
            } else if (item.kind == MenuItemKind::PluginCommand) {
                item.keyText = pluginManager.ShortcutTextFor(item.pluginId, item.pluginCommandId);
            }
        }
    }
}

// Attributes is always exactly 4 characters (one slot per R/H/S/A flag) â€”
// not meaningfully configurable, see Config.h's comment on why it's
// excluded from the width settings above.
constexpr size_t kAttrColWidth = 4;

// Right-aligns a short, plain-ASCII tag (sizeCol's "<DIR>"/"<BROKEN>"/
// "<REPARSE>" markers) within `width`, matching FormatSize's own
// right-alignment for an ordinary byte count in the same column.
std::wstring RightAlignAscii(const std::wstring& s, size_t width) {
    if (s.size() >= width) return s.substr(s.size() - width);
    return std::wstring(width - s.size(), L' ') + s;
}

size_t ColumnDisplayWidth(ColumnId col) {
    switch (col) {
        case ColumnId::Type:
            return static_cast<size_t>(gTypeColumnWidth);
        case ColumnId::Attributes:
            return kAttrColWidth;
        case ColumnId::ModifiedTime:
            return static_cast<size_t>(gMtimeColumnWidth);
        case ColumnId::Size:
            return static_cast<size_t>(gSizeColumnWidth);
    }
    return 0;
}

// UI-005/UI-004: drops columns from the end of `requested` (lowest
// priority â€” a user's own column order doubles as a drop priority) until
// what remains fits `availableWidth` alongside the selection marker and at
// least a sliver of name column. Generalizes UI-004's old hardcoded
// two-tier full/basic drop into an N-tier one driven by the configured
// column list.
std::vector<ColumnId> FitColumnsToWidth(const std::vector<ColumnId>& requested, SHORT availableWidth,
                                        size_t selectionMarkerWidth) {
    std::vector<ColumnId> cols = requested;
    auto reserved = [&] {
        return std::accumulate(cols.begin(), cols.end(), selectionMarkerWidth,
                               [](size_t total, ColumnId c) { return total + 1 + ColumnDisplayWidth(c); });
    };
    while (!cols.empty() && static_cast<size_t>(availableWidth) <= reserved() + 1) {
        cols.pop_back();
    }
    return cols;
}

void DrawPanel(Console& console, const Panel& panel, bool active, SHORT x, SHORT y, SHORT width, SHORT height) {
    // FEA-0002 Phase 3, Â§8: a panel with installed plugin content shows that
    // plugin's own `title` in place of the real path (the path/history
    // underneath is untouched, just not displayed while content covers it)
    // and no volume line (there's no real volume to report), then renders
    // the body through the content itself rather than Entries()/cursor.
    if (panel.Content()) {
        console.PutText(x, y, PadOrTrim(panel.ContentTitle(), width), active ? kAttrHeaderActive : kAttrHeaderInactive);
        console.PutText(x, static_cast<SHORT>(y + 1), PadOrTrim(L"", width), kAttrHeaderInactive);
        panel.Content()->Draw(console, x, static_cast<SHORT>(y + 2), width, static_cast<SHORT>(height - 2), active);
        return;
    }

    std::wstring pathLine = panel.Path().wstring();
    if (!panel.Filter().empty()) pathLine += L"  [" + panel.Filter() + L"]";
    if (panel.ShowingHiddenAndSystem()) pathLine += kStrHiddenAndSystemTag;
    if (panel.IsLoading()) pathLine += kStrLoadingTag;
    pathLine += L"  " + SortIndicator(panel.Sort(), panel.SortDescending());
    console.PutText(x, y, PadOrTrim(pathLine, width), active ? kAttrHeaderActive : kAttrHeaderInactive);
    console.PutText(x, static_cast<SHORT>(y + 1), PadOrTrim(FormatPanelVolumeLine(panel), width), kAttrHeaderInactive);

    const int visibleRows = height - 2;

    // LIST-006: an unavailable location (removed drive, unreachable network
    // share, permissions error) gets a banner inside the panel body itself
    // â€” not just the shared bottom status bar, which only ever reflects the
    // *active* panel and would otherwise leave the *other* panel silently
    // indistinguishable from an ordinary empty directory.
    if (panel.LocationInaccessible()) {
        console.PutText(x, static_cast<SHORT>(y + 2), PadOrTrim(L" " + panel.StatusMessage(), width), kAttrBrokenBanner);
        for (int row = 1; row < visibleRows; ++row) {
            console.PutText(x, static_cast<SHORT>(y + 2 + row), std::wstring(width, L' '), kAttrNormal);
        }
        return;
    }

    const auto& entries = panel.Entries();
    for (int row = 0; row < visibleRows; ++row) {
        SHORT rowY = static_cast<SHORT>(y + 2 + row);
        int index = panel.TopIndex() + row;
        if (index >= static_cast<int>(entries.size())) {
            console.PutText(x, rowY, std::wstring(width, L' '), kAttrNormal);
            continue;
        }
        const auto& entry = entries[index];
        bool isCursorRow = (index == panel.Cursor());
        WORD attr = kAttrNormal;
        if (entry.isDirectory) attr = kAttrDir;
        if (entry.selected) attr = kAttrSelected;
        // LIST-006: a broken reparse point overrides dir/selected coloring â€”
        // more important to notice than either.
        if (entry.inaccessible) attr = kAttrBroken;
        if (isCursorRow) {
            // ACC-002: a broken entry's color (kAttrBroken: FOREGROUND_RED
            // only) combined with the ordinary active-panel cursor
            // highlight (BACKGROUND_BLUE) computes to ~4.0:1 contrast â€”
            // just under WCAG AA's 4.5:1 minimum for normal text. Use
            // kAttrBrokenBanner's white-on-dark-red pairing instead
            // (~11.0:1), which keeps the "broken" signal at least as
            // strong; this is the one row style not derived from
            // kColorMask/BACKGROUND_BLUE, since none of that combination
            // keeps acceptable contrast for this specific color.
            attr = entry.inaccessible
                     ? kAttrBrokenBanner
                     : static_cast<WORD>((attr & kColorMask) | FOREGROUND_INTENSITY | (active ? BACKGROUND_BLUE : 0));
        }

        // ACC-001: selection was previously communicated by kAttrSelected's
        // color alone (unlike directory/broken/reparse status, which already
        // had a text marker below via sizeCol). A leading "* " (plain ASCII,
        // shown regardless of ACC-006 basic-symbol mode) makes a selected
        // entry identifiable from the text alone, e.g. under a monochrome or
        // color-filtered terminal. "  " for an unselected entry keeps every
        // row's name column starting at the same offset.
        constexpr size_t kSelectionMarkerWidth = 2;
        std::wstring selectionMarker = entry.selected ? L"* " : L"  ";

        std::wstring sizeCol = entry.inaccessible    ? RightAlignAscii(kStrSizeColBroken, ColumnDisplayWidth(ColumnId::Size))
                              : entry.isReparsePoint ? RightAlignAscii(kStrSizeColReparse, ColumnDisplayWidth(ColumnId::Size))
                              : entry.isDirectory    ? RightAlignAscii(kStrSizeColDir, ColumnDisplayWidth(ColumnId::Size))
                                                      : FormatSize(entry.sizeBytes, gSizeColumnWidth);

        // UI-004/UI-005: a row can show name plus any configured subset of
        // type/attributes/modification-time/size, but a narrow terminal
        // can't always fit every configured column next to a usable name
        // column â€” so columns drop out gracefully (from the end of the
        // user's own configured order) rather than truncating the name to
        // nothing.
        std::vector<ColumnId> cols = FitColumnsToWidth(gVisibleColumns, width, kSelectionMarkerWidth);
        const size_t reserved = std::accumulate(cols.begin(), cols.end(), kSelectionMarkerWidth,
                                                [](size_t total, ColumnId c) { return total + 1 + ColumnDisplayWidth(c); });
        size_t nameWidth = static_cast<size_t>(width) > reserved ? static_cast<size_t>(width) - reserved : 0;

        std::wstring line = selectionMarker + PadOrTrim(entry.name, nameWidth);
        for (ColumnId c : cols) {
            line += L" ";
            switch (c) {
                case ColumnId::Type:
                    line += PadOrTrim(FormatType(entry), static_cast<size_t>(gTypeColumnWidth));
                    break;
                case ColumnId::Attributes:
                    line += FormatAttributes(entry.attributes);
                    break;
                case ColumnId::ModifiedTime:
                    line += PadOrTrim(FormatModifiedTime(entry.lastWriteTimeUtc), static_cast<size_t>(gMtimeColumnWidth));
                    break;
                case ColumnId::Size:
                    line += sizeCol;
                    break;
            }
        }
        console.PutText(x, rowY, PadOrTrim(line, width), attr);
    }
}

// UI-013, Part B: a native (non-plugin) IPanelContent wrapping a
// DirectoryTree -- the "Tree" view-menu item / Ctrl+T key. One instance
// per panel side (see wmain's leftTreeContent/rightTreeContent), default-
// constructed and re-entered fresh every time the user asks for it;
// nothing about a tree session survives being left. Mirrors ZipPlugin's
// own install/release symmetry via Panel::SetContent, just without the
// plugin ABI boundary -- this lives in the host itself, so it can call
// straight back into Panel rather than going through McPluginHost.
class DirectoryTreeContent : public IPanelContent {
public:
    // `other` is the opposite side: it previews the cursor folder while the
    // tree is open and goes back to its own folder when the tree is left.
    void Enter(Panel& panel, Panel& other) {
        panel_ = &panel;
        other_ = &other;
        otherOriginal_ = other.Path();
        previewed_.clear();
        tree_.emplace(FindFilesystemRoot(panel.Path()), panel.Path());
        panel.SetContent(this, kStrTreeContentTitle);
        UpdatePreview();  // the other side shows this folder's listing straight away
    }

    void Draw(Console& console, SHORT x, SHORT y, SHORT width, SHORT height, bool active) override {
        lastVisibleRows_ = std::max<int>(1, height);
        if (!tree_) return;
        tree_->ScrollToCursor(lastVisibleRows_);
        const bool ascii = console.UseBasicSymbols();
        std::vector<size_t> visible = tree_->VisibleNodeIndices();
        int top = tree_->TopVisiblePosition();
        for (int row = 0; row < lastVisibleRows_; ++row) {
            SHORT rowY = static_cast<SHORT>(y + row);
            size_t pos = static_cast<size_t>(top + row);
            if (pos >= visible.size()) {
                console.PutText(x, rowY, std::wstring(static_cast<size_t>(width), L' '), kAttrNormal);
                continue;
            }
            const TreeNode& node = tree_->NodeAt(visible[pos]);
            bool hasChildren = node.childrenLoaded && !node.childIndices.empty();
            wchar_t glyph = L' ';
            // A not-yet-expanded node whose children haven't been scanned
            // yet still shows the collapsed glyph -- it might have
            // children; LIST-008/PER-003's "never scan until asked"
            // precedent means we genuinely don't know without expanding.
            if (!node.childrenLoaded || hasChildren) {
                glyph = node.expanded ? (ascii ? L'-' : L'\x25BE') : (ascii ? L'+' : L'\x25B8');
            }
            std::wstring label = node.parentIndex == SIZE_MAX ? node.path.wstring() : node.path.filename().wstring();
            std::wstring line = std::wstring(static_cast<size_t>(node.depth) * 2, L' ') + glyph + L' ' + label;
            if (node.loadFailed) {
                line += L"  ";
                line += kStrSizeColBroken;
            } else if (node.isReparsePoint) {
                line += L"  ";
                line += kStrSizeColReparse;
            }
            WORD attr = node.loadFailed ? kAttrBroken : kAttrDir;
            if (visible[pos] == tree_->CursorNodeIndex()) {
                attr = static_cast<WORD>((attr & kColorMask) | FOREGROUND_INTENSITY | (active ? BACKGROUND_BLUE : 0));
            }
            console.PutText(x, rowY, PadOrTrim(line, width), attr);
        }
    }

    void OnKey(WORD virtualKeyCode, DWORD controlKeyState, wchar_t unicodeChar) override {
        if (!tree_) return;
        bool ctrlHeld = (controlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        switch (ClassifyTreeKey(virtualKeyCode, ctrlHeld, unicodeChar)) {
            case TreeKeyAction::MoveUp:
                tree_->MoveCursor(-1, lastVisibleRows_);
                break;
            case TreeKeyAction::MoveDown:
                tree_->MoveCursor(1, lastVisibleRows_);
                break;
            case TreeKeyAction::PageUp:
                tree_->PageMove(-1, lastVisibleRows_);
                break;
            case TreeKeyAction::PageDown:
                tree_->PageMove(1, lastVisibleRows_);
                break;
            case TreeKeyAction::Home:
                tree_->CursorHome();
                break;
            case TreeKeyAction::End:
                tree_->CursorEnd(lastVisibleRows_);
                break;
            case TreeKeyAction::ExpandCursor:
                tree_->ExpandCursor(lastVisibleRows_);
                break;
            case TreeKeyAction::CollapseCursor:
                tree_->CollapseCursor(lastVisibleRows_);
                break;
            case TreeKeyAction::CollapseOrGoToParent:
                tree_->CollapseCursorOrGoToParent(lastVisibleRows_);
                break;
            case TreeKeyAction::Activate:
                NavigateHereAndLeave();
                break;
            case TreeKeyAction::Leave:
                // Ctrl+T (the key that entered the tree) and Esc both leave it.
                Leave();
                break;
            case TreeKeyAction::None:
                break;
        }
        UpdatePreview();
    }

    void OnMouseClick(SHORT relativeX, SHORT relativeY, bool doubleClick) override {
        if (!tree_) return;
        (void)relativeX;
        tree_->SelectVisibleRow(static_cast<int>(relativeY), lastVisibleRows_);
        UpdatePreview();
        if (doubleClick) NavigateHereAndLeave();
    }

private:
    Panel* panel_ = nullptr;
    Panel* other_ = nullptr;
    std::filesystem::path otherOriginal_;
    std::filesystem::path previewed_;
    std::optional<DirectoryTree> tree_;
    int lastVisibleRows_ = 1;

    void UpdatePreview() {
        if (!tree_ || !other_) return;
        const std::filesystem::path& path = tree_->CursorPath();
        if (path == previewed_) return;
        previewed_ = path;
        if (!mc::IsReparsePoint(path)) other_->PreviewLocation(path);
    }

    void NavigateHereAndLeave() {
        if (!tree_ || !panel_) return;
        std::filesystem::path chosen = tree_->CursorPath();
        Panel* panel = panel_;
        Leave();
        panel->NavigateTo(chosen);
    }

    void Leave() {
        if (other_ && other_->Path() != otherOriginal_) other_->PreviewLocation(otherOriginal_);
        other_ = nullptr;
        previewed_.clear();
        if (panel_) panel_->SetContent(nullptr);
        panel_ = nullptr;
        tree_.reset();
    }
};

// FEA-0005: the top menu bar's own row count â€” always exactly 1 (its six
// labels never come close to needing to wrap at the documented 60-column
// minimum, MenuBar.h's own LayOutMenuBarRow comment explains why) â€” kept as
// a named constant rather than a bare 1 so every row calculation below
// that depends on it reads as "the menu bar's rows," not a magic number.
// DrawFrame/ComputeVisibleRows/HitTestPanel/HandleMouseEvent all have to
// agree on this exact value, the same "must stay in agreement" discipline
// IS-0007's own hintRows already established for the bottom of the screen â€”
// this is that same risk, mirrored at the top.
constexpr SHORT kMenuBarRows = 1;

// FEA-0005: draws the always-visible top menu bar row, plus (while
// `menuState.active`) the currently open dropdown beneath it â€” the exact
// data LayOutMenuBarRow/LayOutOpenDropdown (MenuBar.h) already computed,
// rendered here rather than smuggled out of DrawFrame, matching
// LayOutHintBar's own precedent. Each entry/item renders as
// "[" + mnemonic + "]" + label (MenuItem's own doc comment explains why,
// unlike DialogOption's trimmed-label convention). The open menu's own top
// entry, and its currently highlighted item, are drawn with
// kAttrHeaderActive (the same "this is the focused thing" blue highlight
// the active panel's header already uses) instead of the bar's ordinary
// kAttrStatusBar â€” reusing existing theme attributes rather than adding a
// new one to Theme.h/ColorContrastTests.cpp for this alone.
void DrawMenuBar(Console& console, const MenuBarState& menuState) {
    SHORT width = console.Width();
    console.PutText(0, 0, std::wstring(width, L' '), kAttrStatusBar);

    const std::vector<TopMenu>& menus = gMergedMenus;
    std::vector<PlacedMenuEntry> placed = LayOutMenuBarRow(menus, width);
    SHORT openMenuCol = 0;
    for (const auto& entry : placed) {
        const TopMenu& menu = menus[static_cast<size_t>(entry.menuIndex)];
        bool isOpenMenu = menuState.active && entry.menuIndex == menuState.openMenuIndex;
        if (isOpenMenu) openMenuCol = entry.col;
        std::wstring text = L"[" + std::wstring(1, menu.mnemonic) + L"]" + menu.label;
        console.PutText(entry.col, 0, text, isOpenMenu ? kAttrHeaderActive : kAttrStatusBar);
    }

    if (!menuState.active || menuState.openMenuIndex < 0 ||
        menuState.openMenuIndex >= static_cast<int>(menus.size())) {
        return;
    }
    const TopMenu& openMenu = menus[static_cast<size_t>(menuState.openMenuIndex)];
    DropdownLayout dropdown = LayOutOpenDropdown(openMenu, openMenuCol, width);
    for (size_t i = 0; i < openMenu.items.size(); ++i) {
        const MenuItem& item = openMenu.items[i];
        bool isHighlighted = static_cast<int>(i) == menuState.highlightedItemIndex;
        std::wstring text = L"[" + std::wstring(1, item.mnemonic) + L"]" + item.label;
        if (!item.keyText.empty()) {
            // KEY-005: the shortcut right-aligned, one column of padding after it.
            size_t keyWidth = mc::StringDisplayWidth(item.keyText);
            size_t leftWidth = static_cast<size_t>(dropdown.width) > keyWidth + 1 ? dropdown.width - keyWidth - 1 : 0;
            text = PadOrTrim(text, leftWidth) + item.keyText;
        }
        console.PutText(dropdown.col, dropdown.itemRows[i], PadOrTrim(text, static_cast<size_t>(dropdown.width)),
                        isHighlighted ? kAttrHeaderActive : kAttrStatusBar);
    }
}

// Draws both panels, the command line/status/hint bars, and the top menu
// bar, but does not Present() â€” callers compose more onto the back buffer
// first (a dialog box, a progress line) and present once. Also reused as
// the "redrawBackground" callback dialogs call to repaint what's behind
// them, in which case `commandLine` and `menuState` are left at their
// defaults (empty / fully closed) since those callers don't have access to
// the live buffer/menu state â€” a purely cosmetic gap (the menu is never
// open while a dialog is showing anyway â€” see wmain's own dispatch), since
// the real buffer reappears correctly once the dialog closes and the main
// loop redraws again.
void DrawFrame(Console& console, const Panel& left, const Panel& right, bool leftActive,
              const std::wstring& commandLine = L"", const MenuBarState& menuState = MenuBarState{},
              size_t commandLineCursor = 0) {
    SHORT width = console.Width();
    SHORT height = console.Height();
    const Panel& active = leftActive ? left : right;
    // IS-0007: the hint bar grows to however many rows its current context
    // needs at the current width (recomputed every draw call, so a resize
    // or a context change â€” selecting an item, typing into the command
    // line â€” both take effect immediately, with no separate "on resize"
    // handling needed). The command line always gets one row; the status
    // line only gets one if it actually has something to say (BuildStatusLine
    // returns an empty string outside a selection/error â€” a reserved-but-
    // blank row there just reads as a dead strip of the hint bar's own
    // color sitting above it, once the hint bar is tall enough to make that
    // obvious, so it's skipped entirely rather than drawn empty).
    HintContext hintContext = GetHintContext(active, commandLine);
    SHORT hintRows = static_cast<SHORT>(HintBarRowCount(hintContext, width));
    std::wstring status = BuildStatusLine(active);
    bool hasStatusLine = !status.empty();
    SHORT reservedRows = static_cast<SHORT>(1 + (hasStatusLine ? 1 : 0) + hintRows);
    // FEA-0005: the frame's own drawable height now also excludes the menu
    // bar's row(s) at the top, not just the reserved rows at the bottom.
    SHORT contentHeight = static_cast<SHORT>(height - reservedRows - kMenuBarRows);
    // UI-013 Part A: one shared geometry for drawing and the mouse.
    const mc::PanelLayout layout = mc::ComputePanelLayout(width, leftActive, gSinglePanelLayout);

    // UI-014: frame both panes using the single-line Unicode equivalents of
    // the classic CP437 set. ACC-006's basic-symbol mode keeps +, -, and |
    // available for a terminal font without box-drawing glyphs.
    const bool ascii = console.UseBasicSymbols();
    const wchar_t horizontal = ascii ? L'-' : L'\x2500';
    const wchar_t vertical = ascii ? L'|' : L'\x2502';
    const wchar_t topLeft = ascii ? L'+' : L'\x250C';
    const wchar_t topJunction = ascii ? L'+' : L'\x252C';
    const wchar_t topRight = ascii ? L'+' : L'\x2510';
    const wchar_t bottomLeft = ascii ? L'+' : L'\x2514';
    const wchar_t bottomJunction = ascii ? L'+' : L'\x2534';
    const wchar_t bottomRight = ascii ? L'+' : L'\x2518';

    console.Clear(kAttrNormal);
    const SHORT bottom = static_cast<SHORT>(kMenuBarRows + contentHeight - 1);
    console.FillRow(kMenuBarRows, 0, width, horizontal, kAttrNormal);
    console.FillRow(bottom, 0, width, horizontal, kAttrNormal);
    console.PutText(0, kMenuBarRows, std::wstring(1, topLeft), kAttrNormal);
    console.PutText(static_cast<SHORT>(width - 1), kMenuBarRows, std::wstring(1, topRight), kAttrNormal);
    console.PutText(0, bottom, std::wstring(1, bottomLeft), kAttrNormal);
    console.PutText(static_cast<SHORT>(width - 1), bottom, std::wstring(1, bottomRight), kAttrNormal);
    if (layout.dividerX >= 0) {
        console.PutText(layout.dividerX, kMenuBarRows, std::wstring(1, topJunction), kAttrNormal);
        console.PutText(layout.dividerX, bottom, std::wstring(1, bottomJunction), kAttrNormal);
    }

    for (SHORT row = static_cast<SHORT>(kMenuBarRows + 1); row < bottom; ++row) {
        console.FillRow(row, 0, 1, vertical, kAttrNormal);
        if (layout.dividerX >= 0) console.FillRow(row, layout.dividerX, 1, vertical, kAttrNormal);
        console.FillRow(row, static_cast<SHORT>(width - 1), 1, vertical, kAttrNormal);
    }
    if (layout.showLeft)
        DrawPanel(console, left, leftActive, layout.leftX, static_cast<SHORT>(kMenuBarRows + 1), layout.leftWidth,
                  static_cast<SHORT>(contentHeight - 2));
    if (layout.showRight)
        DrawPanel(console, right, !leftActive, layout.rightX, static_cast<SHORT>(kMenuBarRows + 1), layout.rightWidth,
                  static_cast<SHORT>(contentHeight - 2));
    if (gSinglePanelLayout) {
        // UI-013 Part A: name the hidden panel on the bottom border, so F5/F6
        // and compare "to the other panel" stay understandable.
        const Panel& hidden = leftActive ? right : left;
        std::wstring where = hidden.Content() ? hidden.ContentTitle() : hidden.Path().wstring();
        std::wstring label = mc::FormatOtherPanelLabel(where, static_cast<int>(width) - 4);
        if (!label.empty())
            console.PutText(static_cast<SHORT>(width - 2 - StringDisplayWidth(label)), bottom, label, kAttrNormal);
    }

    std::wstring cmdPrefix = active.Path().wstring() + L"> ";
    // IS-0016: a deep path prompt may take at most half the row, shown as
    // "..." plus its tail, so the editable part always keeps room to scroll.
    const int maxPrefixWidth = std::max(4, static_cast<int>(width) / 2);
    if (StringDisplayWidth(cmdPrefix) > maxPrefixWidth) {
        std::wstring tail = cmdPrefix;
        while (!tail.empty() && StringDisplayWidth(tail) + 3 > maxPrefixWidth) tail.erase(0, 1);
        cmdPrefix = L"..." + tail;
    }
    // IS-0016: horizontal scrolling, so the cursor stays on screen however
    // long the typed command is.
    size_t cmdViewWidth = static_cast<size_t>(std::max(1, static_cast<int>(width) - StringDisplayWidth(cmdPrefix)));
    gCommandLineViewOffset =
        mc::ComputeLineEditViewOffset(commandLine, commandLineCursor, gCommandLineViewOffset, cmdViewWidth);
    std::wstring cmdLine = cmdPrefix + commandLine.substr(gCommandLineViewOffset);
    SHORT cmdLineRow = static_cast<SHORT>(height - reservedRows);
    console.PutText(0, cmdLineRow, PadOrTrim(cmdLine, width), kAttrCommandLine);
    // IS-0015 Part 3: a real cursor glyph at commandLineCursor's own
    // position (UI-011 display-width-aware), replacing the old "always
    // append a trailing _" rendering, which had no real cursor concept.
    // IS-0016: kAttrCursor, the look every line editor now shares.
    SHORT cmdCursorCol = static_cast<SHORT>(
        StringDisplayWidth(cmdPrefix) +
        StringDisplayWidth(commandLine.substr(gCommandLineViewOffset, commandLineCursor - gCommandLineViewOffset)));
    if (cmdCursorCol < width) {
        wchar_t cmdCursorChar = (commandLineCursor < commandLine.size()) ? commandLine[commandLineCursor] : L' ';
        console.PutText(cmdCursorCol, cmdLineRow, std::wstring(1, cmdCursorChar), kAttrCursor);
    }
    if (hasStatusLine) {
        console.PutText(0, static_cast<SHORT>(cmdLineRow + 1), PadOrTrim(status, width), kAttrStatusBar);
    }

    DrawMenuBar(console, menuState);

    // IS-0007: one PutText per hint row rather than the old single fixed
    // row â€” each row is cleared to full width first so a row that shrinks
    // back down (context or width changed since the last frame) doesn't
    // leave stale text from a previous, longer layout behind.
    SHORT hintBarTopRow = static_cast<SHORT>(height - hintRows);
    for (SHORT row = 0; row < hintRows; ++row) {
        console.PutText(0, static_cast<SHORT>(hintBarTopRow + row), std::wstring(width, L' '), kAttrStatusBar);
    }
    for (const auto& placed : LayOutHintBar(hintContext, width)) {
        console.PutText(placed.col, static_cast<SHORT>(hintBarTopRow + placed.row), placed.segment.label,
                        kAttrStatusBar);
    }
}

// UI-006: below this, the two-panel layout's column math (leftWidth/2,
// reserved cmdline/status/hint rows, per-row column widths) no longer has
// room to produce a usable panel, so the app refuses to draw it and shows
// a plain message instead until the terminal is grown back. Documented in
// README.md's "Running" section as the supported minimum.
constexpr SHORT kMinWidth = 60;
constexpr SHORT kMinHeight = 15;

bool IsTerminalTooSmall(const Console& console) {
    return console.Width() < kMinWidth || console.Height() < kMinHeight;
}

void DrawTooSmallMessage(Console& console) {
    console.Clear(kAttrNormal);
    std::wstring sizeLine = FormatMinimumSize(kMinWidth, kMinHeight, console.Width(), console.Height());
    SHORT midY = static_cast<SHORT>(std::max<SHORT>(0, static_cast<SHORT>(console.Height() / 2)));
    console.PutText(0, midY, PadOrTrim(kStrTerminalTooSmall, console.Width()), kAttrHeaderActive);
    if (static_cast<SHORT>(midY + 1) < console.Height()) {
        console.PutText(0, static_cast<SHORT>(midY + 1), PadOrTrim(sizeLine, console.Width()), kAttrNormal);
    }
}

// IS-0007/FEA-0005: `hintContext`/`hasStatusLine` must be the same ones
// DrawFrame's own layout used for this frame (GetHintContext(active panel,
// commandLine) / !BuildStatusLine(active panel).empty()) â€” this and
// DrawFrame have to agree on exactly how many rows are reserved, or mouse
// hit-testing and Page Up/Down will target rows that don't match what's
// actually drawn. kMenuBarRows (the top menu bar's own row) is the same
// constant DrawFrame itself uses, not a separate parameter â€” see its own
// comment for why that's safe (it never varies at runtime).
int ComputeVisibleRows(Console& console, HintContext hintContext, bool hasStatusLine) {
    SHORT hintRows = static_cast<SHORT>(HintBarRowCount(hintContext, console.Width()));
    SHORT reservedRows = static_cast<SHORT>(1 + (hasStatusLine ? 1 : 0) + hintRows);
    SHORT contentHeight = static_cast<SHORT>(console.Height() - reservedRows - kMenuBarRows);
    // DrawFrame reserves the top/bottom pane borders, and DrawPanel now has
    // both a path row and a UI-003 volume-information row before file rows.
    return std::max<int>(1, contentHeight - 4);
}

// UI-012: HitTestPanel/PanelHit (which panel, and which visible row within
// it, a mouse event's console-cell coordinates landed on) live in
// Navigation.h/.cpp, not here â€” pure functions of width/visibleRows/mouse
// coordinates with no Console/Panel dependency, kept Console-independent
// (matching Navigation.h's existing ClassifyNavigationKey) so the test
// project can exercise them directly, the same reasoning TST-005 already
// applied to keyboard dispatch.

// Drains all pending input, applying any resize seen along the way, and
// reports whether Esc was among the drained keys. Used to let a long copy
// or move be cancelled without blocking on ReadConsoleInputW. The actual
// drain logic lives in Console.cpp's mc::PollAndConsumeEscape, shared with
// PluginManager's own PollCancel host function (IS-0010).
bool PeekEscapePressed(Console& console) {
    return mc::PollAndConsumeEscape(console);
}

// Bundles the per-operation progress dialog and cancel polling so
// CopyItems/MoveItems callbacks stay short one-line lambdas at the call site.
// FOP-009: a modal-style box (not just a bottom status line) showing source,
// destination, current item/progress, and â€” for Copy/Move, once at least one
// sample has arrived via OnByteProgress â€” the current file's transferred-
// bytes/total and a live transfer speed. `primaryPath`/`destPath` are set by
// the caller right after construction: Copy/Move set both (labeled "Source"/
// "Destination"); Delete/ToggleAttributes set only `primaryPath` (labeled
// "Location", since there's no separate destination for those).
struct ProgressUi {
    Console& console;
    const Panel& left;
    const Panel& right;
    bool leftActive;
    std::wstring verb;

    std::wstring primaryPath;
    std::wstring destPath;

    std::wstring currentName;
    int currentDone = 0;
    int currentTotal = 0;

    uint64_t currentFileTransferred = 0;
    uint64_t currentFileTotal = 0;
    bool haveByteSample = false;   // any OnByteProgress call seen yet for the current item
    uint64_t sampleFileTotal = 0;  // the file total as of the last sample, to detect a new file starting
    uint64_t lastSampleBytes = 0;
    std::chrono::steady_clock::time_point lastSampleTime{};
    double smoothedBytesPerSec = 0.0;
    bool haveRate = false;
    std::chrono::steady_clock::time_point lastDrawTime{};
    std::unique_ptr<mc::AnimatedProgressDialog> animated;

    bool OnItemStart(const std::wstring& name, int done, int total) {
        currentName = name;
        currentDone = done;
        currentTotal = total;
        currentFileTransferred = 0;
        currentFileTotal = 0;
        haveByteSample = false;
        Draw();
        return !PeekEscapePressed(console);
    }

    // FOP-009: called periodically while the current file (or, for a
    // directory item, whichever nested file is actively streaming right
    // now) copies/moves. Speed is a smoothed rate over consecutive samples
    // within the same file; it isn't reset at a file boundary, so it keeps
    // reading as "how fast is this operation going" rather than dropping to
    // zero between files.
    void OnByteProgress(uint64_t transferred, uint64_t total) {
        auto now = std::chrono::steady_clock::now();
        bool sameFile = haveByteSample && total == sampleFileTotal && transferred >= lastSampleBytes;
        if (sameFile) {
            double dt = std::chrono::duration<double>(now - lastSampleTime).count();
            if (dt > 0.02) {
                double instantRate = static_cast<double>(transferred - lastSampleBytes) / dt;
                smoothedBytesPerSec = haveRate ? (smoothedBytesPerSec * 0.7 + instantRate * 0.3) : instantRate;
                haveRate = true;
                lastSampleTime = now;
                lastSampleBytes = transferred;
            }
        } else {
            lastSampleTime = now;
            lastSampleBytes = transferred;
        }
        sampleFileTotal = total;
        haveByteSample = true;
        currentFileTransferred = transferred;
        currentFileTotal = total;

        // Throttle redraws so a fast local copy doesn't flicker/spend most
        // of its time repainting the console instead of copying.
        if (lastDrawTime.time_since_epoch().count() != 0 &&
            now - lastDrawTime < std::chrono::milliseconds(100)) {
            return;
        }
        Draw();
    }

    bool PollCancel() { return PeekEscapePressed(console); }

private:
    void Draw() {
        std::vector<std::wstring> body;
        if (!primaryPath.empty()) {
            body.push_back((destPath.empty() ? kStrProgressLocationLabel : kStrProgressSourceLabel) + primaryPath);
        }
        if (!destPath.empty()) body.push_back(kStrProgressDestinationLabel + destPath);
        body.push_back(FormatProgressItemLine(currentDone + 1, currentTotal, currentName));
        if (haveByteSample && currentFileTotal > 0) {
            std::wstring bytesLine = FormatHumanBytes(static_cast<double>(currentFileTransferred)) + L" / " +
                                     FormatHumanBytes(static_cast<double>(currentFileTotal));
            if (haveRate) bytesLine += L"   " + FormatHumanBytes(smoothedBytesPerSec) + L"/s";
            body.push_back(bytesLine);
        }
        body.push_back(L"");
        body.push_back(kStrEscToCancel);

        if (!animated) animated = std::make_unique<mc::AnimatedProgressDialog>(console, verb, currentTotal,
            [this]() { DrawFrame(console, left, right, leftActive); });
        std::wstring details;
        for (const auto& line : body) { if (!details.empty()) details += L'\n'; details += line; }
        animated->Update(currentDone, currentTotal, details);
        lastDrawTime = std::chrono::steady_clock::now();
    }
};

ConflictAskResult AskConflict(Console& console, const Panel& left, const Panel& right, bool leftActive,
                              const std::filesystem::path& destination) {
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    std::wstring name = destination.filename().wstring();
    std::vector<std::wstring> lines = {L"\"" + name + kStrAlreadyExistsAt, destination.wstring()};
    std::vector<DialogOption> options = {{L'O', kStrOverwriteLabel}, {L'A', kStrOverwriteAllLabel}, {L'S', kStrSkipLabel},
                                         {L'K', kStrSkipAllLabel},   {L'R', kStrRenameLabel},          {L'C', kStrCancelLabel}};
    int choice = ShowChoiceDialog(console, kStrFileExistsTitle, lines, options, redraw);

    ConflictAskResult result;
    switch (choice) {
        case 0:
            result.choice = ConflictChoice::Overwrite;
            break;
        case 1:
            result.choice = ConflictChoice::Overwrite;
            result.applyToAll = true;
            break;
        case 2:
            result.choice = ConflictChoice::Skip;
            break;
        case 3:
            result.choice = ConflictChoice::Skip;
            result.applyToAll = true;
            break;
        case 4: {
            auto renamed = ShowTextPrompt(console, kStrRenameToTitle, name, redraw);
            if (!renamed || renamed->empty()) {
                result.choice = ConflictChoice::Cancel;
            } else {
                result.choice = ConflictChoice::Rename;
                result.renameTo = *renamed;
            }
            break;
        }
        default:
            result.choice = ConflictChoice::Cancel;
            break;
    }
    return result;
}

// FOP-010: asked when an item fails partway through an operation for a
// reason other than a name collision (AskConflict above) or a user
// cancellation â€” access denied, a sharing violation because something else
// has the file open, a full disk, and the like. Retry re-attempts the same
// item immediately (useful once the underlying cause â€” e.g. whatever had
// the file open â€” is fixed); Skip/Skip all mirror AskConflict's own
// Skip/Skip all.
// SEC-003: `win32Error == ERROR_ACCESS_DENIED` adds an extra "Elevate & Retry"
// hotkey that isn't offered for any other failure â€” elevation only ever
// plausibly helps a permissions problem, and offering it unconditionally
// would invite a UAC prompt for failures it can't fix (a locked file, a
// full disk, ...).
ErrorAskResult AskFileError(Console& console, const Panel& left, const Panel& right, bool leftActive,
                            const std::wstring& itemName, const std::wstring& errorMessage, DWORD win32Error) {
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    std::vector<std::wstring> lines = {L"\"" + itemName + L"\":", errorMessage};
    bool offerElevate = win32Error == ERROR_ACCESS_DENIED;
    std::vector<DialogOption> options = {{L'R', kStrRetryLabel}, {L'S', kStrSkipLabel}, {L'K', kStrSkipAllLabel}};
    if (offerElevate) options.push_back({L'E', kStrElevateAndRetryLabel});
    options.push_back({L'C', kStrCancelLabel});
    int choice = ShowChoiceDialog(console, kStrOperationFailedTitle, lines, options, redraw);

    ErrorAskResult result;
    if (choice == 0) {
        result.choice = ErrorChoice::Retry;
    } else if (choice == 1) {
        result.choice = ErrorChoice::Skip;
    } else if (choice == 2) {
        result.choice = ErrorChoice::Skip;
        result.applyToAll = true;
    } else if (offerElevate && choice == 3) {
        result.choice = ErrorChoice::ElevateAndRetry;
    } else {
        result.choice = ErrorChoice::Cancel;
    }
    return result;
}

// Reports what happened after a multi-item operation (FOP-011). Silent on a
// clean, fully successful run so routine copies don't interrupt the workflow.
// ERR-004: every failed item and, separately, a cancellation are also
// logged (Error/Warning) â€” this is the one place all of Copy/Move/Delete/
// Toggle-attribute's failures funnel through, so it's the natural single
// hook point rather than instrumenting each operation individually.
void ReportOutcome(Console& console, const Panel& left, const Panel& right, bool leftActive, const std::wstring& verb,
                   const OperationOutcome& outcome, Logger& logger) {
    for (const auto& failure : outcome.failed) {
        logger.Log(LogLevel::Error, verb + L" failed: " + failure.first + L": " + failure.second);
    }
    if (outcome.cancelled) {
        logger.Log(LogLevel::Warning, verb + L" was cancelled by the user");
    }

    bool noteworthy = !outcome.failed.empty() || outcome.cancelled || outcome.skipped > 0;
    if (!noteworthy) return;

    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    std::wstring header =
        FormatOperationResultHeader(outcome.succeeded, outcome.skipped, outcome.failed.size(), outcome.cancelled);

    std::vector<std::wstring> lines{header};
    size_t shown = std::min<size_t>(outcome.failed.size(), 5);
    for (size_t i = 0; i < shown; ++i) {
        lines.push_back(outcome.failed[i].first + L": " + outcome.failed[i].second);
    }
    if (outcome.failed.size() > shown) {
        lines.push_back(FormatMoreFailures(outcome.failed.size() - shown));
    }
    ShowMessage(console, FormatOperationResultTitle(verb), lines, redraw);
}

// SRC-003: filters the active panel's own listing by name/wildcard. An
// empty submission (just pressing Enter on the pre-filled prompt) clears it.
void DoFilter(Console& console, Panel& left, Panel& right, bool leftActive) {
    Panel& target = leftActive ? left : right;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    auto patternOpt = ShowTextPrompt(console, kStrFilterPrompt, target.Filter(), redraw);
    if (!patternOpt) return;

    target.SetFilter(*patternOpt);
}

// SEL-003: sets or clears the selection mark on every entry in the active
// panel matching a user-entered wildcard mask (default "*", i.e. everything,
// matching PathUtil.h's WildcardMatch semantics â€” unlike legacy DOS masking,
// "*" rather than "*.*" is what matches every name including one without a
// dot). Cancelling the prompt (Esc) leaves the selection untouched.
void DoSelectByMask(Console& console, Panel& left, Panel& right, bool leftActive, bool select) {
    Panel& target = leftActive ? left : right;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    auto patternOpt = ShowTextPrompt(
        console, select ? kStrSelectByMaskPrompt : kStrDeselectByMaskPrompt, L"*", redraw);
    if (!patternOpt) return;

    target.SelectByMask(*patternOpt, select);
}

// SRC-001/SRC-002: recursively finds entries under the active panel's
// directory matching a name/wildcard, then shows them in a full-screen
// picker. Enter jumps the active panel straight to the chosen entry
// ("navigable ... in a panel"); F3 opens a file result directly in the
// built-in viewer ("openable ... in a viewer") without leaving the picker.
void DoSearch(Console& console, Panel& left, Panel& right, bool leftActive) {
    Panel& target = leftActive ? left : right;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    // SRC-004/005: one form for name and (optional) content search. With
    // "Containing text" empty it is SRC-001's name search, as before.
    // Remembered for the session only.
    struct FindState {
        std::wstring mask = L"*";
        std::wstring text;
        bool regex = false;
        bool matchCase = false;
        int encoding = 0;  // TextEncodingChoice order: Auto, UTF-8, ANSI, UTF-16LE, UTF-16BE
        bool skipBinary = true;
    };
    static FindState state;

    mc::SearchOptions options;
    while (true) {
        std::vector<mc::FormField> fields(6);
        fields[0].kind = mc::FormFieldKind::Text;
        fields[0].label = L"File name mask";
        fields[0].text = state.mask;
        fields[0].textCursor = state.mask.size();
        fields[1].kind = mc::FormFieldKind::Text;
        fields[1].label = L"Containing text";
        fields[1].text = state.text;
        fields[1].textCursor = state.text.size();
        fields[2].kind = mc::FormFieldKind::Checkbox;
        fields[2].label = L"Regular expression";
        fields[2].boolValue = state.regex;
        fields[3].kind = mc::FormFieldKind::Checkbox;
        fields[3].label = L"Match case";
        fields[3].boolValue = state.matchCase;
        fields[4].kind = mc::FormFieldKind::Choice;
        fields[4].label = L"Encoding";
        fields[4].choices = {L"Auto", L"UTF-8", L"ANSI", L"UTF-16LE", L"UTF-16BE"};
        fields[4].choiceIndex = state.encoding;
        fields[5].kind = mc::FormFieldKind::Checkbox;
        fields[5].label = L"Skip binary files";
        fields[5].boolValue = state.skipBinary;
        if (!mc::ShowFormDialog(console, kStrFindFilesTitle, fields, redraw)) return;
        state = {fields[0].text, fields[1].text, fields[2].boolValue, fields[3].boolValue, fields[4].choiceIndex,
                 fields[5].boolValue};

        options = mc::SearchOptions{};
        options.nameMask = state.mask.empty() && !state.text.empty() ? L"*" : state.mask;
        if (options.nameMask.empty()) return;
        options.encoding = static_cast<mc::TextEncodingChoice>(state.encoding);
        options.skipBinary = state.skipBinary;
        if (!state.text.empty()) {
            std::wstring error;
            options.content = mc::ContentMatcher::Create({state.text, state.regex, state.matchCase}, error);
            if (!options.content) {
                ShowMessage(console, kStrFindFilesTitle, {error}, redraw);
                continue;  // back to the form, values kept
            }
        }
        break;
    }
    const bool contentSearch = options.content.has_value();
    const std::wstring shownTerm = contentSearch ? state.text : options.nameMask;

    console.Clear(kAttrNormal);
    console.PutText(0, 0, PadOrTrim(FormatSearchingHeader(target.Path().wstring(), shownTerm), console.Width()),
                    kAttrHeaderActive);
    console.Present();

    auto cancelPoll = [&]() { return PeekEscapePressed(console); };
    // SRC-004: a live progress line, at most ~10 updates a second.
    auto lastProgress = std::chrono::steady_clock::time_point{};
    auto onProgress = [&](size_t files, const std::filesystem::path& current) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastProgress < std::chrono::milliseconds(100)) return;
        lastProgress = now;
        console.PutText(0, 0,
                        PadOrTrim(FormatContentSearchProgress(target.Path().wstring(), shownTerm, files, current.wstring()),
                                  console.Width()),
                        kAttrHeaderActive);
        console.Present();
    };
    SearchOutcome outcome = SearchFiles(target.Path(), options, cancelPoll, onProgress);

    if (outcome.results.empty()) {
        std::wstring why = outcome.cancelled ? kStrSearchCancelled : (contentSearch ? kStrNoFilesContain : kStrNoMatchesFound);
        std::vector<std::wstring> lines{why};
        if (contentSearch && (outcome.unreadable > 0 || outcome.binarySkipped > 0))
            lines.push_back(FormatContentResultsHeader(0, false, shownTerm, outcome.unreadable, outcome.binarySkipped));
        ShowMessage(console, kStrFindFilesTitle, lines, redraw);
        return;
    }

    int cursor = 0;
    int topIndex = 0;
    bool running = true;
    while (running) {
        int rows = std::max<int>(1, console.Height() - 2);
        cursor = std::clamp(cursor, 0, static_cast<int>(outcome.results.size()) - 1);
        if (cursor < topIndex) topIndex = cursor;
        if (cursor >= topIndex + rows) topIndex = cursor - rows + 1;
        topIndex = std::max(0, topIndex);

        console.Clear(kAttrNormal);
        std::wstring header =
            contentSearch ? FormatContentResultsHeader(outcome.results.size(), outcome.truncated, shownTerm,
                                                       outcome.unreadable, outcome.binarySkipped)
                          : FormatSearchResultsHeader(outcome.results.size(), outcome.truncated, shownTerm);
        console.PutText(0, 0, PadOrTrim(header, console.Width()), kAttrHeaderActive);
        for (int row = 0; row < rows; ++row) {
            int index = topIndex + row;
            SHORT y = static_cast<SHORT>(1 + row);
            if (index >= static_cast<int>(outcome.results.size())) {
                console.PutText(0, y, std::wstring(console.Width(), L' '), kAttrNormal);
                continue;
            }
            const SearchResult& r = outcome.results[index];
            WORD attr = r.isDirectory ? kAttrDir : kAttrNormal;
            if (index == cursor) attr = static_cast<WORD>((attr & kColorMask) | FOREGROUND_INTENSITY | BACKGROUND_BLUE);
            // ACC-001: a directory result was previously distinguished from a
            // file result by kAttrDir's color alone â€” a trailing "\" (the
            // ordinary Windows convention for a directory path) makes that
            // status readable from the text itself.
            std::wstring resultLine = r.fullPath.wstring() + (r.isDirectory ? L"\\" : L"");
            if (r.lineNumber > 0) {
                // SRC-004: "path  :12:  matching line" (leading whitespace trimmed, tabs as spaces).
                std::wstring text = r.lineText;
                std::replace(text.begin(), text.end(), L'\t', L' ');
                size_t first = text.find_first_not_of(L' ');
                text = first == std::wstring::npos ? L"" : text.substr(first);
                resultLine += L"  :" + std::to_wstring(r.lineNumber) + L":  " + text;
            }
            console.PutText(0, y, PadOrTrim(resultLine, console.Width()), attr);
        }
        console.PutText(0, static_cast<SHORT>(console.Height() - 1),
                        PadOrTrim(kStrSearchResultsHint, console.Width()), kAttrStatusBar);
        console.Present();

        INPUT_RECORD record;
        DWORD read = 0;
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;
        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            console.UpdateSize();
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

        switch (record.Event.KeyEvent.wVirtualKeyCode) {
            case VK_UP:
                --cursor;
                break;
            case VK_DOWN:
                ++cursor;
                break;
            case VK_PRIOR:
                cursor -= rows;
                break;
            case VK_NEXT:
                cursor += rows;
                break;
            case VK_HOME:
                cursor = 0;
                break;
            case VK_END:
                cursor = static_cast<int>(outcome.results.size()) - 1;
                break;
            case VK_F3:
                if (!outcome.results[cursor].isDirectory) {
                    // SRC-004: open at the matching line with the term ready for 'n'. The viewer's
                    // search is plain text, so a regex isn't passed on -- nor a term starting with
                    // '#', which the viewer would read as a hex byte search.
                    mc::ViewerStart start;
                    start.line = outcome.results[cursor].lineNumber;
                    if (contentSearch && !state.regex && !shownTerm.empty() && shownTerm[0] != L'#')
                        start.searchTerm = shownTerm;
                    ShowTextFileViewer(console, outcome.results[cursor].fullPath, start);
                }
                break;
            case VK_RETURN: {
                const SearchResult& r = outcome.results[cursor];
                std::filesystem::path dir = r.isDirectory ? r.fullPath : r.fullPath.parent_path();
                std::wstring selectName = r.isDirectory ? L"" : r.fullPath.filename().wstring();
                target.NavigateTo(dir, selectName);
                running = false;
                break;
            }
            case VK_ESCAPE:
            case VK_F10:
                running = false;
                break;
            default:
                break;
        }
    }
}

// SRC-006: a short ASCII status column for the compare results list.
const wchar_t* CompareStatusGlyph(mc::CompareStatus s) {
    switch (s) {
        case mc::CompareStatus::Identical: return L"=";
        case mc::CompareStatus::LeftOnly: return L"->";
        case mc::CompareStatus::RightOnly: return L"<-";
        case mc::CompareStatus::LeftNewer: return L">";
        case mc::CompareStatus::RightNewer: return L"<";
        case mc::CompareStatus::Different: return L"!=";
        case mc::CompareStatus::TypeMismatch: return L"!";
        case mc::CompareStatus::Unreadable: return L"?";
    }
    return L" ";
}

// Moves `panel`'s cursor onto the entry called `name` (case-insensitive)
// without reloading, so its selection marks survive.
void FocusPanelEntry(Panel& panel, const std::wstring& name, int visibleRows) {
    const auto& entries = panel.Entries();
    for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
        if (CompareStringOrdinal(entries[i].name.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL) {
            panel.MoveCursor(i - panel.Cursor(), visibleRows);
            return;
        }
    }
}

// SRC-006/007: one side's "size time" column for the compare and sync lists.
std::wstring CompareSideText(const std::optional<mc::CompareSide>& side, bool withTime) {
    std::wstring size = !side ? L"" : side->isDirectory ? L"<DIR>" : FormatHumanBytes(static_cast<double>(side->size));
    std::wstring text = std::wstring(10 - std::min<size_t>(10, size.size()), L' ') + size;  // ASCII only
    if (withTime) text += L" " + mc::PadToDisplayWidth(side ? FormatModifiedTime(side->lastWriteUtc) : L"", 16);
    return text;
}

// SRC-006/007: the refusals shared by compare and sync. True = refused.
bool RefuseCompareOfPanels(Console& console, const Panel& left, const Panel& right, bool leftActive,
                           const std::wstring& title) {
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    if (left.Content() != nullptr || right.Content() != nullptr) {
        ShowMessage(console, title, {kStrCompareNeedsFolders}, redraw);
        return true;
    }
    const Panel* const panels[] = {&left, &right};
    auto broken = std::find_if(std::begin(panels), std::end(panels), [](const Panel* p) { return p->LocationInaccessible(); });
    if (broken != std::end(panels)) {
        ShowMessage(console, title, {kStrCompareCannotList, (*broken)->Path().wstring()}, redraw);
        return true;
    }
    if (mc::PathsEquivalent(left.Path(), right.Path())) {
        ShowMessage(console, title, {kStrCompareSameFolder}, redraw);
        return true;
    }
    return false;
}

// SRC-006/007: runs the compare full-screen with the throttled progress line.
// SRC-008: shows the differences between two files -- through the user's
// Config::diffProgram when one is set (unless `forceBuiltin`, Shift+Alt+F3),
// otherwise the built-in DiffViewer. Binary files, or files too large to
// align line by line (over 64 MB or 200,000 lines), get a byte comparison
// instead: identical, or the first differing offset.
void ShowFileDifferences(Console& console, const std::filesystem::path& leftFile, const std::filesystem::path& rightFile,
                         const Config& config, bool forceBuiltin, const std::function<void()>& redraw) {
    if (!forceBuiltin && !config.diffProgram.empty()) {
        auto command = mc::BuildExternalDiffCommand(config.diffProgram, leftFile, rightFile);
        if (command) {
            DetachedProcessResult started = mc::RunDetachedCommandLine(command->program, command->arguments,
                                                                       leftFile.parent_path());
            if (started.started) {
                CloseHandle(started.processHandle);
                return;
            }
            ShowMessage(console, kStrDiffTitle, {L"Could not start the difference program:", command->program, started.error},
                        redraw);
        } else {
            ShowMessage(console, kStrDiffTitle, {L"The diffProgram setting has no program in it."}, redraw);
        }
        return;
    }

    constexpr uint64_t kMaxDiffBytes = 64ull * 1024 * 1024;
    constexpr size_t kMaxDiffLines = 200000;
    mc::DiffDocument document;
    document.leftPath = leftFile;
    document.rightPath = rightFile;
    bool binary = false, tooLarge = false;
    std::wstring readError;
    auto readLines = [&](const std::filesystem::path& file, std::vector<std::wstring>& out, std::wstring& encoding) {
        std::error_code ec;
        uint64_t size = std::filesystem::file_size(mc::ToLongPath(file), ec);
        if (ec) {
            readError = L"Cannot read " + file.wstring();
            return;
        }
        if (size > kMaxDiffBytes) {
            tooLarge = true;
            return;
        }
        bool ok = mc::ForEachTextLine(
            file, mc::TextEncodingChoice::Auto,
            [&](size_t, const std::wstring& line) {
                if (out.size() >= kMaxDiffLines) {
                    tooLarge = true;
                    return false;
                }
                out.push_back(line);
                return true;
            },
            [&](const mc::TextFileProbe& probe) {
                const auto& h = probe.head;
                encoding = probe.utf16Bom ? L"UTF-16"
                           : (h.size() >= 3 && h[0] == 0xEF && h[1] == 0xBB && h[2] == 0xBF) ? L"UTF-8 (BOM)"
                                                                                           : L"UTF-8/ANSI";
                if (mc::LooksBinary(h.data(), h.size(), probe.utf16Bom)) {
                    binary = true;
                    return false;
                }
                return true;
            });
        if (!ok) readError = L"Cannot read " + file.wstring();
    };
    readLines(leftFile, document.left, document.leftEncoding);
    if (readError.empty() && !binary && !tooLarge) readLines(rightFile, document.right, document.rightEncoding);
    if (!readError.empty()) {
        ShowMessage(console, kStrDiffTitle, {readError}, redraw);
        return;
    }

    if (binary || tooLarge) {
        console.Clear(kAttrNormal);
        console.PutText(0, 0, PadOrTrim(L"Comparing bytes... Esc to cancel", console.Width()), kAttrHeaderActive);
        console.Present();
        mc::ByteCompareResult bytes =
            mc::FirstDifferenceOffset(leftFile, rightFile, [&]() { return PeekEscapePressed(console); });
        if (bytes.cancelled) return;
        std::vector<std::wstring> lines{binary ? L"Binary files -- compared byte by byte."
                                               : L"Too large to compare line by line (over 64 MB or 200,000 lines) "
                                                 L"-- compared byte by byte."};
        if (!bytes.ok) {
            lines.push_back(bytes.error);
        } else if (bytes.identical) {
            lines.push_back(kStrDiffFilesIdentical);
        } else {
            wchar_t hex[32];
            swprintf_s(hex, L"0x%llX", static_cast<unsigned long long>(bytes.firstDifference));
            lines.push_back(L"They differ at byte " + std::to_wstring(bytes.firstDifference) + L" (" + hex + L").");
        }
        lines.push_back(L"Left: " + std::to_wstring(bytes.leftSize) + L" bytes, right: " +
                        std::to_wstring(bytes.rightSize) + L" bytes.");
        ShowMessage(console, kStrDiffTitle, lines, redraw);
        return;
    }

    // The line diff, with a progress line and Esc; also used by the viewer's I / W keys.
    auto compute = [&](const mc::DiffOptions& options) {
        console.PutText(0, 0, PadOrTrim(FormatDiffProgress(0, mc::kMaxDiffEdits), console.Width()), kAttrHeaderActive);
        console.Present();
        auto last = std::chrono::steady_clock::time_point{};
        return mc::ComputeLineDiff(
            document.left, document.right, options, [&]() { return PeekEscapePressed(console); },
            [&](size_t edits, size_t limit) {
                auto now = std::chrono::steady_clock::now();
                if (now - last < std::chrono::milliseconds(100)) return;
                last = now;
                console.PutText(0, 0, PadOrTrim(FormatDiffProgress(edits, limit), console.Width()), kAttrHeaderActive);
                console.Present();
            });
    };
    mc::DiffOptions options;
    mc::DiffResult result = compute(options);
    if (result.status == mc::DiffStatus::Cancelled) return;
    if (result.status == mc::DiffStatus::TooLarge) {
        ShowMessage(console, kStrDiffTitle, {kStrDiffTooDifferentLong}, redraw);
        return;
    }
    if (result.Identical()) {
        std::vector<std::wstring> lines{kStrDiffFilesIdentical};
        if (document.leftEncoding != document.rightEncoding)
            lines.push_back(L"(as text: " + document.leftEncoding + L" | " + document.rightEncoding + L")");
        ShowMessage(console, kStrDiffTitle, lines, redraw);
        return;
    }
    mc::ShowTextDiffViewer(console, document, options, std::move(result), compute);
}

// SRC-008 (File > Compare contents, Alt+F3): which two files, per
// TextDiff.h's ChooseFilesToCompare -- two selected in the active panel, one
// selected in each panel, else the active file and the same name (or the
// cursor file) in the other panel.
void DoCompareFiles(Console& console, Panel& left, Panel& right, bool leftActive, const Config& config,
                    bool forceBuiltin) {
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    if (RefuseCompareOfPanels(console, left, right, leftActive, kStrDiffTitle)) return;
    auto state = [](const Panel& panel) {
        mc::ComparePanelState s;
        s.folder = panel.Path();
        const auto& entries = panel.Entries();
        for (const auto& e : entries) {
            if (e.name == L"..") continue;
            if (!e.isDirectory) s.fileNames.push_back(e.name);
            if (!e.selected) continue;
            if (e.isDirectory) ++s.selectedFolders;
            else s.selectedFiles.push_back(e.name);
        }
        int cursor = panel.Cursor();
        if (cursor >= 0 && cursor < static_cast<int>(entries.size())) {
            const auto& e = entries[static_cast<size_t>(cursor)];
            if (!e.isDirectory && e.name != L"..") s.cursorFile = e.name;
        }
        return s;
    };
    mc::FilesToCompare files = mc::ChooseFilesToCompare(state(left), state(right), leftActive);
    if (!files.pair) {
        ShowMessage(console, kStrDiffTitle, {files.problem}, redraw);
        return;
    }
    ShowFileDifferences(console, files.pair->first, files.pair->second, config, forceBuiltin, redraw);
}

mc::CompareOutcome RunCompareWithProgress(Console& console, const Panel& left, const Panel& right,
                                          const mc::CompareOptions& options) {
    console.Clear(kAttrNormal);
    console.PutText(0, 0, PadOrTrim(FormatCompareProgress(0, FormatHumanBytes(0), L""), console.Width()),
                    kAttrHeaderActive);
    console.Present();
    auto cancelPoll = [&]() { return PeekEscapePressed(console); };
    auto lastProgress = std::chrono::steady_clock::time_point{};
    auto onProgress = [&](const mc::CompareProgress& progress) {
        auto now = std::chrono::steady_clock::now();
        if (now - lastProgress < std::chrono::milliseconds(100)) return;
        lastProgress = now;
        console.PutText(0, 0,
                        PadOrTrim(FormatCompareProgress(progress.itemsCompared,
                                                        FormatHumanBytes(static_cast<double>(progress.bytesRead)),
                                                        progress.current),
                                  console.Width()),
                        kAttrHeaderActive);
        console.Present();
    };
    return mc::CompareDirectories(left.Path(), right.Path(), options, cancelPoll, onProgress);
}

// SRC-007: what the compare list's F5 hands to the sync form.
struct SyncPreset {
    int compareMethod = 2;  // CompareMethod order
    bool recursive = true;
    int64_t tolerance = 2;
    bool ignoreOneHour = false;
};

// SRC-006 (Shift+F2, Selection > Compare Folders): compares the two panels'
// folders by name / size / timestamp / content, selects the differences in
// both panels the Total Commander way (only-here files and the newer side of
// each differing pair), then optionally shows the full results list.
// SRC-007: F5 in that list returns a preset for the sync form.
std::optional<SyncPreset> DoCompareDirectories(Console& console, Panel& left, Panel& right, bool leftActive,
                                               const Config& config) {
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    if (RefuseCompareOfPanels(console, left, right, leftActive, kStrCompareTitle)) return std::nullopt;

    // Remembered for the session only.
    struct CompareState {
        int method = 2;  // CompareMethod order: Name, Size, Timestamp, Content
        bool recursive = false;
        int64_t tolerance = 2;
        bool ignoreOneHour = false;
        bool showList = true;
    };
    static CompareState state;

    std::vector<mc::FormField> fields(5);
    fields[0].kind = mc::FormFieldKind::Choice;
    fields[0].label = L"Compare by";
    fields[0].choices = {L"Name", L"Name + size", L"Name + size + timestamp", L"Name + size + content"};
    fields[0].choiceIndex = state.method;
    fields[1].kind = mc::FormFieldKind::Checkbox;
    fields[1].label = L"Include subfolders";
    fields[1].boolValue = state.recursive;
    fields[2].kind = mc::FormFieldKind::Number;
    fields[2].label = L"Timestamp tolerance (seconds)";
    fields[2].numberValue = state.tolerance;
    fields[2].numberMin = 0;
    fields[2].numberMax = 3600;
    fields[3].kind = mc::FormFieldKind::Checkbox;
    fields[3].label = L"Ignore exact 1-hour difference";
    fields[3].boolValue = state.ignoreOneHour;
    fields[4].kind = mc::FormFieldKind::Checkbox;
    fields[4].label = L"Show the results list";
    fields[4].boolValue = state.showList;
    if (!mc::ShowFormDialog(console, kStrCompareTitle, fields, redraw)) return std::nullopt;
    state = {fields[0].choiceIndex, fields[1].boolValue, fields[2].numberValue, fields[3].boolValue,
             fields[4].boolValue};

    mc::CompareOptions options;
    options.method = static_cast<mc::CompareMethod>(state.method);
    options.recursive = state.recursive;
    options.toleranceSeconds = static_cast<uint32_t>(std::clamp<int64_t>(state.tolerance, 0, 3600));
    options.ignoreOneHour = state.ignoreOneHour;
    options.includeHiddenLeft = left.ShowingHiddenAndSystem();
    options.includeHiddenRight = right.ShowingHiddenAndSystem();
    options.fileMaskLeft = left.Filter();
    options.fileMaskRight = right.Filter();

    mc::CompareOutcome outcome = RunCompareWithProgress(console, left, right, options);

    if (!outcome.leftListed || !outcome.rightListed) {
        ShowMessage(console, kStrCompareTitle,
                    {kStrCompareCannotList, (outcome.leftListed ? right : left).Path().wstring()}, redraw);
        return std::nullopt;
    }

    // D4: replace both selections with the top-level differences.
    for (Panel* p : {&left, &right}) {
        while (p->IsLoading()) p->PumpRefresh(4096);
        p->ClearSelection();
    }
    auto markIn = [](Panel& panel, const std::wstring& name) {
        const auto& entries = panel.Entries();
        for (int i = 0; i < static_cast<int>(entries.size()); ++i) {
            if (CompareStringOrdinal(entries[i].name.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL) {
                panel.SetEntrySelected(i, true);
                return;
            }
        }
    };
    size_t different = 0, leftOnly = 0, rightOnly = 0, identical = 0;
    for (const mc::CompareItem& item : outcome.items) {
        switch (item.status) {
            case mc::CompareStatus::Identical: ++identical; break;
            case mc::CompareStatus::LeftOnly: ++leftOnly; break;
            case mc::CompareStatus::RightOnly: ++rightOnly; break;
            case mc::CompareStatus::Unreadable: break;  // counted by outcome.unreadable
            default: ++different; break;
        }
        if (item.depth != 0) continue;
        if (mc::MarksLeft(item.status) && item.left) markIn(left, item.name);
        if (mc::MarksRight(item.status) && item.right) markIn(right, item.name);
    }
    std::wstring summary = FormatCompareSummary(different, leftOnly, rightOnly, identical, outcome.unreadable,
                                                outcome.cancelled, outcome.truncated);

    if (!state.showList || outcome.items.empty()) {
        ShowMessage(console, kStrCompareTitle,
                    {outcome.cancelled && outcome.items.empty() ? std::wstring(kStrCompareCancelledNothing) : summary},
                    redraw);
        return std::nullopt;
    }

    // D5: the results list. Tab cycles the view filter.
    static const wchar_t* kViewNames[] = {L"all", L"differences", L"left only", L"right only"};
    auto inView = [](int view, mc::CompareStatus s) {
        switch (view) {
            case 1: return s != mc::CompareStatus::Identical;
            case 2: return s == mc::CompareStatus::LeftOnly;
            case 3: return s == mc::CompareStatus::RightOnly;
            default: return true;
        }
    };
    int view = (different + leftOnly + rightOnly + outcome.unreadable) > 0 ? 1 : 0;
    std::vector<size_t> shown;
    auto rebuild = [&]() {
        shown.clear();
        for (size_t i = 0; i < outcome.items.size(); ++i)
            if (inView(view, outcome.items[i].status)) shown.push_back(i);
    };
    rebuild();

    const std::wstring header = left.Path().wstring() + L"  <->  " + right.Path().wstring() + L"  \x2014 " +
                                fields[0].choices[static_cast<size_t>(state.method)] + L" \x2014 " + summary;
    auto sideText = CompareSideText;

    int cursor = 0;
    int topIndex = 0;
    bool running = true;
    while (running) {
        const int width = console.Width();
        int rows = std::max<int>(1, console.Height() - 3);
        int count = static_cast<int>(shown.size());
        cursor = std::clamp(cursor, 0, std::max(0, count - 1));
        if (cursor < topIndex) topIndex = cursor;
        if (cursor >= topIndex + rows) topIndex = cursor - rows + 1;
        topIndex = std::max(0, topIndex);

        // Columns: status(3) path | left | right. Times go first on a narrow screen.
        const bool withTime = width >= 100;
        const int sideWidth = withTime ? 27 : 10;
        const int pathWidth = std::max(10, width - 3 - 2 * (sideWidth + 3));

        console.Clear(kAttrNormal);
        console.PutText(0, 0, PadOrTrim(header, width), kAttrHeaderActive);
        console.PutText(0, 1,
                        PadOrTrim(L"Showing: " + std::wstring(kViewNames[view]) + L" (" + std::to_wstring(count) +
                                      L")",
                                  width),
                        kAttrStatusBar);
        for (int row = 0; row < rows; ++row) {
            int index = topIndex + row;
            SHORT y = static_cast<SHORT>(2 + row);
            if (index >= count) {
                console.PutText(0, y, std::wstring(width, L' '), kAttrNormal);
                continue;
            }
            const mc::CompareItem& item = outcome.items[shown[static_cast<size_t>(index)]];
            bool isDir = (item.left && item.left->isDirectory) || (item.right && item.right->isDirectory);
            std::wstring line = PadOrTrim(CompareStatusGlyph(item.status), 3) +
                                PadOrTrim(item.relativePath + (isDir ? L"\\" : L""), static_cast<size_t>(pathWidth)) +
                                L" | " + sideText(item.left, withTime) + L" | " + sideText(item.right, withTime);
            WORD attr = isDir ? kAttrDir : kAttrNormal;
            if (index == cursor) attr = static_cast<WORD>((attr & kColorMask) | FOREGROUND_INTENSITY | BACKGROUND_BLUE);
            console.PutText(0, y, PadOrTrim(line, width), attr);
        }
        console.PutText(0, static_cast<SHORT>(console.Height() - 1), PadOrTrim(kStrCompareResultsHint, width),
                        kAttrStatusBar);
        console.Present();

        INPUT_RECORD record;
        DWORD read = 0;
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;
        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            console.UpdateSize();
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;
        const bool shift = (record.Event.KeyEvent.dwControlKeyState & SHIFT_PRESSED) != 0;

        switch (record.Event.KeyEvent.wVirtualKeyCode) {
            case VK_UP: --cursor; break;
            case VK_DOWN: ++cursor; break;
            case VK_PRIOR: cursor -= rows; break;
            case VK_NEXT: cursor += rows; break;
            case VK_HOME: cursor = 0; break;
            case VK_END: cursor = count - 1; break;
            case VK_TAB:
                view = (view + 1) % 4;
                rebuild();
                cursor = 0;
                topIndex = 0;
                break;
            case VK_F3: {
                if (count == 0) break;
                const mc::CompareItem& item = outcome.items[shown[static_cast<size_t>(cursor)]];
                if (record.Event.KeyEvent.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) {
                    // SRC-008: the differences of a pair present on both sides.
                    if (item.left && item.right && !item.left->isDirectory && !item.right->isDirectory)
                        ShowFileDifferences(console, left.Path() / item.relativePath, right.Path() / item.relativePath,
                                            config, shift, [] {});
                    break;
                }
                const auto& side = shift ? item.right : item.left;
                if (side && !side->isDirectory)
                    ShowTextFileViewer(console, (shift ? right : left).Path() / item.relativePath);
                break;
            }
            case VK_RETURN: {
                if (count == 0) break;
                const mc::CompareItem& item = outcome.items[shown[static_cast<size_t>(cursor)]];
                if (item.depth == 0) {
                    // Stay put so the marks survive; just move both cursors onto the item.
                    int visible = std::max(1, console.Height() - 8);
                    FocusPanelEntry(left, item.name, visible);
                    FocusPanelEntry(right, item.name, visible);
                } else {
                    std::filesystem::path parent = std::filesystem::path(item.relativePath).parent_path();
                    for (Panel* p : {&left, &right}) {
                        std::filesystem::path dir = p->Path() / parent;
                        std::error_code ec;
                        if (std::filesystem::is_directory(mc::ToLongPath(dir), ec)) p->NavigateTo(dir, item.name);
                    }
                }
                running = false;
                break;
            }
            case VK_F5:  // SRC-007: synchronize, pre-filled from this compare
                return SyncPreset{state.method, state.recursive, state.tolerance, state.ignoreOneHour};
            case VK_ESCAPE:
            case VK_F10:
                running = false;
                break;
            default:
                break;
        }
    }
    return std::nullopt;
}

// NAV-004: one Windows logical drive's identity and, where queryable,
// its free/total space â€” a removed removable drive or an unreachable
// mapped network share simply reports no space rather than failing the
// whole enumeration.
struct DriveInfo {
    std::wstring root;  // e.g. "C:\\"
    std::wstring label;  // volume label, empty if unavailable/unlabeled
    UINT type = DRIVE_UNKNOWN;
    uint64_t freeBytes = 0;
    uint64_t totalBytes = 0;
    bool spaceKnown = false;
};

std::wstring DriveTypeLabel(UINT type) {
    switch (type) {
        case DRIVE_REMOVABLE:
            return kStrDriveTypeRemovable;
        case DRIVE_FIXED:
            return kStrDriveTypeLocalDisk;
        case DRIVE_REMOTE:
            return kStrDriveTypeNetwork;
        case DRIVE_CDROM:
            return kStrDriveTypeCdDvd;
        case DRIVE_RAMDISK:
            return kStrDriveTypeRamDisk;
        default:
            return kStrUnknown;
    }
}

// Enumerates every logical drive letter Windows currently exposes
// (GetLogicalDriveStringsW): fixed disks, removable media, mapped network
// shares, optical drives, and RAM disks alike (NAV-004).
std::vector<DriveInfo> EnumerateDrives() {
    std::vector<DriveInfo> drives;
    wchar_t buffer[1024];
    DWORD len = GetLogicalDriveStringsW(1023, buffer);
    if (len == 0 || len > 1023) return drives;

    for (const wchar_t* p = buffer; *p; p += wcslen(p) + 1) {
        DriveInfo info;
        info.root = p;
        info.type = GetDriveTypeW(p);

        wchar_t label[MAX_PATH + 1] = {};
        GetVolumeInformationW(p, label, MAX_PATH + 1, nullptr, nullptr, nullptr, nullptr, 0);
        info.label = label;

        ULARGE_INTEGER freeBytes{}, totalBytes{};
        if (GetDiskFreeSpaceExW(p, &freeBytes, &totalBytes, nullptr)) {
            info.freeBytes = freeBytes.QuadPart;
            info.totalBytes = totalBytes.QuadPart;
            info.spaceKnown = true;
        }
        drives.push_back(std::move(info));
    }
    return drives;
}

std::wstring FormatGigabytes(uint64_t bytes) {
    wchar_t buf[32];
    swprintf_s(buf, L"%.1f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    return buf;
}

std::wstring FormatDriveRow(const DriveInfo& info) {
    std::wstring row = PadOrTrim(info.root, 4) + L" " + PadOrTrim(DriveTypeLabel(info.type), 10);
    std::wstring label = info.label.empty() ? L"" : (L"[" + info.label + L"]");
    row += L" " + PadOrTrim(label, 22);
    if (info.spaceKnown) {
        row += L" " + FormatGigabytes(info.freeBytes) + L" free of " + FormatGigabytes(info.totalBytes);
    }
    return row;
}

// NAV-004: a full-screen drive picker, in the spirit of DoSearch's results
// picker. Alt+F1/Alt+F2 always target the left/right panel respectively
// (Total Commander's convention) rather than "whichever panel is active",
// since the point is being able to set up two different drives side by
// side without switching focus back and forth first.
void DoSelectDrive(Console& console, Panel& left, Panel& right, bool& leftActive, bool selectLeftPanel) {
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    std::vector<DriveInfo> drives = EnumerateDrives();
    if (drives.empty()) {
        ShowMessage(console, kStrSelectDriveTitle, {kStrNoDrivesFound}, redraw);
        return;
    }

    int cursor = 0;
    int topIndex = 0;
    bool running = true;
    while (running) {
        int rows = std::max<int>(1, console.Height() - 2);
        cursor = std::clamp(cursor, 0, static_cast<int>(drives.size()) - 1);
        if (cursor < topIndex) topIndex = cursor;
        if (cursor >= topIndex + rows) topIndex = cursor - rows + 1;
        topIndex = std::max(0, topIndex);

        console.Clear(kAttrNormal);
        std::wstring header = FormatSelectDriveForPanelHeader(selectLeftPanel);
        console.PutText(0, 0, PadOrTrim(header, console.Width()), kAttrHeaderActive);
        for (int row = 0; row < rows; ++row) {
            int index = topIndex + row;
            SHORT y = static_cast<SHORT>(1 + row);
            if (index >= static_cast<int>(drives.size())) {
                console.PutText(0, y, std::wstring(console.Width(), L' '), kAttrNormal);
                continue;
            }
            WORD attr = kAttrNormal;
            if (index == cursor) attr = static_cast<WORD>((attr & kColorMask) | FOREGROUND_INTENSITY | BACKGROUND_BLUE);
            console.PutText(0, y, PadOrTrim(FormatDriveRow(drives[index]), console.Width()), attr);
        }
        console.PutText(0, static_cast<SHORT>(console.Height() - 1),
                        PadOrTrim(kStrSelectDriveHint, console.Width()), kAttrStatusBar);
        console.Present();

        INPUT_RECORD record;
        DWORD read = 0;
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;
        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            console.UpdateSize();
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

        switch (record.Event.KeyEvent.wVirtualKeyCode) {
            case VK_UP:
                --cursor;
                break;
            case VK_DOWN:
                ++cursor;
                break;
            case VK_PRIOR:
                cursor -= rows;
                break;
            case VK_NEXT:
                cursor += rows;
                break;
            case VK_HOME:
                cursor = 0;
                break;
            case VK_END:
                cursor = static_cast<int>(drives.size()) - 1;
                break;
            case VK_RETURN: {
                Panel& target = selectLeftPanel ? left : right;
                target.NavigateTo(drives[cursor].root);
                leftActive = selectLeftPanel;
                running = false;
                break;
            }
            case VK_ESCAPE:
            case VK_F10:
                running = false;
                break;
            default:
                break;
        }
    }
}

// NAV-005/LIST-002/LIST-003: re-sorting the same key toggles direction
// (ascending -> descending -> ascending ...), matching the familiar
// click-the-same-column-header convention; choosing a different key starts
// it ascending.
void DoSort(Panel& target, SortKey key) {
    bool descending = (target.Sort() == key) && !target.SortDescending();
    target.SetSort(key, descending);
}

// IS-0015/NAV-012: keeps the *other* panel in sync when it happens to show
// the exact same directory as `target` -- most single-panel operations
// (rename, create, delete, attribute toggle, execute) only ever refresh
// `target` itself; without this, the other panel silently goes stale
// whenever both happen to point at the same place.
// FOP-014, Phase 3: background copy/move/queue. All of this runs on the UI
// thread; the worker only ever publishes data (see JobWorker.h).
const wchar_t* JobStateLabel(JobState state) {
    switch (state) {
        case JobState::Queued: return L"Queued";
        case JobState::Running: return L"Running";
        case JobState::WaitingForAnswer: return L"Waiting";
        case JobState::Done: return L"Done";
        case JobState::Cancelled: return L"Cancelled";
        case JobState::Failed: return L"Failed";
    }
    return L"";
}

bool IsActiveJob(JobState state) {
    return state == JobState::Queued || state == JobState::Running || state == JobState::WaitingForAnswer;
}

std::wstring FormatJobLine(const Job& job) {
    std::wstring verb = job.kind == JobKind::Move ? L"Move" : (job.kind == JobKind::Copy ? L"Copy" : L"Delete");
    std::wstring name = job.sources.empty() ? L"" : job.sources.front().filename().wstring();
    if (job.sources.size() > 1) name += L" +" + std::to_wstring(job.sources.size() - 1);
    // A delete has no destination: say where the items go instead of "-> ".
    std::wstring where = job.kind == JobKind::Delete ? (job.permanent ? L"(permanent)" : L"(Recycle Bin)")
                                                     : L"-> " + job.destination.wstring();
    return std::wstring(JobStateLabel(job.state)) + L"  " + verb + L"  " + name + L" " + where + L"  " +
           std::to_wstring(job.done) + L"/" + std::to_wstring(job.total);
}

// FOP-014 completion: the running JobWorker (set once in wmain), so the
// synchronous file operations below can check for a background job using
// the same files without threading the worker through every signature --
// the gVisibleColumns / gFileActions module-state precedent.
mc::JobWorker* gJobWorker = nullptr;
// EXT-003: lets F3 / Shift+F3 / Assign File to App reach viewer plugins
// without threading the manager through every file-action helper.
PluginManager* gPluginManager = nullptr;

std::vector<std::filesystem::path> ItemPaths(const std::filesystem::path& folder, const std::vector<std::wstring>& names) {
    std::vector<std::filesystem::path> paths(names.size());
    std::transform(names.begin(), names.end(), paths.begin(), [&](const std::wstring& name) { return folder / name; });
    return paths;
}

// Refuses (with a message naming the job) a synchronous operation on files an
// active background job is using, rather than letting the two race. True if
// refused. Navigation, viewing and search never call this.
bool RefuseIfBusyWithJob(Console& console, const Panel& left, const Panel& right, bool leftActive,
                         const std::vector<std::filesystem::path>& paths) {
    if (!gJobWorker) return false;
    auto job = gJobWorker->ActiveJobTouching(paths);
    if (!job) return false;
    ShowMessage(console, kStrJobBusyTitle, {kStrJobBusyIntro, FormatJobLine(*job), kStrJobBusyAdvice},
                [&]() { DrawFrame(console, left, right, leftActive); });
    return true;
}

// SRC-007: the preview's action column.
const wchar_t* SyncActionGlyph(const mc::SyncItem& e) {
    switch (e.action) {
        case mc::SyncAction::CopyToRight: return mc::Overwrites(e) ? L"=>" : L"->";
        case mc::SyncAction::CopyToLeft: return mc::Overwrites(e) ? L"<=" : L"<-";
        case mc::SyncAction::DeleteRight: return L"DEL>";
        case mc::SyncAction::DeleteLeft: return L"<DEL";
        case mc::SyncAction::None: return e.conflict ? L"??" : L"";
    }
    return L"";
}

// SRC-007: copies first, then deletions (GroupForExecution's order), each item
// re-checked against what the preview showed so nothing that changed since is
// overwritten or deleted. Overwrites the user confirmed are answered without
// asking; any other name collision gets the usual prompt.
void ExecuteSyncPlan(Console& console, Panel& left, Panel& right, bool leftActive, Logger& logger,
                     const std::vector<mc::SyncItem>& plan) {
    ProgressUi ui{console, left, right, leftActive, kStrVerbSyncing};
    auto onItemStart = [&](const std::wstring& n, int done, int total) { return ui.OnItemStart(n, done, total); };
    auto cancelPoll = [&]() { return ui.PollCancel(); };
    auto onError = [&](const std::wstring& n, const std::wstring& msg, DWORD win32Error) {
        return AskFileError(console, left, right, leftActive, n, msg, win32Error);
    };
    auto onByteProgress = [&](uint64_t transferred, uint64_t total) { ui.OnByteProgress(transferred, total); };
    auto onElevateCopy = [&](const std::filesystem::path& src, const std::filesystem::path& dst, std::wstring& err) {
        logger.Log(LogLevel::Warning, L"Elevating copy: \"" + src.wstring() + L"\" -> \"" + dst.wstring() + L"\"");
        DWORD result = RunElevated(ElevatedRequest{ElevatedAction::Copy, src, dst});
        if (result == ERROR_SUCCESS) return true;
        err = FormatWinError(result);
        logger.Log(LogLevel::Error, L"Elevated copy failed: \"" + src.wstring() + L"\": " + err);
        return false;
    };
    auto sameText = [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_EQUAL;
    };

    OperationOutcome total;
    for (const mc::SyncGroup& group : mc::GroupForExecution(plan)) {
        if (total.cancelled) break;
        const bool isDelete = group.action == mc::SyncAction::DeleteRight || group.action == mc::SyncAction::DeleteLeft;
        const bool towardRight = group.action == mc::SyncAction::CopyToRight || group.action == mc::SyncAction::DeleteRight;
        const std::filesystem::path leftDir = left.Path() / group.relativeDir;
        const std::filesystem::path rightDir = right.Path() / group.relativeDir;

        std::vector<std::wstring> names, overwriteNames;
        for (size_t index : group.planIndices) {
            const mc::SyncItem& e = plan[index];
            auto nowLeft = mc::ReadSide(leftDir / e.item.name);
            auto nowRight = mc::ReadSide(rightDir / e.item.name);
            bool stale = isDelete ? (towardRight ? mc::SideChanged(e.item.right, nowRight)
                                                 : mc::SideChanged(e.item.left, nowLeft))
                                  : (mc::SideChanged(e.item.left, nowLeft) || mc::SideChanged(e.item.right, nowRight));
            if (stale) {
                total.failed.push_back({e.item.relativePath, kStrSyncChangedSincePreview});
                continue;
            }
            names.push_back(e.item.name);
            if (mc::Overwrites(e)) overwriteNames.push_back(e.item.name);
        }
        if (names.empty()) continue;

        OperationOutcome o;
        if (isDelete) {
            const std::filesystem::path& dir = towardRight ? rightDir : leftDir;
            ui.primaryPath = dir.wstring();
            ui.destPath.clear();
            o = DeleteItems(dir, names, /*permanent=*/false, onItemStart, cancelPoll, onError);
        } else {
            const std::filesystem::path& src = towardRight ? leftDir : rightDir;
            const std::filesystem::path& dst = towardRight ? rightDir : leftDir;
            ui.primaryPath = src.wstring();
            ui.destPath = dst.wstring();
            auto onConflict = [&](const std::filesystem::path& d) {
                if (sameText(d.parent_path().wstring(), dst.wstring()) &&
                    std::any_of(overwriteNames.begin(), overwriteNames.end(),
                                [&](const std::wstring& n) { return sameText(n, d.filename().wstring()); })) {
                    ConflictAskResult confirmed;
                    confirmed.choice = ConflictChoice::Overwrite;
                    return confirmed;
                }
                return AskConflict(console, left, right, leftActive, d);
            };
            o = CopyItems(src, names, dst, onConflict, onItemStart, cancelPoll, onError, onByteProgress, onElevateCopy);
        }
        total.succeeded += o.succeeded;
        total.skipped += o.skipped;
        total.cancelled = total.cancelled || o.cancelled;
        for (auto& f : o.failed) {
            std::filesystem::path rel = group.relativeDir / f.first;
            total.failed.push_back({rel.wstring(), f.second});
        }
    }

    left.Refresh();
    right.Refresh();
    ReportOutcome(console, left, right, leftActive, kStrVerbSync, total, logger);
}

// SRC-007 (Ctrl+Shift+F2, Commands > Synchronize Folders, F5 in the compare
// list): makes the two panels' folders match -- one way or both ways --
// only after a preview the user can adjust and a mandatory confirmation
// (never skippable by config: the requirement itself demands it).
void DoSynchronizeDirectories(Console& console, Panel& left, Panel& right, bool leftActive, Logger& logger,
                              const std::optional<SyncPreset>& preset = std::nullopt) {
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    if (RefuseCompareOfPanels(console, left, right, leftActive, kStrSyncTitle)) return;
    if (mc::IsSameOrWithin(left.Path(), right.Path()) || mc::IsSameOrWithin(right.Path(), left.Path())) {
        ShowMessage(console, kStrSyncTitle, {kStrSyncInsideOther}, redraw);
        return;
    }
    if (RefuseIfBusyWithJob(console, left, right, leftActive, {left.Path(), right.Path()})) return;  // FOP-014

    // Remembered for the session only.
    struct SyncState {
        int direction = 0;  // SyncDirection order: LeftToRight, RightToLeft, Both
        int method = 0;     // 0 = timestamp, 1 = content
        bool recursive = true;
        bool deleteExtras = false;
        int64_t tolerance = 2;
        bool ignoreOneHour = false;
    };
    static SyncState state;
    if (preset) {
        state.method = preset->compareMethod == static_cast<int>(mc::CompareMethod::Content) ? 1 : 0;
        state.recursive = preset->recursive;
        state.tolerance = preset->tolerance;
        state.ignoreOneHour = preset->ignoreOneHour;
    }

    std::vector<mc::FormField> fields(6);
    fields[0].kind = mc::FormFieldKind::Choice;
    fields[0].label = L"Direction";
    fields[0].choices = {L"Left -> Right", L"Right -> Left", L"Both ways"};
    fields[0].choiceIndex = state.direction;
    fields[1].kind = mc::FormFieldKind::Choice;
    fields[1].label = L"Compare by";
    fields[1].choices = {L"Name + size + timestamp", L"Name + size + content"};
    fields[1].choiceIndex = state.method;
    fields[2].kind = mc::FormFieldKind::Checkbox;
    fields[2].label = L"Include subfolders";
    fields[2].boolValue = state.recursive;
    fields[3].kind = mc::FormFieldKind::Checkbox;
    fields[3].label = L"Delete files only on the target (one-way, Recycle Bin)";
    fields[3].boolValue = state.deleteExtras;
    fields[4].kind = mc::FormFieldKind::Number;
    fields[4].label = L"Timestamp tolerance (seconds)";
    fields[4].numberValue = state.tolerance;
    fields[4].numberMin = 0;
    fields[4].numberMax = 3600;
    fields[5].kind = mc::FormFieldKind::Checkbox;
    fields[5].label = L"Ignore exact 1-hour difference";
    fields[5].boolValue = state.ignoreOneHour;
    if (!mc::ShowFormDialog(console, kStrSyncTitle, fields, redraw)) return;
    state = {fields[0].choiceIndex, fields[1].choiceIndex, fields[2].boolValue, fields[3].boolValue,
             fields[4].numberValue, fields[5].boolValue};

    mc::SyncOptions syncOptions;
    syncOptions.direction = static_cast<mc::SyncDirection>(state.direction);
    syncOptions.deleteExtras = state.deleteExtras && syncOptions.direction != mc::SyncDirection::Both;

    mc::CompareOptions options;
    options.method = state.method == 1 ? mc::CompareMethod::Content : mc::CompareMethod::Timestamp;
    options.recursive = state.recursive;
    options.toleranceSeconds = static_cast<uint32_t>(std::clamp<int64_t>(state.tolerance, 0, 3600));
    options.ignoreOneHour = state.ignoreOneHour;
    options.includeHiddenLeft = left.ShowingHiddenAndSystem();
    options.includeHiddenRight = right.ShowingHiddenAndSystem();
    options.fileMaskLeft = left.Filter();
    options.fileMaskRight = right.Filter();

    mc::CompareOutcome outcome = RunCompareWithProgress(console, left, right, options);
    if (!outcome.leftListed || !outcome.rightListed) {
        ShowMessage(console, kStrSyncTitle,
                    {kStrCompareCannotList, (outcome.leftListed ? right : left).Path().wstring()}, redraw);
        return;
    }
    if (outcome.cancelled) {
        ShowMessage(console, kStrSyncTitle, {kStrSyncCancelled}, redraw);
        return;
    }
    if (outcome.truncated) {  // a partial plan could skip or delete things silently
        ShowMessage(console, kStrSyncTitle, {kStrSyncTruncated}, redraw);
        return;
    }

    std::vector<mc::SyncItem> plan = mc::BuildSyncPlan(outcome, syncOptions);
    {
        mc::SyncSummary s = mc::Summarize(plan);
        if (s.Empty() && s.conflicts == 0) {
            ShowMessage(console, kStrSyncTitle, {kStrSyncNothing}, redraw);
            return;
        }
    }

    // D3: the preview. View 0 = every difference -- including items the
    // rules leave alone (e.g. right-only in Left -> Right), so `<-` can
    // still be chosen for them -- 1 = conflicts, 2 = all.
    static const wchar_t* kViewNames[] = {L"differences", L"conflicts", L"all"};
    int view = 0;
    std::vector<size_t> shown;
    auto rebuild = [&]() {
        shown.clear();
        for (size_t i = 0; i < plan.size(); ++i) {
            const mc::SyncItem& e = plan[i];
            bool differs = e.item.status != mc::CompareStatus::Identical || e.action != mc::SyncAction::None;
            bool include = view == 2 || (view == 1 ? e.conflict : differs);
            if (include) shown.push_back(i);
        }
    };
    rebuild();
    const wchar_t* arrow = syncOptions.direction == mc::SyncDirection::LeftToRight   ? L"  ->  "
                           : syncOptions.direction == mc::SyncDirection::RightToLeft ? L"  <-  "
                                                                                    : L"  <->  ";
    const std::wstring header = left.Path().wstring() + arrow + right.Path().wstring() + L"  \x2014 " +
                                fields[0].choices[static_cast<size_t>(state.direction)] + L", " +
                                fields[1].choices[static_cast<size_t>(state.method)];
    const mc::SyncAction deleteAction = syncOptions.direction == mc::SyncDirection::RightToLeft
                                            ? mc::SyncAction::DeleteLeft
                                            : mc::SyncAction::DeleteRight;

    int cursor = 0;
    int topIndex = 0;
    while (true) {
        const int width = console.Width();
        int rows = std::max<int>(1, console.Height() - 3);
        int count = static_cast<int>(shown.size());
        cursor = std::clamp(cursor, 0, std::max(0, count - 1));
        if (cursor < topIndex) topIndex = cursor;
        if (cursor >= topIndex + rows) topIndex = cursor - rows + 1;
        topIndex = std::max(0, topIndex);

        const bool withTime = width >= 110;
        const int sideWidth = withTime ? 27 : 10;
        const int pathWidth = std::max(10, width - 8 - 2 * (sideWidth + 3));
        mc::SyncSummary s = mc::Summarize(plan);

        console.Clear(kAttrNormal);
        console.PutText(0, 0, PadOrTrim(header, width), kAttrHeaderActive);
        console.PutText(0, 1,
                        PadOrTrim(FormatSyncTotals(s.copyRight, FormatHumanBytes(static_cast<double>(s.bytesRight)),
                                                   s.copyLeft, FormatHumanBytes(static_cast<double>(s.bytesLeft)),
                                                   s.overwrite, s.deleteRight, s.deleteLeft, s.conflicts) +
                                      L"  [" + kViewNames[view] + L"]",
                                  width),
                        kAttrStatusBar);
        for (int row = 0; row < rows; ++row) {
            int index = topIndex + row;
            SHORT y = static_cast<SHORT>(2 + row);
            if (index >= count) {
                console.PutText(0, y, std::wstring(width, L' '), kAttrNormal);
                continue;
            }
            const mc::SyncItem& e = plan[shown[static_cast<size_t>(index)]];
            bool isDir = (e.item.left && e.item.left->isDirectory) || (e.item.right && e.item.right->isDirectory);
            std::wstring path = e.item.relativePath + (isDir ? L"\\" : L"");
            if (!e.reason.empty()) path += L"  (" + e.reason + L")";
            std::wstring line = PadOrTrim(SyncActionGlyph(e), 5) + PadOrTrim(CompareStatusGlyph(e.item.status), 3) +
                                PadOrTrim(path, static_cast<size_t>(pathWidth)) + L" | " +
                                CompareSideText(e.item.left, withTime) + L" | " + CompareSideText(e.item.right, withTime);
            WORD attr = isDir ? kAttrDir : kAttrNormal;
            if (index == cursor) attr = static_cast<WORD>((attr & kColorMask) | FOREGROUND_INTENSITY | BACKGROUND_BLUE);
            console.PutText(0, y, PadOrTrim(line, width), attr);
        }
        console.PutText(0, static_cast<SHORT>(console.Height() - 1), PadOrTrim(kStrSyncPreviewHint, width),
                        kAttrStatusBar);
        console.Present();

        INPUT_RECORD record;
        DWORD read = 0;
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;
        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            console.UpdateSize();
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;
        const bool shift = (record.Event.KeyEvent.dwControlKeyState & SHIFT_PRESSED) != 0;
        mc::SyncItem* current = count > 0 ? &plan[shown[static_cast<size_t>(cursor)]] : nullptr;
        auto setAction = [&](mc::SyncAction a) {
            if (current && mc::CanApply(*current, a, syncOptions)) current->action = a;
        };

        switch (record.Event.KeyEvent.wVirtualKeyCode) {
            case VK_UP: --cursor; break;
            case VK_DOWN: ++cursor; break;
            case VK_PRIOR: cursor -= rows; break;
            case VK_NEXT: cursor += rows; break;
            case VK_HOME: cursor = 0; break;
            case VK_END: cursor = count - 1; break;
            case VK_SPACE:
                if (current) setAction(current->action != mc::SyncAction::None ? mc::SyncAction::None : current->proposed);
                ++cursor;
                break;
            case VK_RIGHT: setAction(mc::SyncAction::CopyToRight); break;
            case VK_LEFT: setAction(mc::SyncAction::CopyToLeft); break;
            case VK_DELETE: setAction(deleteAction); break;
            case VK_TAB:
                view = (view + 1) % 3;
                rebuild();
                cursor = 0;
                topIndex = 0;
                break;
            case VK_F3:
                if (current) {
                    const auto& side = shift ? current->item.right : current->item.left;
                    if (side && !side->isDirectory)
                        ShowTextFileViewer(console, (shift ? right : left).Path() / current->item.relativePath);
                }
                break;
            case VK_F5: {
                if (s.Empty()) {
                    ShowMessage(console, kStrSyncTitle, {kStrSyncNothingSelected}, redraw);
                    break;
                }
                // D4: the explicit confirmation -- Back is first (the default), always shown.
                std::vector<std::wstring> lines{left.Path().wstring() + arrow + right.Path().wstring()};
                if (s.copyRight > 0)
                    lines.push_back(L"Copy " + std::to_wstring(s.copyRight) + L" item(s) to the right (" +
                                    FormatHumanBytes(static_cast<double>(s.bytesRight)) + L")");
                if (s.copyLeft > 0)
                    lines.push_back(L"Copy " + std::to_wstring(s.copyLeft) + L" item(s) to the left (" +
                                    FormatHumanBytes(static_cast<double>(s.bytesLeft)) + L")");
                auto listSome = [&](const std::wstring& title, auto pick) {
                    std::vector<std::wstring> names;
                    for (const mc::SyncItem& e : plan)
                        if (pick(e)) names.push_back(e.item.relativePath);
                    if (names.empty()) return;
                    lines.push_back(title + L" " + std::to_wstring(names.size()) + L":");
                    for (size_t i = 0; i < names.size() && i < 5; ++i) lines.push_back(L"   " + names[i]);
                    if (names.size() > 5) lines.push_back(L"   ... and " + std::to_wstring(names.size() - 5) + L" more");
                };
                listSome(L"OVERWRITE", [](const mc::SyncItem& e) { return mc::Overwrites(e); });
                listSome(L"DELETE on the right (Recycle Bin)",
                         [](const mc::SyncItem& e) { return e.action == mc::SyncAction::DeleteRight; });
                listSome(L"DELETE on the left (Recycle Bin)",
                         [](const mc::SyncItem& e) { return e.action == mc::SyncAction::DeleteLeft; });
                int choice = ShowChoiceDialog(console, kStrSyncConfirmTitle, lines,
                                              {{L'B', kStrSyncBackLabel}, {L'S', kStrSyncGoLabel}, {L'C', kStrCancelLabel}},
                                              redraw);
                if (choice == 0) break;  // back to the preview
                if (choice == 1) ExecuteSyncPlan(console, left, right, leftActive, logger, plan);
                return;
            }
            case VK_ESCAPE:
            case VK_F10:
                return;
            default:
                break;
        }
    }
}

// Alt+F5 / Alt+F6: the same destination prompt as F5/F6, but the work goes to the
// background queue instead of blocking the screen.
void QueueCopyOrMove(Console& console, const Panel& left, const Panel& right, bool leftActive, bool isMove,
                     JobWorker& jobs) {
    const Panel& source = leftActive ? left : right;
    const Panel& dest = leftActive ? right : left;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    if (source.Content() || dest.Content()) {
        ShowMessage(console, kStrJobsTitle, {kStrQueuePluginRefused}, redraw);
        return;
    }
    std::vector<std::wstring> names = source.SelectionOrCursor();
    if (names.empty()) return;
    std::wstring verb = isMove ? kStrVerbMove : kStrVerbCopy;
    auto destOpt = ShowTextPrompt(console, FormatItemsToDestinationHeader(verb, names.size()),
                                  dest.Path().wstring(), redraw);
    if (!destOpt || destOpt->empty()) return;
    std::filesystem::path destination = *destOpt;
    std::error_code ec;
    if (!std::filesystem::is_directory(mc::ToLongPath(destination), ec) || ec) {
        ShowMessage(console, kStrJobsTitle, {kStrQueueBadDestination}, redraw);
        return;
    }
    Job job;
    job.kind = isMove ? JobKind::Move : JobKind::Copy;
    for (const auto& name : names) job.sources.push_back(source.Path() / name);
    job.destination = destination;
    jobs.Enqueue(std::move(job));
}

// Answers every outstanding prompt by showing the same dialog F5/F6 would have shown.
void ServiceJobPrompts(Console& console, const Panel& left, const Panel& right, bool leftActive, JobWorker& jobs) {
    for (const PendingPrompt& prompt : jobs.PendingPrompts()) {
        if (prompt.kind == PromptKind::Conflict) {
            jobs.AnswerConflict(prompt.seq, AskConflict(console, left, right, leftActive, prompt.path));
        } else {
            jobs.AnswerError(prompt.seq,
                             AskFileError(console, left, right, leftActive, prompt.path.wstring(), prompt.text, 0));
        }
    }
}

// What the main loop remembers between passes, so a job's finish is noticed once.
struct JobsWatch {
    std::map<uint64_t, JobState> seen;
    size_t active = 0;
    size_t queued = 0;
};

std::wstring FormatJobsTitle(size_t active, size_t queued) {
    wchar_t base[64];
    swprintf_s(base, L"MyCommander - %S", MC_VERSION_STR);
    std::wstring title = base;
    if (active > 0 || queued > 0) {
        title += L" - " + std::to_wstring(active) + L" running, " + std::to_wstring(queued) + L" queued";
    }
    return title;
}

// Refreshes only the panels a finished job touched, so the user's selections
// elsewhere survive, and keeps the window title in step with the queue.
void PollBackgroundJobs(Console& console, Panel& left, Panel& right, bool leftActive, const JobWorker& jobs,
                        JobsWatch& watch) {
    std::vector<Job> snapshot = jobs.Jobs();
    size_t active = 0;
    size_t queued = 0;
    for (const Job& job : snapshot) {
        if (job.state == JobState::Queued) ++queued;
        if (IsActiveJob(job.state)) ++active;
        auto seen = watch.seen.find(job.id);
        if (seen != watch.seen.end() && seen->second == job.state) continue;
        watch.seen[job.id] = job.state;
        if (IsActiveJob(job.state)) continue;
        // A job that ended with failed items reports them, the way a synchronous F5/F6 does.
        if (job.state == JobState::Failed) {
            std::vector<std::wstring> lines = job.failures;
            if (lines.empty()) lines.push_back(kStrJobFailedGeneric);
            if (lines.size() > 10) {
                lines.resize(10);
                lines.push_back(L"...");
            }
            ShowMessage(console, kStrJobsTitle, lines, [&]() { DrawFrame(console, left, right, leftActive); });
        }
        for (Panel* panel : {&left, &right}) {
            bool touched = PathsEquivalent(panel->Path(), job.destination);
            for (const auto& src : job.sources) {
                if (PathsEquivalent(panel->Path(), src.parent_path())) touched = true;
            }
            if (touched) panel->Refresh();
        }
    }
    if (active != watch.active || queued != watch.queued) {
        watch.active = active;
        watch.queued = queued;
        console.SetTitle(FormatJobsTitle(active, queued));
    }
}

void ShowJobQueue(Console& console, const Panel& left, const Panel& right, bool leftActive, JobWorker& jobs) {
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    std::vector<Job> snapshot = jobs.Jobs();
    if (snapshot.empty()) {
        ShowMessage(console, kStrJobsTitle, {kStrJobsEmpty}, redraw);
        return;
    }
    std::vector<std::wstring> items(snapshot.size());
    std::transform(snapshot.begin(), snapshot.end(), items.begin(), [](const Job& job) { return FormatJobLine(job); });
    int index = ShowListChoice(console, kStrJobsTitle, items, redraw, 0);
    if (index < 0) return;
    const Job& chosen = snapshot[static_cast<size_t>(index)];
    if (!IsActiveJob(chosen.state)) return;
    int answer = ShowChoiceDialog(console, kStrJobsTitle, {kStrJobsCancelQuestion},
                                  {DialogOption{L'Y', L"es"}, DialogOption{L'N', L"o"}}, redraw);
    if (answer == 0) jobs.Cancel(chosen.id);
}

// F10 with jobs still active: wait for them, cancel them, or stay. Returns true when the app may exit.
bool ConfirmQuitWithJobs(Console& console, Panel& left, Panel& right, bool leftActive, JobWorker& jobs,
                         JobsWatch& watch) {
    auto countActive = [&jobs]() {
        const std::vector<Job> all = jobs.Jobs();
        return static_cast<size_t>(std::count_if(all.begin(), all.end(), [](const Job& job) { return IsActiveJob(job.state); }));
    };
    size_t pending = countActive();
    if (pending == 0) return true;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    int choice = ShowChoiceDialog(console, kStrQuitJobsTitle, {FormatJobsPending(pending)},
                                  {DialogOption{L'W', L"ait until they finish, then quit"},
                                   DialogOption{L'C', L"ancel them and quit"}},
                                  redraw);
    if (choice == 1) {
        for (const Job& job : jobs.Jobs()) {
            if (IsActiveJob(job.state)) jobs.Cancel(job.id);
        }
        return true;
    }
    if (choice != 0) return false;
    // Wait: keep answering prompts until the queue is idle. Esc gives up
    // waiting, cancels what's left, and quits.
    while (countActive() > 0) {
        ServiceJobPrompts(console, left, right, leftActive, jobs);
        PollBackgroundJobs(console, left, right, leftActive, jobs, watch);
        if (PeekEscapePressed(console)) {
            for (const Job& job : jobs.Jobs()) {
                if (IsActiveJob(job.state)) jobs.Cancel(job.id);
            }
            return true;
        }
        Sleep(50);
    }
    return true;
}

void SyncOtherPanelIfSamePath(const Panel& target, Panel& other) {
    if (PathsEquivalent(target.Path(), other.Path())) other.Refresh();
}

void DoRename(Console& console, Panel& left, Panel& right, bool leftActive) {
    Panel& target = leftActive ? left : right;
    Panel& other = leftActive ? right : left;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    const auto& entries = target.Entries();
    int cursor = target.Cursor();
    if (cursor < 0 || cursor >= static_cast<int>(entries.size())) return;
    const auto& entry = entries[cursor];
    if (entry.name == L"..") return;
    if (RefuseIfBusyWithJob(console, left, right, leftActive, {target.Path() / entry.name})) return;  // FOP-014

    auto newNameOpt = ShowTextPrompt(console, kStrRenameTitle, entry.name, redraw);
    if (!newNameOpt || newNameOpt->empty() || *newNameOpt == entry.name) return;

    RenameResult result = RenameItem(target.Path(), entry.name, *newNameOpt);
    target.Refresh();
    SyncOtherPanelIfSamePath(target, other);
    if (!result.ok) {
        ShowMessage(console, kStrRenameFailedTitle, {result.error}, redraw);
    }
}

// FOP-017: Ctrl+M / File > Group Rename. Options form -> preview of every
// "old -> new" (problems marked, nothing touched yet) -> run. F2 in the
// preview goes back to the form; Esc anywhere cancels with nothing renamed.
// The options are remembered for the session only (never written to
// mycommander.ini -- ERR-006 spirit: no operation state on disk).
void DoGroupRename(Console& console, Panel& left, Panel& right, bool leftActive, Logger& logger) {
    Panel& target = leftActive ? left : right;
    Panel& other = leftActive ? right : left;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    if (target.Content()) {
        ShowMessage(console, kStrGroupRenameTitle, {kStrGroupRenamePluginRefused}, redraw);
        return;
    }
    std::vector<std::wstring> names = target.SelectionOrCursor();
    if (names.empty()) return;
    if (RefuseIfBusyWithJob(console, left, right, leftActive, ItemPaths(target.Path(), names))) return;

    std::vector<mc::RenameSource> sources;
    for (const auto& name : names) {
        mc::RenameSource source{name, {}};
        for (const auto& entry : target.Entries())
            if (entry.name == name) source.lastWrite = mc::LocalDateTimeFromFileTime(entry.lastWriteTimeUtc);
        sources.push_back(std::move(source));
    }
    // Every name really in the folder -- including ones the panel hides or
    // filters out -- so the preview catches every clash.
    std::vector<std::wstring> existing;
    {
        std::error_code ec;
        for (std::filesystem::directory_iterator it(mc::ToLongPath(target.Path()), ec), end; !ec && it != end;
             it.increment(ec))
            existing.push_back(it->path().filename().wstring());
    }
    std::wstring parentName = target.Path().filename().wstring();
    if (parentName.empty()) parentName = target.Path().wstring();

    static mc::MultiRenameOptions remembered;
    mc::MultiRenameOptions options = remembered;
    const std::wstring arrow = console.UseBasicSymbols() ? L"->" : L"\x2192";

    while (true) {
        std::vector<mc::FormField> fields(8);
        auto text = [&](size_t i, const wchar_t* label, const std::wstring& value) {
            fields[i].kind = mc::FormFieldKind::Text;
            fields[i].label = label;
            fields[i].text = value;
            fields[i].textCursor = value.size();
        };
        auto number = [&](size_t i, const wchar_t* label, int64_t value, int64_t min, int64_t max) {
            fields[i].kind = mc::FormFieldKind::Number;
            fields[i].label = label;
            fields[i].numberValue = value;
            fields[i].numberMin = min;
            fields[i].numberMax = max;
        };
        text(0, L"Name mask", options.nameMask);
        text(1, L"Extension mask", options.extMask);
        text(2, L"Search for", options.search);
        text(3, L"Replace with", options.replace);
        fields[4].kind = mc::FormFieldKind::Choice;
        fields[4].label = L"Case";
        fields[4].choices = {L"unchanged", L"lower case", L"UPPER CASE", L"First letter capital", L"Each Word Capital"};
        fields[4].choiceIndex = static_cast<int>(options.caseMode);
        number(5, L"Counter start", options.counterStart, 0, 999999999);
        number(6, L"Counter step", options.counterStep, 1, 999999);
        number(7, L"Counter digits", options.counterDigits, 1, 9);
        if (!mc::ShowFormDialog(console, kStrGroupRenameTitle, fields, redraw)) return;
        options.nameMask = fields[0].text;
        options.extMask = fields[1].text;
        options.search = fields[2].text;
        options.replace = fields[3].text;
        options.caseMode = static_cast<mc::RenameCase>(fields[4].choiceIndex);
        options.counterStart = fields[5].numberValue;
        options.counterStep = fields[6].numberValue;
        options.counterDigits = static_cast<int>(fields[7].numberValue);
        remembered = options;

        mc::RenamePlan plan = mc::BuildRenamePlan(options, sources, parentName, existing);
        size_t problems = 0, unchanged = 0, oldWidth = 0;
        for (const auto& item : plan.items) {
            if (item.status == mc::RenameStatus::Unchanged) ++unchanged;
            if (item.status == mc::RenameStatus::Invalid || item.status == mc::RenameStatus::DuplicateTarget ||
                item.status == mc::RenameStatus::ClashesWithExisting)
                ++problems;
            oldWidth = std::max<size_t>(oldWidth, static_cast<size_t>(StringDisplayWidth(item.oldName)));
        }
        oldWidth = std::min<size_t>(oldWidth, 40);
        std::vector<std::wstring> rows;
        std::vector<std::wstring> problemLines;
        if (!plan.maskError.empty()) {
            rows.push_back(L"! " + plan.maskError);
            problemLines.push_back(plan.maskError);
        }
        for (const auto& item : plan.items) {
            bool bad = item.status == mc::RenameStatus::Invalid || item.status == mc::RenameStatus::DuplicateTarget ||
                       item.status == mc::RenameStatus::ClashesWithExisting;
            std::wstring row = (bad ? L"! " : L"  ") + PadOrTrim(item.oldName, oldWidth) + L"  " + arrow + L"  " +
                               (item.newName.empty() ? L"?" : item.newName);
            if (item.status == mc::RenameStatus::Unchanged) row += L"  " + std::wstring(kStrGroupRenameUnchanged);
            if (bad && plan.maskError.empty()) {
                row += L"  \x2014 " + item.message;
                problemLines.push_back(item.oldName + L": " + item.message);
            }
            rows.push_back(row);
        }
        std::wstring title = plan.HasProblems() ? FormatGroupRenameProblemsTitle(plan.maskError.empty() ? problems : 1)
                                                : FormatGroupRenameTitle(plan.items.size(), plan.RenameCount(), unchanged);

        while (true) {
            auto choice = mc::ShowListChoiceWithKeys(console, title, rows, redraw, 0, {VK_F2}, kStrGroupRenameHint);
            if (choice.index < 0) return;
            if (choice.key == VK_F2) break;  // back to the options form
            if (plan.HasProblems()) {
                std::vector<std::wstring> lines{kStrGroupRenameFixFirst};
                for (size_t i = 0; i < problemLines.size() && i < 5; ++i) lines.push_back(problemLines[i]);
                if (problemLines.size() > 5) lines.push_back(FormatMoreFailures(problemLines.size() - 5));
                ShowMessage(console, kStrGroupRenameTitle, lines, redraw);
                continue;
            }
            if (plan.RenameCount() == 0) {
                ShowMessage(console, kStrGroupRenameTitle, {kStrGroupRenameNothing}, redraw);
                return;
            }
            std::vector<std::filesystem::path> newPaths;
            std::wstring firstRenamed;
            for (const auto& item : plan.items) {
                if (item.status != mc::RenameStatus::Ok && item.status != mc::RenameStatus::CaseOnly) continue;
                newPaths.push_back(target.Path() / item.newName);
                if (firstRenamed.empty()) firstRenamed = item.newName;
            }
            if (RefuseIfBusyWithJob(console, left, right, leftActive, newPaths)) return;

            ProgressUi ui{console, left, right, leftActive, kStrVerbRenaming};
            ui.primaryPath = target.Path().wstring();
            auto onItemStart = [&](const std::wstring& n, int done, int total) { return ui.OnItemStart(n, done, total); };
            auto onError = [&](const std::wstring& n, const std::wstring& msg, DWORD win32Error) {
                return AskFileError(console, left, right, leftActive, n, msg, win32Error);
            };
            OperationOutcome outcome =
                mc::RenameItems(target.Path(), mc::OrderRenames(plan, existing), onItemStart, onError);
            // IS-0001 convention: the cursor lands on the first renamed item.
            target.NavigateTo(target.Path(), firstRenamed);
            SyncOtherPanelIfSamePath(target, other);
            ReportOutcome(console, left, right, leftActive, kStrVerbGroupRename, outcome, logger);
            return;
        }
    }
}

void DoMakeDirectory(Console& console, Panel& left, Panel& right, bool leftActive) {
    Panel& target = leftActive ? left : right;
    Panel& other = leftActive ? right : left;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    auto nameOpt = ShowTextPrompt(console, kStrNewFolderNamePrompt, L"", redraw);
    if (!nameOpt || nameOpt->empty()) return;
    if (RefuseIfBusyWithJob(console, left, right, leftActive, {target.Path() / *nameOpt})) return;  // FOP-014

    auto result = CreateNewDirectory(target.Path(), *nameOpt);
    // IS-0001: select the new directory once the refresh lands, not just
    // preserve wherever the cursor already was.
    target.NavigateTo(target.Path(), *nameOpt);
    SyncOtherPanelIfSamePath(target, other);
    if (!result.ok) {
        ShowMessage(console, kStrCreateFolderFailedTitle, {result.error}, redraw);
    }
}

// FOP-005: creates a new, empty file in the active panel's directory.
void DoCreateNewFile(Console& console, Panel& left, Panel& right, bool leftActive) {
    Panel& target = leftActive ? left : right;
    Panel& other = leftActive ? right : left;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    auto nameOpt = ShowTextPrompt(console, kStrNewFileNamePrompt, L"", redraw);
    if (!nameOpt || nameOpt->empty()) return;
    if (RefuseIfBusyWithJob(console, left, right, leftActive, {target.Path() / *nameOpt})) return;  // FOP-014

    auto result = CreateNewFile(target.Path(), *nameOpt);
    // IS-0001: select the new file once the refresh lands, not just
    // preserve wherever the cursor already was.
    target.NavigateTo(target.Path(), *nameOpt);
    SyncOtherPanelIfSamePath(target, other);
    if (!result.ok) {
        ShowMessage(console, kStrCreateFileFailedTitle, {result.error}, redraw);
    }
}

// Returns the cursor entry's full path, or nullopt if the cursor is on ".."
// or a directory (view/edit only ever act on a file).
std::optional<std::filesystem::path> CursorFilePath(const Panel& panel) {
    const auto& entries = panel.Entries();
    int cursor = panel.Cursor();
    if (cursor < 0 || cursor >= static_cast<int>(entries.size())) return std::nullopt;
    const auto& entry = entries[cursor];
    if (entry.name == L".." || entry.isDirectory) return std::nullopt;
    return panel.Path() / entry.name;
}

void DoView(Console& console, const Panel& left, const Panel& right, bool leftActive) {
    const Panel& target = leftActive ? left : right;
    auto filePath = CursorFilePath(target);
    if (!filePath) return;
    ShowTextFileViewer(console, *filePath);
}

// FEA-0003: Alt+Enter (also reachable from the top menu bar's View ->
// Executable properties, FEA-0005) â€” General fields (name/size/modified)
// always come straight from the panel's own already-loaded Entry rather
// than re-querying the filesystem; File info/Digital signature only for a
// "runnable" (PE) extension, and only once each is actually available â€”
// FileInfo.h's own functions degrade gracefully (empty/NotSigned) rather
// than ever failing loudly, so nothing here needs its own error handling.
// Reuses ShowMessage unchanged â€” a properties view is exactly an
// informational, any-key-or-click-dismissed box, so no new Dialog.cpp code
// is needed.
void DoShowFileProperties(Console& console, Panel& left, Panel& right, bool leftActive) {
    Panel& target = leftActive ? left : right;
    auto filePath = CursorFilePath(target);
    if (!filePath) return;
    const Entry& entry = target.Entries()[static_cast<size_t>(target.Cursor())];

    std::vector<std::wstring> lines;
    lines.push_back(FormatPropLine(L"File name", entry.name));
    lines.push_back(FormatPropLine(L"Size", FormatHumanBytes(static_cast<double>(entry.sizeBytes))));
    lines.push_back(FormatPropLine(L"Modified", FormatModifiedTime(entry.lastWriteTimeUtc)));

    if (IsRunnableExtension(filePath->extension().wstring())) {
        FileVersionFields fields = ReadFileVersionFields(*filePath);
        if (fields.available) {
            lines.push_back(L"");
            if (!fields.description.empty()) lines.push_back(FormatPropLine(L"Description", fields.description));
            if (!fields.fileVersion.empty()) lines.push_back(FormatPropLine(L"File version", fields.fileVersion));
            if (!fields.productName.empty()) lines.push_back(FormatPropLine(L"Product Name", fields.productName));
            if (!fields.productVersion.empty())
                lines.push_back(FormatPropLine(L"Product version", fields.productVersion));
            if (!fields.copyright.empty()) lines.push_back(FormatPropLine(L"Copyright", fields.copyright));
        }

        lines.push_back(L"");
        SignatureInfo sig = CheckDigitalSignature(*filePath);
        switch (sig.status) {
            case SignatureStatus::NotSigned:
                lines.push_back(kStrPropNotSigned);
                break;
            case SignatureStatus::Signed:
                lines.push_back(sig.signerName.empty() ? kStrPropSignedUnknownSigner
                                                        : FormatPropSignedBy(sig.signerName));
                break;
            case SignatureStatus::CheckFailed:
                lines.push_back(kStrPropSignatureCheckFailed);
                break;
        }
    }

    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    ShowMessage(console, kStrFilePropertiesTitle, lines, redraw);
}

// IS-0002: a file main.cpp is watching for changes while its external
// editor runs in the background (Config::waitForEditorToClose == false).
// The watch's lifetime is tied to editorProcess's own lifetime (checked
// alongside changeNotification on every poll) so it can't accumulate
// unboundedly across a session of editing many different files in turn.
struct PendingEdit {
    std::filesystem::path filePath;
    HANDLE editorProcess = nullptr;
    HANDLE changeNotification = nullptr;
};

// Refreshes (and, via IS-0001's Panel::pendingSelectName_ mechanism,
// re-selects) `editedFile`'s entry in whichever of left/right currently has
// it open as their own directory â€” neither, either, or both may match.
void RefreshPanelsForEditedFile(Panel& left, Panel& right, const std::filesystem::path& editedFile) {
    std::filesystem::path parent = editedFile.parent_path();
    std::wstring name = editedFile.filename().wstring();
    // IS-0015: PathsEquivalent, not a plain path == (case-sensitive even on
    // Windows, and doesn't normalize a trailing separator) -- the same fix
    // applied to every other single-panel-refresh call site below.
    if (PathsEquivalent(left.Path(), parent)) left.NavigateTo(left.Path(), name);
    if (PathsEquivalent(right.Path(), parent)) right.NavigateTo(right.Path(), name);
}

// IS-0002: called once per main-loop iteration â€” the same ~16 ms cadence
// LIST-008's incremental panel loading already relies on â€” with
// non-blocking checks (WaitForSingleObject's 0 timeout never blocks, it
// only polls), so this never stalls the input loop. A signaled
// changeNotification means the watched directory changed; re-arm it and
// refresh. A signaled editorProcess means the editor has exited: do one
// final refresh (in case a save happened right before exit hasn't been
// picked up yet), then close both handles and drop the entry.
void PollPendingEdits(std::vector<PendingEdit>& pendingEdits, Panel& left, Panel& right) {
    for (size_t i = 0; i < pendingEdits.size();) {
        PendingEdit& edit = pendingEdits[i];
        bool changed = WaitForSingleObject(edit.changeNotification, 0) == WAIT_OBJECT_0;
        if (changed) {
            RefreshPanelsForEditedFile(left, right, edit.filePath);
            FindNextChangeNotification(edit.changeNotification);
        }
        bool exited = WaitForSingleObject(edit.editorProcess, 0) == WAIT_OBJECT_0;
        if (exited) {
            if (!changed) RefreshPanelsForEditedFile(left, right, edit.filePath);
            FindCloseChangeNotification(edit.changeNotification);
            CloseHandle(edit.editorProcess);
            pendingEdits.erase(pendingEdits.begin() + static_cast<ptrdiff_t>(i));
            continue;
        }
        ++i;
    }
}

void DoEdit(Console& console, Panel& left, Panel& right, bool leftActive, const Config& config, Logger& logger,
           std::vector<PendingEdit>& pendingEdits) {
    Panel& target = leftActive ? left : right;
    auto filePath = CursorFilePath(target);
    if (!filePath) return;

    std::wstring editor = ResolveEditorCommand();

    if (config.waitForEditorToClose) {
        console.SuspendForChildProcess();
        ProcessRunResult result = RunAndWait(editor, {filePath->wstring()}, target.Path());
        console.ResumeAfterChildProcess();

        target.Refresh();
        SyncOtherPanelIfSamePath(target, leftActive ? right : left);
        if (!result.started) {
            auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
            ShowMessage(console, kStrCannotStartEditorTitle, {kStrEditorLabel + editor, result.error}, redraw);
        }
        return;
    }

    // IS-0002: launch detached and return immediately â€” no console
    // suspend/resume, since we're not giving up the console (see
    // RunDetached's own doc comment for the console-subsystem-editor
    // trade-off this deliberately accepts).
    DetachedProcessResult result = RunDetached(editor, {filePath->wstring()}, target.Path());
    if (!result.started) {
        auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
        ShowMessage(console, kStrCannotStartEditorTitle, {kStrEditorLabel + editor, result.error}, redraw);
        return;
    }

    PendingEdit edit;
    edit.filePath = *filePath;
    edit.editorProcess = result.processHandle;
    edit.changeNotification = FindFirstChangeNotificationW(
        filePath->parent_path().c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    if (edit.changeNotification == INVALID_HANDLE_VALUE) {
        // ERR-004: a failed watch is a missed convenience, not a reason to
        // refuse an edit that's already started â€” log it and move on
        // without tracking this file.
        logger.Log(LogLevel::Warning, L"Could not watch \"" + filePath->wstring() +
                                       L"\" for changes: " + FormatWinError(GetLastError()));
        CloseHandle(edit.editorProcess);
        return;
    }
    pendingEdits.push_back(edit);
}

// IS-0006: Ctrl+F11's config-file edit-and-live-reload session, tracked across
// main-loop iterations the same way PendingEdit (IS-0002) tracks an F4
// edit â€” but unlike PendingEdit (whose reaction to a change is "refresh a
// panel's directory listing"), a config-file change means "reload Config
// from disk and re-apply every setting it drives to already-running app
// state," an entirely different reaction, so this gets its own small watch
// rather than reusing PendingEdit/PollPendingEdits. Only one can ever be
// meaningful at a time (there's only one config file), unlike
// PendingEdit's vector of concurrently-edited panel files.
struct ConfigWatch {
    bool active = false;
    HANDLE editorProcess = nullptr;
    HANDLE changeNotification = nullptr;
};

// IS-0006: applies every Config field that's only ever consumed once, at
// startup, to the already-running app â€” the fields read live via a
// by-const-reference Config& at each call site (confirmRecycleBinDelete,
// confirmPermanentDelete, enterFileAction, persistCommandHistory,
// waitForEditorToClose) need no plumbing here, since reassigning wmain's
// own `config` variable (done by the caller, right after this) is already
// enough for their next read to see the new value.
void ApplyConfigLive(const Config& newConfig, Console& console, Panel& left, Panel& right, Logger& logger) {
    console.SetUseBasicSymbols(newConfig.useBasicSymbols);
    left.SetGroupDirectoriesFirst(newConfig.groupDirectoriesFirst);
    right.SetGroupDirectoriesFirst(newConfig.groupDirectoriesFirst);
    gVisibleColumns = newConfig.visibleColumns;
    gTypeColumnWidth = newConfig.typeColumnWidth;
    gMtimeColumnWidth = newConfig.mtimeColumnWidth;
    gSizeColumnWidth = newConfig.sizeColumnWidth;
    ApplyThemePalette(newConfig.colorTheme == ColorTheme::HighContrast ? kHighContrastThemePalette
                                                                       : kDefaultThemePalette);
    logger.SetLevel(newConfig.logLevel);
}

// IS-0006: reloads Config from disk and applies it live â€” shared by
// PollConfigWatch's two trigger points (a save, and the editor's final
// exit) so both go through the exact same reload-plus-apply-plus-report
// sequence. Deliberately does not re-run AppendMissingConfigKeys (that
// backfill is startup-only â€” see the fix plan's own scope note on why:
// re-adding a key the user just deleted while editing live would fight
// their edit).
void ReloadConfigLive(Config& config, Console& console, Panel& left, Panel& right, bool leftActive, Logger& logger) {
    auto reloaded = LoadConfig();
    config = reloaded.config;
    ApplyConfigLive(config, console, left, right, logger);
    if (!reloaded.warnings.empty()) {
        auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
        std::vector<std::wstring> lines{kStrUsingDefaultForSettings};
        lines.insert(lines.end(), reloaded.warnings.begin(), reloaded.warnings.end());
        ShowMessage(console, kStrConfigurationWarningTitle, lines, redraw);
        for (const auto& warning : reloaded.warnings) {
            logger.Log(LogLevel::Warning, L"Config: " + warning);
        }
    }
}

// IS-0006: called once per main-loop iteration, the same ~16 ms cadence
// PollPendingEdits already uses. A signaled changeNotification means the
// config file's directory changed (most likely: it was saved) â€” reload
// and apply, then re-arm the watch. A signaled editorProcess means the
// editor exited: one final reload in case a save right before exit hasn't
// been picked up yet, then close both handles and clear the watch.
void PollConfigWatch(ConfigWatch& watch, Config& config, Console& console, Panel& left, Panel& right,
                     bool leftActive, Logger& logger) {
    if (!watch.active) return;
    bool changed = WaitForSingleObject(watch.changeNotification, 0) == WAIT_OBJECT_0;
    if (changed) {
        ReloadConfigLive(config, console, left, right, leftActive, logger);
        FindNextChangeNotification(watch.changeNotification);
    }
    bool exited = WaitForSingleObject(watch.editorProcess, 0) == WAIT_OBJECT_0;
    if (exited) {
        if (!changed) ReloadConfigLive(config, console, left, right, leftActive, logger);
        FindCloseChangeNotification(watch.changeNotification);
        CloseHandle(watch.editorProcess);
        watch = ConfigWatch{};
    }
}

// IS-0006: Ctrl+F11 opens the config file in the external editor (same
// resolution/launch logic as F4's DoEdit) and starts a ConfigWatch so
// PollConfigWatch can reload and apply it live once the user saves.
// Always launches detached, regardless of Config::waitForEditorToClose â€”
// that setting is specifically about F4's own file-editing UX trade-off (a
// console-subsystem %EDITOR% sharing this window); blocking the whole app
// until the config editor closes would defeat the point of a *live*
// reload while you keep working. EnsureConfigFileExists() has already run
// unconditionally before the main loop starts, so the file is always
// guaranteed to exist by the time Ctrl+F11 could ever be pressed.
void DoEditConfig(Console& console, const Panel& left, const Panel& right, bool leftActive, Logger& logger,
                  ConfigWatch& watch) {
    std::filesystem::path configPath = ConfigFilePath();
    if (configPath.empty()) return;  // CFG-002: %APPDATA% couldn't be resolved

    std::wstring editor = ResolveEditorCommand();
    DetachedProcessResult result = RunDetached(editor, {configPath.wstring()}, configPath.parent_path());
    if (!result.started) {
        auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
        ShowMessage(console, kStrCannotStartEditorTitle, {kStrEditorLabel + editor, result.error}, redraw);
        return;
    }

    if (watch.active) {
        // A previous config-edit session's watch never got its final
        // reload (e.g. Ctrl+F11 pressed again before the first editor
        // window closed) â€” close it out cleanly first rather than leak it.
        FindCloseChangeNotification(watch.changeNotification);
        CloseHandle(watch.editorProcess);
    }
    watch.active = true;
    watch.editorProcess = result.processHandle;
    watch.changeNotification = FindFirstChangeNotificationW(
        configPath.parent_path().c_str(), FALSE, FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE);
    if (watch.changeNotification == INVALID_HANDLE_VALUE) {
        logger.Log(LogLevel::Warning, L"Could not watch \"" + configPath.wstring() +
                                       L"\" for changes: " + FormatWinError(GetLastError()));
        CloseHandle(watch.editorProcess);
        watch = ConfigWatch{};
    }
}

// --- KEY-005: keys.conf -------------------------------------------------------

// Re-reads keys.conf into gKeys.map (a missing file means the defaults),
// re-binds plugin shortcuts, and -- when `console` is given -- shows any
// problems once. Every problem is also logged (ERR-004).
void ReloadKeyBindings(Console* console, const Panel& left, const Panel& right, bool leftActive, Logger& logger,
                       PluginManager& pluginManager) {
    std::filesystem::path path = KeysFilePath();
    std::error_code ec;
    std::vector<std::wstring> warnings;
    if (!path.empty() && std::filesystem::exists(path, ec)) {
        gKeys.map = mc::KeyMap::Load(mc::ReadUtf8TextFile(path), warnings);
        gKeys.fileSeen = true;
        gKeys.lastWrite = std::filesystem::last_write_time(path, ec);
        gKeys.lastSize = std::filesystem::file_size(path, ec);
    } else {
        gKeys.map = mc::KeyMap::Defaults();
        gKeys.fileSeen = false;
    }
    pluginManager.SetKeyMap(&gKeys.map);
    for (const auto& warning : warnings) logger.Log(LogLevel::Warning, warning);
    if (console && !warnings.empty()) {
        std::vector<std::wstring> lines{kStrKeyBindingsWarningIntro};
        size_t shown = std::min<size_t>(warnings.size(), 8);
        lines.insert(lines.end(), warnings.begin(), warnings.begin() + static_cast<std::ptrdiff_t>(shown));
        if (warnings.size() > shown) lines.push_back(FormatMoreFailures(warnings.size() - shown));
        ShowMessage(*console, kStrKeyBindingsWarningTitle, lines, [&]() { DrawFrame(*console, left, right, leftActive); });
    }
}

// Checked every main-loop iteration, but stats the file at most twice a second.
void PollKeyBindings(Console& console, const Panel& left, const Panel& right, bool leftActive, Logger& logger,
                     PluginManager& pluginManager) {
    ULONGLONG now = GetTickCount64();
    if (now - gKeys.lastCheck < 500) return;
    gKeys.lastCheck = now;
    std::filesystem::path path = KeysFilePath();
    if (path.empty()) return;
    std::error_code ec;
    bool exists = std::filesystem::exists(path, ec);
    if (!exists && !gKeys.fileSeen) return;
    if (exists && gKeys.fileSeen) {
        auto write = std::filesystem::last_write_time(path, ec);
        auto size = std::filesystem::file_size(path, ec);
        if (write == gKeys.lastWrite && size == gKeys.lastSize) return;
    }
    ReloadKeyBindings(&console, left, right, leftActive, logger, pluginManager);
}

// Settings > Edit key bindings: creates keys.conf from every default
// (commented out) on first use, then opens it in the editor, detached --
// PollKeyBindings applies each save.
void DoEditKeyBindings(Console& console, const Panel& left, const Panel& right, bool leftActive) {
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    std::filesystem::path path = KeysFilePath();
    if (path.empty()) return;
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        std::filesystem::create_directories(path.parent_path(), ec);
        if (!mc::WriteUtf8TextFileAtomic(path, mc::DefaultKeysFileText())) {
            ShowMessage(console, kStrKeyBindingsWarningTitle, {kStrCannotCreateKeysFile, path.wstring()}, redraw);
            return;
        }
    }
    std::wstring editor = ResolveEditorCommand();
    DetachedProcessResult result = RunDetached(editor, {path.wstring()}, path.parent_path());
    if (!result.started) {
        ShowMessage(console, kStrCannotStartEditorTitle, {kStrEditorLabel + editor, result.error}, redraw);
        return;
    }
    if (result.processHandle) CloseHandle(result.processHandle);
}

// VEE-002/VEE-008: runs the cursor file through its own Windows file
// association (ShellExecuteW), the same as double-clicking it in Explorer
// â€” an .exe runs, a document opens in whatever app is registered for it.
// The only path in the app that ever runs a file's own code/association
// rather than viewing or editing it; only reachable via Enter, and only
// once the user has explicitly configured enterFileAction=execute, so a
// file is never run without a deliberate, explicit action (VEE-008).
void DoExecute(Console& console, Panel& left, Panel& right, bool leftActive) {
    Panel& target = leftActive ? left : right;
    Panel& other = leftActive ? right : left;
    auto filePath = CursorFilePath(target);
    if (!filePath) return;

    std::wstring workDir = target.Path().wstring();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = filePath->c_str();
    info.lpDirectory = workDir.c_str();
    info.nShow = SW_SHOWNORMAL;

    console.SuspendForChildProcess();
    BOOL ok = ShellExecuteExW(&info);
    DWORD err = ok ? 0 : GetLastError();
    if (ok && info.hProcess) {
        WaitForSingleObject(info.hProcess, INFINITE);
        CloseHandle(info.hProcess);
    }
    console.ResumeAfterChildProcess();

    target.Refresh();
    SyncOtherPanelIfSamePath(target, other);
    if (!ok) {
        auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
        ShowMessage(console, kStrCannotOpenFileTitle, {filePath->filename().wstring(), FormatWinError(err)}, redraw);
    }
}

// --- FEA-0009: per-extension file actions (VEE-009..013) ---------------------

// The parsed file-actions.conf, reloaded lazily whenever its timestamp or
// size changed since the last load -- checked each time an action is about
// to be resolved, so a save (by hand, or by the Assign File to App manager)
// applies on the very next keypress with no watch handle to manage.
struct FileActionsState {
    mc::FileActionTable table;
    std::filesystem::file_time_type lastWrite{};
    uintmax_t lastSize = 0;
    bool loaded = false;
};
FileActionsState gFileActions;

const mc::FileActionTable& CurrentFileActions(Console& console, const Panel& left, const Panel& right, bool leftActive,
                                              Logger& logger) {
    std::filesystem::path path = mc::FileActionsFilePath();
    // Also catches the 1.11.0 sample block being left in a file the user is
    // editing while MyCommander runs (MigrateLegacyFileActionsText).
    mc::EnsureFileActionsFileExistsAt(path);
    std::error_code ec;
    auto writeTime = std::filesystem::last_write_time(path, ec);
    if (ec) writeTime = {};
    uintmax_t size = std::filesystem::file_size(path, ec);
    if (ec) size = 0;
    if (gFileActions.loaded && writeTime == gFileActions.lastWrite && size == gFileActions.lastSize)
        return gFileActions.table;

    auto parsed = mc::LoadFileActionsFrom(path);
    gFileActions = FileActionsState{std::move(parsed.table), writeTime, size, true};
    if (!parsed.warnings.empty()) {
        for (const auto& warning : parsed.warnings) logger.Log(LogLevel::Warning, L"File actions: " + warning);
        auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
        std::vector<std::wstring> lines{kStrFileActionsWarningIntro};
        lines.insert(lines.end(), parsed.warnings.begin(), parsed.warnings.end());
        ShowMessage(console, kStrFileActionsWarningTitle, lines, redraw);
    }
    return gFileActions.table;
}

// D4: "system:" -- the Windows association, without waiting (an Open action
// hands the file to a long-lived application, unlike Execute's DoExecute,
// which waits for a run program to finish).
void OpenWithShellAssociation(Console& console, const Panel& left, const Panel& right, bool leftActive,
                              const std::filesystem::path& file) {
    std::wstring workDir = file.parent_path().wstring();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_FLAG_NO_UI;
    info.lpVerb = L"open";
    info.lpFile = file.c_str();
    info.lpDirectory = workDir.c_str();
    info.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&info)) {
        DWORD err = GetLastError();
        auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
        ShowMessage(console, kStrCannotOpenFileTitle, {file.filename().wstring(), FormatWinError(err)}, redraw);
    }
}

// D4: runs one resolved action for `file`. Only ever reached from an
// explicit keypress (F3, Shift+Enter, the Shift+F3 chooser, or Enter with an
// explicitly configured enterFileAction) -- VEE-008; never elevated (SEC-002).
void RunFileAction(Console& console, const Panel& left, const Panel& right, bool leftActive, Logger& logger,
                   const mc::FileAction& action, const std::filesystem::path& file) {
    if (action.program == mc::kFileActionProgramInternalViewer) {
        ShowTextFileViewer(console, file);
        return;
    }
    if (auto pluginId = mc::FileActionPluginId(action)) {
        // EXT-003: a viewer plugin; if it isn't loaded or can't show the
        // file, say so and use the built-in viewer instead.
        if (gPluginManager && gPluginManager->ShowPluginView(*pluginId, file)) return;
        auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
        ShowMessage(console, kStrViewerPluginTitle, {FormatViewerPluginFailed(*pluginId)}, redraw);
        ShowTextFileViewer(console, file);
        return;
    }
    if (action.program == mc::kFileActionProgramSystem) {
        OpenWithShellAssociation(console, left, right, leftActive, file);
        return;
    }
    std::wstring arguments = mc::ExpandFileActionArguments(action.arguments, file);
    DetachedProcessResult result = mc::RunDetachedCommandLine(action.program, arguments, file.parent_path());
    if (result.started) {
        CloseHandle(result.processHandle);
        return;
    }
    // ERR-005: the program path and the error only -- never the arguments.
    logger.Log(LogLevel::Warning, L"File action failed: \"" + action.program + L"\": " + result.error);
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    ShowMessage(console, kStrCannotRunFileActionTitle,
                {mc::FileActionLabel(action), kStrProgramLabel + action.program, result.error}, redraw);
}

// F3 (and Enter with enterFileAction=view): the default "view" action, or
// the built-in viewer when none is registered -- exactly the old F3.
void DoViewAction(Console& console, const Panel& left, const Panel& right, bool leftActive, Logger& logger) {
    const Panel& target = leftActive ? left : right;
    auto filePath = CursorFilePath(target);
    if (!filePath) return;
    const auto& table = CurrentFileActions(console, left, right, leftActive, logger);
    auto action = mc::DefaultFileAction(table, filePath->filename().wstring(), mc::FileActionType::View);
    // EXT-003 D3: file-actions.conf's view default, then a viewer plugin
    // claiming the file as its default, then the built-in viewer.
    std::vector<mc::ViewerCandidate> viewers;
    if (!action && gPluginManager) viewers = gPluginManager->FindViewers(filePath->filename().wstring());
    size_t pluginIndex = 0;
    switch (mc::ChooseViewRoute(action.has_value(), viewers, pluginIndex)) {
        case mc::ViewRoute::FileAction:
            RunFileAction(console, left, right, leftActive, logger, *action, *filePath);
            return;
        case mc::ViewRoute::Plugin: {
            if (gPluginManager->ShowPluginView(viewers[pluginIndex].pluginId, *filePath)) return;
            auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
            ShowMessage(console, kStrViewerPluginTitle, {FormatViewerPluginFailed(viewers[pluginIndex].name)}, redraw);
            DoView(console, left, right, leftActive);
            return;
        }
        case mc::ViewRoute::BuiltIn:
            DoView(console, left, right, leftActive);
            return;
    }
}

// Shift+Enter (and Enter with enterFileAction=open): the default "open"
// action, or the Windows association when none is registered.
void DoOpenAction(Console& console, const Panel& left, const Panel& right, bool leftActive, Logger& logger) {
    const Panel& target = leftActive ? left : right;
    auto filePath = CursorFilePath(target);
    if (!filePath) return;
    const auto& table = CurrentFileActions(console, left, right, leftActive, logger);
    auto action = mc::DefaultFileAction(table, filePath->filename().wstring(), mc::FileActionType::Open);
    if (!action) {
        OpenWithShellAssociation(console, left, right, leftActive, *filePath);
        return;
    }
    RunFileAction(console, left, right, leftActive, logger, *action, *filePath);
}

// Shift+F3: every registered view/open action for the cursor file plus the
// two built-ins, so a non-default one can be picked. The effective default
// of each type is marked with '*'.
void DoChooseAction(Console& console, Panel& left, Panel& right, bool leftActive, Logger& logger) {
    Panel& target = leftActive ? left : right;
    auto filePath = CursorFilePath(target);
    if (!filePath) return;
    const auto& table = CurrentFileActions(console, left, right, leftActive, logger);
    std::wstring name = filePath->filename().wstring();

    std::vector<mc::FileAction> choices;
    std::vector<std::wstring> labels;
    for (mc::FileActionType type : {mc::FileActionType::View, mc::FileActionType::Open}) {
        auto found = mc::FindFileActions(table, name, type);
        auto def = mc::DefaultFileAction(table, name, type);
        mc::FileAction builtin;
        builtin.type = type;
        builtin.program = type == mc::FileActionType::View ? mc::kFileActionProgramInternalViewer
                                                             : mc::kFileActionProgramSystem;
        builtin.description = type == mc::FileActionType::View ? kStrBuiltinViewerLabel : kStrWindowsDefaultLabel;
        // Offered only once: skip it if the file already registers it itself
        // (the first-run sample registers internal:viewer for text files).
        bool alreadyListed = std::any_of(found.begin(), found.end(),
                                         [&](const mc::FileAction& a) { return a.program == builtin.program; });
        // Without a file-actions default, what F3 / Shift+Enter actually run
        // (EXT-003 D3): a viewer plugin claiming Default, else the built-in.
        std::wstring fallbackProgram = builtin.program;
        std::vector<mc::ViewerCandidate> viewers;
        if (type == mc::FileActionType::View && gPluginManager) {
            viewers = gPluginManager->FindViewers(name);
            size_t pluginIndex = 0;
            if (mc::ChooseViewRoute(false, viewers, pluginIndex) == mc::ViewRoute::Plugin)
                fallbackProgram = mc::kFileActionProgramPluginPrefix + viewers[pluginIndex].pluginId;
        }
        // EXT-003: viewer plugins claiming the file, unless file-actions.conf
        // already lists them as plugin:<id> for it.
        if (type == mc::FileActionType::View) {
            for (const auto& viewer : viewers) {
                bool listed = std::any_of(found.begin(), found.end(), [&](const mc::FileAction& a) {
                    auto id = mc::FileActionPluginId(a);
                    return id && _wcsicmp(id->c_str(), viewer.pluginId.c_str()) == 0;
                });
                if (listed) continue;
                mc::FileAction pluginView;
                pluginView.type = mc::FileActionType::View;
                pluginView.program = mc::kFileActionProgramPluginPrefix + viewer.pluginId;
                pluginView.description = viewer.name + kStrPluginSuffix;
                found.push_back(pluginView);
            }
        }
        if (!alreadyListed) found.push_back(builtin);
        for (const auto& action : found) {
            bool isEffectiveDefault = def ? action.sourceLine == def->sourceLine && action.sourceLine != 0
                                          : action.sourceLine == 0 && action.program == fallbackProgram;
            std::wstring keyword = mc::FileActionTypeKeyword(type);
            std::wstring label = (isEffectiveDefault ? L"* " : L"  ") + keyword + L"  " + mc::FileActionLabel(action);
            if (action.sourceLine != 0) label += L"  (" + action.program + L")";
            labels.push_back(label);
            choices.push_back(action);
        }
    }
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    int chosen = ShowListChoice(console, FormatChooseActionTitle(name), labels, redraw);
    if (chosen < 0) return;
    RunFileAction(console, left, right, leftActive, logger, choices[static_cast<size_t>(chosen)], *filePath);
}

// NAV-007: Ctrl+D / Commands > Directory Hotlist. Lists the named favorite
// folders (Favorites.h) plus "+ Add current folder"; Enter goes there (or
// adds), Del removes (after a Yes/No), F2 renames. Re-read from disk every
// time it opens -- the file is tiny, and that picks up hand edits with no
// watch to manage. Saved immediately after each change.
void DoDirectoryHotlist(Console& console, Panel& left, Panel& right, bool leftActive, Logger& logger) {
    Panel& target = leftActive ? left : right;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    const std::filesystem::path file = mc::FavoritesFilePath();

    auto loaded = mc::LoadFavoritesFrom(file);
    std::vector<mc::Favorite> favorites = std::move(loaded.favorites);
    if (!loaded.warnings.empty()) {
        // Shown once per distinct set of problems, not on every Ctrl+D.
        static std::vector<std::wstring> lastShown;
        if (loaded.warnings != lastShown) {
            lastShown = loaded.warnings;
            for (const auto& w : loaded.warnings) logger.Log(LogLevel::Warning, L"Favorites: " + w);
            std::vector<std::wstring> lines{kStrHotlistWarningIntro};
            lines.insert(lines.end(), loaded.warnings.begin(), loaded.warnings.end());
            ShowMessage(console, kStrHotlistTitle, lines, redraw);
        }
    }
    auto save = [&]() {
        if (mc::SaveFavoritesTo(file, favorites)) return;
        logger.Log(LogLevel::Warning, L"Favorites: could not save \"" + file.wstring() + L"\"");
        ShowMessage(console, kStrHotlistTitle, {kStrHotlistCannotSave}, redraw);
    };
    auto askName = [&](const std::wstring& title, const std::wstring& initial) -> std::optional<std::wstring> {
        auto typed = ShowTextPrompt(console, title, initial, redraw);
        if (!typed) return std::nullopt;
        std::wstring name = *typed;
        size_t a = name.find_first_not_of(L" \t");
        name = a == std::wstring::npos ? L"" : name.substr(a, name.find_last_not_of(L" \t") - a + 1);
        if (!mc::IsValidFavoriteName(name)) {
            ShowMessage(console, kStrHotlistTitle, {kStrHotlistInvalidName}, redraw);
            return std::nullopt;
        }
        return name;
    };

    // Opens with the current folder's own entry selected when it's a favorite
    // (lexical match, FindFavoriteByPath); otherwise on the first row.
    int focus = std::max(0, mc::FindFavoriteByPath(favorites, target.Path()));
    while (true) {
        size_t nameWidth = 0;
        for (const auto& f : favorites) nameWidth = std::max<size_t>(nameWidth, static_cast<size_t>(StringDisplayWidth(f.name)));
        std::vector<std::wstring> rows(favorites.size());
        std::transform(favorites.begin(), favorites.end(), rows.begin(), [&](const auto& f) {
            return PadOrTrim(f.name, nameWidth) + L"   " + f.path.wstring();
        });
        const int addIndex = static_cast<int>(rows.size());
        rows.push_back(FormatHotlistAddRow(target.Path().wstring()));

        auto choice = mc::ShowListChoiceWithKeys(console, kStrHotlistTitle, rows, redraw, focus, {VK_DELETE, VK_F2},
                                                 kStrHotlistHint);
        if (choice.index < 0) return;
        focus = choice.index;
        bool onFavorite = choice.index < addIndex;

        if (choice.key == VK_RETURN && !onFavorite) {
            int existing = mc::FindFavoriteByPath(favorites, target.Path());
            if (existing >= 0) {
                ShowMessage(console, kStrHotlistTitle,
                            {FormatHotlistAlreadyThere(favorites[static_cast<size_t>(existing)].name)}, redraw);
                focus = existing;
                continue;
            }
            auto name = askName(kStrHotlistAddPrompt, mc::DefaultFavoriteName(target.Path()));
            if (!name) continue;
            favorites.push_back({*name, target.Path()});
            save();
            focus = static_cast<int>(favorites.size()) - 1;
            continue;
        }
        if (!onFavorite) continue;  // Del/F2 on the "+ Add" row: nothing to act on
        mc::Favorite& fav = favorites[static_cast<size_t>(choice.index)];
        if (choice.key == VK_RETURN) {
            target.NavigateTo(fav.path);  // NAV-009 / LIST-006 handle reparse points and unreachable folders
            return;
        }
        if (choice.key == VK_F2) {
            auto name = askName(kStrHotlistRenamePrompt, fav.name);
            if (!name) continue;
            fav.name = *name;
            save();
            continue;
        }
        if (choice.key == VK_DELETE) {
            int sure = ShowChoiceDialog(console, kStrHotlistRemoveTitle, {fav.name + L"   " + fav.path.wstring()},
                                        {{L'Y', kStrYesLabel}, {L'N', kStrNoLabel}}, redraw);
            if (sure != 0) continue;
            favorites.erase(favorites.begin() + choice.index);
            save();
            focus = std::min(focus, static_cast<int>(favorites.size()));
        }
    }
}

// FEA-0009 Phase 2 (VEE-013): File > Assign File to App -- lists the
// actions registered for the cursor file's extension and lets the user set
// a default, remove one, add one, or open the file for hand-editing. Every
// change is a line-level edit (FileActions.h), so the user's own comments
// and layout survive; the next CurrentFileActions call picks it up.
void DoAssignFileToApp(Console& console, Panel& left, Panel& right, bool leftActive, Logger& logger) {
    Panel& target = leftActive ? left : right;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    auto filePath = CursorFilePath(target);
    if (!filePath) {
        ShowMessage(console, kStrAssignFileToAppTitle, {kStrAssignNeedsFile}, redraw);
        return;
    }
    std::wstring name = filePath->filename().wstring();
    std::wstring ext = filePath->extension().wstring();
    std::wstring pattern = ext.empty() || ext == name ? name : ext;
    std::transform(pattern.begin(), pattern.end(), pattern.begin(),
                   [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    std::filesystem::path path = mc::FileActionsFilePath();
    mc::EnsureFileActionsFileExistsAt(path);

    auto save = [&](const std::wstring& text) {
        if (mc::SaveFileActionsText(path, text)) return;
        logger.Log(LogLevel::Warning, L"File actions: could not save \"" + path.wstring() + L"\"");
        ShowMessage(console, kStrAssignFileToAppTitle, {kStrCannotSaveFileActions}, redraw);
    };

    int focus = 0;
    while (true) {
        std::wstring text = mc::ReadFileActionsText(path);
        auto parsed = mc::ParseFileActions(text);
        std::vector<mc::FileAction> own;
        std::vector<std::wstring> labels;
        for (const auto& block : parsed.table.blocks) {
            bool exact = std::any_of(block.patterns.begin(), block.patterns.end(),
                                     [&](const std::wstring& p) { return _wcsicmp(p.c_str(), pattern.c_str()) == 0; });
            if (!exact) continue;
            // An action in a block shared with other patterns affects those
            // too if it's removed or made default -- say so in its label.
            std::wstring sharedWith;
            for (const auto& p : block.patterns)
                if (_wcsicmp(p.c_str(), pattern.c_str()) != 0) sharedWith += (sharedWith.empty() ? L"" : L" ") + p;
            for (const auto& action : block.actions) {
                std::wstring keyword = mc::FileActionTypeKeyword(action.type);
                std::wstring label = (action.isDefault ? L"* " : L"  ") + keyword + L"  " + mc::FileActionLabel(action) +
                                     L"  (" + action.program + (action.arguments.empty() ? L"" : L" " + action.arguments) + L")";
                if (!sharedWith.empty()) label += L"  [shared with " + sharedWith + L"]";
                labels.push_back(label);
                own.push_back(action);
            }
        }
        const int addIndex = static_cast<int>(labels.size());
        labels.push_back(kStrAddFileAction);
        // EXT-003: offered when a loaded viewer plugin claims this file.
        std::vector<mc::ViewerCandidate> viewers =
            gPluginManager ? gPluginManager->FindViewers(name) : std::vector<mc::ViewerCandidate>{};
        const int addPluginIndex = viewers.empty() ? -1 : static_cast<int>(labels.size());
        if (!viewers.empty()) labels.push_back(kStrAddViewerPlugin);
        labels.push_back(kStrEditFileActionsFile);

        int chosen = ShowListChoice(console, FormatFileActionsForTitle(pattern), labels, redraw, focus);
        if (chosen < 0) return;
        focus = chosen;

        if (chosen < addIndex) {
            const mc::FileAction& action = own[static_cast<size_t>(chosen)];
            int what = ShowChoiceDialog(console, FormatFileActionsForTitle(pattern), {labels[static_cast<size_t>(chosen)]},
                                        {{L'D', L"efault"}, {L'R', L"emove"}, {L'C', L"ancel"}}, redraw);
            if (what == 0) {
                save(mc::SetDefaultFileActionForPattern(text, pattern, action.sourceLine));
            } else if (what == 1) {
                int sure = ShowChoiceDialog(console, kStrRemoveFileActionQuestion, {labels[static_cast<size_t>(chosen)]},
                                            {{L'Y', kStrYesLabel}, {L'N', kStrNoLabel}}, redraw);
                if (sure == 0) {
                    save(mc::RemoveFileActionLine(text, action.sourceLine));
                    focus = 0;
                }
            }
            continue;
        }
        if (chosen == addIndex) {
            std::vector<mc::FormField> fields(5);
            fields[0].kind = mc::FormFieldKind::Choice;
            fields[0].label = L"Type";
            fields[0].choices = {L"view", L"open"};
            fields[1].kind = mc::FormFieldKind::Text;
            fields[1].label = L"Description";
            fields[2].kind = mc::FormFieldKind::Text;
            fields[2].label = L"Program";
            fields[3].kind = mc::FormFieldKind::Text;
            fields[3].label = L"Arguments";
            fields[3].text = L"\"%f\"";
            fields[3].textCursor = fields[3].text.size();
            fields[4].kind = mc::FormFieldKind::Checkbox;
            fields[4].label = L"Default for this type";
            fields[4].boolValue = true;
            if (!mc::ShowFormDialog(console, FormatFileActionsForTitle(pattern), fields, redraw)) continue;
            mc::FileAction action;
            action.type = fields[0].choiceIndex == 0 ? mc::FileActionType::View : mc::FileActionType::Open;
            action.description = fields[1].text;
            action.program = fields[2].text;
            action.arguments = fields[3].text;
            action.isDefault = fields[4].boolValue;
            // Trim surrounding spaces/quotes the way the parser will read them back.
            auto trim = [](std::wstring s) {
                size_t a = s.find_first_not_of(L" \t");
                if (a == std::wstring::npos) return std::wstring();
                return s.substr(a, s.find_last_not_of(L" \t") - a + 1);
            };
            action.description = trim(action.description);
            action.program = trim(action.program);
            action.arguments = trim(action.arguments);
            if (action.program.empty()) {
                ShowMessage(console, kStrAssignFileToAppTitle, {kStrProgramRequired}, redraw);
                continue;
            }
            save(mc::AddFileActionLine(text, pattern, action));
            continue;
        }
        if (chosen == addPluginIndex) {
            // EXT-003: register a viewer plugin as this pattern's default view action.
            std::vector<std::wstring> names(viewers.size());
            std::transform(viewers.begin(), viewers.end(), names.begin(), [](const auto& viewer) {
                return viewer.name + L"  (plugin:" + viewer.pluginId + L")";
            });
            int pick = ShowListChoice(console, FormatFileActionsForTitle(pattern), names, redraw);
            if (pick < 0) continue;
            const mc::ViewerCandidate& viewer = viewers[static_cast<size_t>(pick)];
            mc::FileAction action;
            action.type = mc::FileActionType::View;
            action.description = viewer.name;
            action.program = mc::kFileActionProgramPluginPrefix + viewer.pluginId;
            action.isDefault = true;
            save(mc::AddFileActionLine(text, pattern, action));
            continue;
        }
        // Edit the file by hand: same detached %EDITOR% launch as F4; the
        // next CurrentFileActions call notices the save on its own.
        DetachedProcessResult result = RunDetached(ResolveEditorCommand(), {path.wstring()}, path.parent_path());
        if (result.started) {
            CloseHandle(result.processHandle);
        } else {
            ShowMessage(console, kStrCannotStartEditorTitle, {kStrEditorLabel + ResolveEditorCommand(), result.error},
                        redraw);
        }
        return;
    }
}

// EXT-001/CON-004/IS-0003: the actual copy/move engine call, shared by
// DoCopyOrMove (F5/F6, which first prompts for `destDir` via ShowTextPrompt)
// and IS-0003's drag-and-drop drop handler (which already knows `destDir` â€”
// it's the panel the item was dropped on â€” and so skips the prompt
// entirely). Everything from the destination existence check onward is
// identical either way: this UI layer only decides what to do about each
// outcome (ask, show an error), never touches std::filesystem directly.
//
// FOP-019: `copyAsTarget`, when given (copy only, exactly one item), is the
// full path the single item is copied to under its own new name (Shift+F5);
// `destDir` is then that target's parent directory, so the same "create the
// missing destination folder?" check applies to it unchanged.
void PerformCopyOrMove(Console& console, Panel& left, Panel& right, bool leftActive, const Panel& source,
                       const std::vector<std::wstring>& items, const std::filesystem::path& destDir, bool isMove,
                       Logger& logger, const std::optional<std::filesystem::path>& copyAsTarget = std::nullopt) {
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    std::wstring verb = isMove ? kStrVerbMove : kStrVerbCopy;

    // FOP-014: F5/F6, Shift+F5 and drag-and-drop all come through here.
    std::vector<std::filesystem::path> touched = ItemPaths(source.Path(), items);
    touched.push_back(copyAsTarget ? *copyAsTarget : destDir);
    if (RefuseIfBusyWithJob(console, left, right, leftActive, touched)) return;

    DestinationState destState = CheckCopyMoveDestination(destDir);
    if (destState == DestinationState::DoesNotExist) {
        int choice = ShowChoiceDialog(console, kStrCreateDestinationTitle,
                                      {L"\"" + destDir.wstring() + kStrDoesNotExistSuffix},
                                      {{L'Y', kStrYesCreateItLabel}, {L'N', kStrNoCancelLabel}}, redraw);
        if (choice != 0) return;
        auto created = CreateDestinationDirectory(destDir);
        if (!created.ok) {
            ShowMessage(console, kStrCannotCreateDestinationTitle, {destDir.wstring(), created.error}, redraw);
            return;
        }
    } else if (destState == DestinationState::ExistsButNotDirectory) {
        ShowMessage(console, FormatCannotVerbTitle(verb), {destDir.wstring(), kStrIsNotADirectory}, redraw);
        return;
    }

    ProgressUi ui{console, left, right, leftActive, isMove ? kStrVerbMoving : kStrVerbCopying};
    ui.primaryPath = source.Path().wstring();
    ui.destPath = destDir.wstring();
    auto onConflict = [&](const std::filesystem::path& d) { return AskConflict(console, left, right, leftActive, d); };
    auto onItemStart = [&](const std::wstring& n, int done, int total) { return ui.OnItemStart(n, done, total); };
    auto cancelPoll = [&]() { return ui.PollCancel(); };
    auto onError = [&](const std::wstring& n, const std::wstring& msg, DWORD win32Error) {
        return AskFileError(console, left, right, leftActive, n, msg, win32Error);
    };
    auto onByteProgress = [&](uint64_t transferred, uint64_t total) { ui.OnByteProgress(transferred, total); };
    // SEC-003: only reached if the user explicitly chose "Elevate & Retry" on
    // an access-denied failure (AskFileError above) â€” a UAC prompt for this
    // one item, never anything broader.
    auto onElevateCopy = [&](const std::filesystem::path& src, const std::filesystem::path& dst, std::wstring& err) {
        logger.Log(LogLevel::Warning, L"Elevating copy: \"" + src.wstring() + L"\" -> \"" + dst.wstring() + L"\"");
        DWORD result = RunElevated(ElevatedRequest{ElevatedAction::Copy, src, dst});
        if (result == ERROR_SUCCESS) return true;
        err = FormatWinError(result);
        logger.Log(LogLevel::Error, L"Elevated copy failed: \"" + src.wstring() + L"\": " + err);
        return false;
    };
    auto onElevateMove = [&](const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite,
                             std::wstring& err) {
        logger.Log(LogLevel::Warning, L"Elevating move: \"" + src.wstring() + L"\" -> \"" + dst.wstring() + L"\"");
        ElevatedRequest req{ElevatedAction::Move, src, dst};
        req.overwrite = overwrite;
        DWORD result = RunElevated(req);
        if (result == ERROR_SUCCESS) return true;
        err = FormatWinError(result);
        logger.Log(LogLevel::Error, L"Elevated move failed: \"" + src.wstring() + L"\": " + err);
        return false;
    };

    OperationOutcome outcome =
        isMove ? MoveItems(source.Path(), items, destDir, onConflict, onItemStart, cancelPoll, onError, onByteProgress,
                          onElevateMove)
        : copyAsTarget
            ? mc::CopyItemAs(source.Path(), items.front(), *copyAsTarget, onConflict, onItemStart, cancelPoll,
                             onError, onByteProgress, onElevateCopy)
            : CopyItems(source.Path(), items, destDir, onConflict, onItemStart, cancelPoll, onError, onByteProgress,
                        onElevateCopy);

    left.Refresh();
    right.Refresh();
    ReportOutcome(console, left, right, leftActive, verb, outcome, logger);
}

// VEE-002: activates the active panel's cursor entry â€” a directory (or
// "..") is entered via EnterSelected(); a file follows the configured
// default action (View/Edit/Execute), same as F3/F4 always meaning
// View/Edit regardless of the setting. Shared by plain Enter (with an
// empty command line) and a mouse double-click (UI-012), so both trigger
// byte-for-byte identical behavior rather than two parallel copies of the
// same switch.
void ActivateCursorEntry(Console& console, Panel& left, Panel& right, bool leftActive, const Config& config,
                         Logger& logger, PluginManager& pluginManager, std::vector<PendingEdit>& pendingEdits) {
    Panel& target = leftActive ? left : right;
    if (auto filePath = CursorFilePath(target)) {
        if (pluginManager.TryEnterAsArchive(leftActive ? kMcPanelLeft : kMcPanelRight, *filePath)) return;
        switch (config.enterFileAction) {
            case EnterFileAction::Edit:
                DoEdit(console, left, right, leftActive, config, logger, pendingEdits);
                break;
            case EnterFileAction::Execute:
                DoExecute(console, left, right, leftActive);
                break;
            case EnterFileAction::Open:
                DoOpenAction(console, left, right, leftActive, logger);  // FEA-0009
                break;
            case EnterFileAction::View:
            default:
                // FEA-0009: the default "view" action -- the built-in viewer
                // unless file-actions.conf registers another one.
                DoViewAction(console, left, right, leftActive, logger);
                break;
        }
    } else {
        target.EnterSelected();
    }
}

// IS-0003: drag-and-drop state, tracked across consecutive HandleMouseEvent
// calls (like PendingEdit's own cross-iteration lifetime) since Windows
// console mouse events carry no distinct "button released" event type â€” a
// release is inferred only by noticing FROM_LEFT_1ST_BUTTON_PRESSED
// disappeared from dwButtonState between one event and the next.
// `dragCandidate` covers the gap between a plain press and the first
// MOUSE_MOVED event that would promote it to an actual drag, so a click
// with no movement in between never becomes one.
struct DragState {
    bool dragCandidate = false;
    bool dragging = false;
    bool sourceIsLeftPanel = false;
    std::vector<std::wstring> items;
};

// IS-0004: right-click/drag selection state, mirroring DragState's own
// cross-event lifetime for the same reason (no distinct "button released"
// event exists to key off of). Unlike DragState there's no "candidate"
// stage â€” a right-press has no other meaning in this app to disambiguate
// against, so it commits to toggling that entry immediately.
struct SelectDragState {
    bool active = false;
    bool isLeftPanel = false;
    bool targetSelected = false;  // the state every further entry is forced to
    int lastIndex = -1;           // avoids re-processing the same cell repeatedly
};

// IS-0003: injects a synthetic key-down/key-up pair into the console's own
// input queue via WriteConsoleInputW, so a hint-bar click can trigger
// exactly what pressing that key for real would â€” reusing wmain's existing
// ~600-line key-dispatch switch untouched, rather than a second, parallel
// copy of it. The main loop's next ReadConsoleInputW call picks this back
// up like any other keypress.
void SimulateKeyPress(Console& console, const HintAction& action) {
    DWORD controlKeyState = 0;
    if (action.ctrlHeld) controlKeyState |= LEFT_CTRL_PRESSED;
    if (action.shiftHeld) controlKeyState |= SHIFT_PRESSED;
    if (action.altHeld) controlKeyState |= LEFT_ALT_PRESSED;

    INPUT_RECORD records[2] = {};
    for (auto& record : records) {
        record.EventType = KEY_EVENT;
        record.Event.KeyEvent.wRepeatCount = 1;
        record.Event.KeyEvent.wVirtualKeyCode = action.virtualKeyCode;
        record.Event.KeyEvent.wVirtualScanCode =
            static_cast<WORD>(MapVirtualKeyW(action.virtualKeyCode, MAPVK_VK_TO_VSC));
        record.Event.KeyEvent.dwControlKeyState = controlKeyState;
    }
    records[0].Event.KeyEvent.bKeyDown = TRUE;
    records[1].Event.KeyEvent.bKeyDown = FALSE;

    // KEY-005: menus and hint clicks inject *default* keys, which must reach
    // the switch untranslated whatever keys.conf says.
    mc::NoteInjectedKey(mc::KeyChord{action.virtualKeyCode, action.ctrlHeld, action.shiftHeld, action.altHeld});
    DWORD written = 0;
    WriteConsoleInputW(console.InputHandle(), records, 2, &written);
}

// FEA-0005: executes one menu item's action, per its MenuItemKind
// (MenuBar.h). A SimulatedKey item reuses SimulateKeyPress unchanged â€” the
// exact same "replay the key press" mechanism IS-0003's hint-bar clicks
// already use, not a second, parallel action-dispatch table.
// ToggleColorMode is the one genuinely new piece of logic this feature
// adds â€” flips Config::colorTheme, persists it (Config.h's
// UpdateConfigValue, which rewrites just that one line in place), and
// applies it live via the exact same ApplyConfigLive IS-0006's own
// external-config-file-save path already uses. Stub shows a plain "not yet
// implemented" message and does nothing else â€” the request's own explicit
// scope for most items (most requested features, e.g. FTP/archive
// packing, don't exist yet).
void ExecuteMenuItem(Console& console, Config& config, Panel& left, Panel& right, bool leftActive, Logger& logger,
                     PluginManager& pluginManager, const MenuItem& item) {
    switch (item.kind) {
        case MenuItemKind::SimulatedKey:
            SimulateKeyPress(console, item.action);
            break;
        case MenuItemKind::ToggleColorMode: {
            config.colorTheme =
                config.colorTheme == ColorTheme::HighContrast ? ColorTheme::Default : ColorTheme::HighContrast;
            UpdateConfigValue(ConfigFilePath(), L"colorTheme",
                              config.colorTheme == ColorTheme::HighContrast ? L"highcontrast" : L"default");
            ApplyConfigLive(config, console, left, right, logger);
            break;
        }
        case MenuItemKind::Stub: {
            auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
            ShowMessage(console, kStrMenuNotImplementedTitle, {kStrMenuNotImplementedBody}, redraw);
            break;
        }
        case MenuItemKind::PluginCommand:
            // FEA-0002: dispatched through PluginManager, not
            // SimulateKeyPress -- a plugin command has no existing key
            // press to replay.
            pluginManager.DispatchMenuCommand(item.pluginId, item.pluginCommandId);
            break;
        case MenuItemKind::AssignFileToApp:
            DoAssignFileToApp(console, left, right, leftActive, logger);  // FEA-0009 Phase 2
            break;
        case MenuItemKind::EditKeyBindings:
            DoEditKeyBindings(console, left, right, leftActive);  // KEY-005
            break;
    }
}

// IS-0003/IS-0007: which hint-bar action (if any) a click at (mouseX,
// mouseY) hits â€” recomputes LayOutHintBar's row/column layout fresh from
// the same inputs DrawFrame used to render it (GetHintContext +
// LayOutHintBar are both pure and cheap) rather than smuggling drawn state
// out of DrawFrame, the same "recompute using the same deterministic
// inputs" approach HitTestPanel's own callers already follow. Now
// row-aware: a click on a segment that wrapped down to row > 0 resolves
// exactly the same way a click on row 0 always did.
std::optional<HintAction> MatchHintBarClick(SHORT consoleWidth, SHORT consoleHeight, HintContext context,
                                            SHORT mouseX, SHORT mouseY) {
    SHORT hintRows = static_cast<SHORT>(HintBarRowCount(context, consoleWidth));
    SHORT hintBarTopRow = static_cast<SHORT>(consoleHeight - hintRows);
    if (mouseY < hintBarTopRow || mouseY >= consoleHeight) return std::nullopt;

    std::vector<PlacedHintSegment> placed = LayOutHintBar(context, consoleWidth);
    std::vector<ClickableRegion> regions;
    std::vector<size_t> regionToPlaced;
    for (size_t i = 0; i < placed.size(); ++i) {
        if (!placed[i].segment.action) continue;
        SHORT row = static_cast<SHORT>(hintBarTopRow + placed[i].row);
        SHORT segWidth = static_cast<SHORT>(StringDisplayWidth(placed[i].segment.label));
        SHORT endCol = static_cast<SHORT>(std::min<int>(placed[i].col + segWidth, consoleWidth));
        regions.push_back({row, placed[i].col, endCol});
        regionToPlaced.push_back(i);
    }
    int matched = MatchClickRegion(regions, mouseX, mouseY);
    if (matched < 0) return std::nullopt;
    return placed[regionToPlaced[static_cast<size_t>(matched)]].segment.action;
}

// UI-012/IS-0003/IS-0004: mouse selection, scrolling, activation,
// drag-and-drop copy/move, hint-bar clicks, and right-click/Shift-click
// selection for the two main panels â€” the optional, never-required mouse
// support named there and in KEY-001's invariant (see Console.cpp's
// constructor comment for how that invariant is preserved once
// ENABLE_MOUSE_INPUT is on). Left-click moves the cursor to the clicked
// entry and, if the click landed in the other panel, makes that panel
// active â€” matching Total Commander's own click-to-activate convention. A
// double-click additionally activates the entry (ActivateCursorEntry),
// identical to pressing Enter on it. Dragging a selection (or the cursor
// entry, if nothing is selected) from one panel to the other copies it
// there by default, or moves it if Shift is held at the moment of drop â€”
// see DragState's own comment for why this needs to be a small state
// machine rather than a single self-contained gesture. A right-click
// toggles the clicked entry's selection mark, and holding the right button
// while dragging extends that selection to every further entry the mouse
// passes over (SelectDragState) â€” deliberately the *right* button, not a
// second use of the left button, since a plain left-drag already means
// copy/move (IS-0003). A Ctrl+left-click, or a Ctrl+right-click on a
// second entry after an ordinary right-click on a first one (IS-0004's
// addendum), each select every entry between wherever the cursor already
// was and the clicked entry (see Panel::SelectRange's own comment on how
// its anchor differs from Explorer's) â€” Ctrl+right-click is a single
// discrete gesture, not draggable, so it never starts a SelectDragState.
// Ctrl, not the Shift the issue originally asked for, is what triggers
// this: classic console host (conhost.exe) silently swallows a Shift-held
// mouse click before it ever reaches the app's input queue (confirmed
// against both buttons; no SetConsoleMode flag changes that), so Shift
// can't be used for a mouse gesture there. Shift is still what a plain
// left-drag's own drop uses to mean "move instead of copy" (IS-0003) â€”
// that's a different moment (a button *release*, not a fresh click) and
// hasn't shown the same symptom, so it's deliberately left alone here.
// The mouse wheel moves the active panel's cursor, 3 rows per
// notch (an ordinary wheel-scroll granularity), regardless of which panel
// the wheel was over â€” the same "always acts on the active panel" rule
// every other unmodified navigation key already follows. Deliberately out
// of scope: dialogs (apart from IS-0003's own dialog-button clicks, handled
// entirely inside Dialog.cpp), the drive/search pickers, the viewer, and
// the help screen stay keyboard-only â€” this covers exactly what the
// requirement names (panel selection, scrolling, activation, drag-and-drop,
// hint-bar commands, multi-select), not every screen in the app.
void HandleMouseEvent(Console& console, Panel& left, Panel& right, bool& leftActive, const std::wstring& commandLine,
                      Config& config, Logger& logger, PluginManager& pluginManager,
                      std::vector<PendingEdit>& pendingEdits, DragState& drag, SelectDragState& selectDrag,
                      MenuBarState& menuState, const MOUSE_EVENT_RECORD& mouse) {
    // FEA-0005: the menu bar takes exclusive control of mouse handling
    // whenever it's involved â€” a left-press on the always-visible top row
    // (open/switch/close a menu), a left-press on an open dropdown's item
    // (activate it and close), or any other mouse activity while a menu is
    // open (close it without activating anything, the standard "click/
    // scroll away to dismiss" convention) â€” checked first, before any
    // panel/hint-bar logic, the same "modal while showing" precedence a
    // right-click drag already gets over the left button's own gestures
    // just below.
    bool isLeftPress = mouse.dwEventFlags == 0 && (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
    if (isLeftPress || menuState.active) {
        const std::vector<TopMenu>& menus = gMergedMenus;
        std::vector<PlacedMenuEntry> barEntries = LayOutMenuBarRow(menus, console.Width());
        if (isLeftPress && mouse.dwMousePosition.Y == 0) {
            for (const auto& entry : barEntries) {
                SHORT entryEnd = static_cast<SHORT>(entry.col + entry.width);
                if (mouse.dwMousePosition.X >= entry.col && mouse.dwMousePosition.X < entryEnd) {
                    // Clicking the already-open menu's own entry closes it
                    // (a toggle); clicking a different entry switches to it.
                    menuState = (menuState.active && menuState.openMenuIndex == entry.menuIndex)
                                    ? CloseMenu()
                                    : OpenMenu(entry.menuIndex);
                    return;
                }
            }
            if (menuState.active) menuState = CloseMenu();
            return;
        }
        if (menuState.active) {
            if (isLeftPress) {
                auto openEntry = std::find_if(barEntries.begin(), barEntries.end(),
                                              [&](const auto& entry) { return entry.menuIndex == menuState.openMenuIndex; });
                const SHORT openMenuCol = openEntry != barEntries.end() ? openEntry->col : SHORT{0};
                const TopMenu& openMenu = menus[static_cast<size_t>(menuState.openMenuIndex)];
                DropdownLayout dropdown = LayOutOpenDropdown(openMenu, openMenuCol, console.Width());
                for (size_t i = 0; i < dropdown.itemRows.size(); ++i) {
                    SHORT rowEnd = static_cast<SHORT>(dropdown.col + dropdown.width);
                    if (mouse.dwMousePosition.Y == dropdown.itemRows[i] && mouse.dwMousePosition.X >= dropdown.col &&
                        mouse.dwMousePosition.X < rowEnd) {
                        ExecuteMenuItem(console, config, left, right, leftActive, logger, pluginManager, openMenu.items[i]);
                        menuState = CloseMenu();
                        return;
                    }
                }
            }
            // A left-press elsewhere, or any other mouse activity (wheel,
            // right-click, a move) while a menu is open â€” dismiss it
            // without activating anything, then let this event fall
            // through to the ordinary handling below only if it wasn't a
            // press (a press elsewhere is fully consumed by closing the
            // menu, matching standard "click away" menu behavior; a wheel/
            // move with the menu open has nothing sensible to do to the
            // panels underneath a covering dropdown either way).
            menuState = CloseMenu();
            if (isLeftPress) return;
        }
    }

    // IS-0007: computed once from the panel/command-line state as of this
    // event, matching DrawFrame's own hint-bar sizing for the frame this
    // event is handled within â€” see ComputeVisibleRows's own comment for
    // why the two must agree. A click that flips the active panel partway
    // through this function could leave later ComputeVisibleRows calls in
    // *this* call using a context computed from the panel active at entry,
    // not after; harmless (ComputeVisibleRows always returns a safe, >=1
    // value, and the very next frame recomputes fresh), so not worth
    // re-deriving mid-function.
    HintContext hintContext = GetHintContext(leftActive ? left : right, commandLine);
    bool hasStatusLine = !BuildStatusLine(leftActive ? left : right).empty();

    // FEA-0002 Phase 3, Â§8: a left-press or double-click landing inside a
    // panel that has installed content is forwarded to the plugin's own
    // OnMouseClick instead of the panel's usual click/drag/select handling
    // below -- mirrors the keyboard interception's "content owns this
    // panel's input" rule. Reuses HitTestPanel (UI-012's own row/side hit
    // test) rather than re-deriving panel geometry a second time; only the
    // relative X still needs computing here, the same left-edge-at-1/
    // right-edge-at-leftWidth+1 layout DrawFrame itself lays panels out at.
    if (left.Content() || right.Content()) {
        auto contentHit = HitTestPanel(mc::ComputePanelLayout(console.Width(), leftActive, gSinglePanelLayout), ComputeVisibleRows(console, hintContext, hasStatusLine),
                                       mouse.dwMousePosition.X, mouse.dwMousePosition.Y, kMenuBarRows);
        if (contentHit) {
            Panel& clickedPanel = contentHit->isLeftPanel ? left : right;
            if (clickedPanel.Content()) {
                if ((mouse.dwButtonState & (FROM_LEFT_1ST_BUTTON_PRESSED | RIGHTMOST_BUTTON_PRESSED)) ||
                    mouse.dwEventFlags == MOUSE_WHEELED)
                    leftActive = contentHit->isLeftPanel;
                const mc::PanelLayout layout = mc::ComputePanelLayout(console.Width(), leftActive, gSinglePanelLayout);
                SHORT originX = contentHit->isLeftPanel ? layout.leftX : layout.rightX;
                SHORT relativeX = static_cast<SHORT>(mouse.dwMousePosition.X - originX);
                clickedPanel.Content()->OnMouseEvent(relativeX, static_cast<SHORT>(contentHit->rowOffset),
                    mouse.dwButtonState, mouse.dwEventFlags, mouse.dwControlKeyState);
                return;
            }
        }
    }

    if (mouse.dwEventFlags == MOUSE_WHEELED) {
        // The wheel delta lives in dwButtonState's high word as a signed
        // 16-bit value: positive means scrolled toward the user (up),
        // negative away (down) â€” the opposite sign convention from
        // Panel::MoveCursor's delta (positive moves down the list).
        short wheelDelta = static_cast<short>(HIWORD(mouse.dwButtonState));
        int steps = (wheelDelta > 0) ? -3 : 3;
        Panel& target = leftActive ? left : right;
        target.MoveCursor(steps, ComputeVisibleRows(console, hintContext, hasStatusLine));
        return;
    }

    // IS-0004: right-click/drag selection, handled entirely in its own
    // block and returning before any of the left-button branches below, so
    // the two buttons' gestures can never interfere with each other.
    // Entered whenever the right button is currently held, or a right-drag
    // is already active (so its release â€” the button bit disappearing â€” is
    // still seen here rather than falling through to the left-button code).
    bool rightButtonHeld = (mouse.dwButtonState & RIGHTMOST_BUTTON_PRESSED) != 0;
    if (rightButtonHeld || selectDrag.active) {
        if (!rightButtonHeld) {
            // Released â€” every selection change already took effect live
            // as the mouse passed over each entry, so there's nothing left
            // to do but reset.
            selectDrag = SelectDragState{};
            return;
        }
        auto hit = HitTestPanel(mc::ComputePanelLayout(console.Width(), leftActive, gSinglePanelLayout), ComputeVisibleRows(console, hintContext, hasStatusLine), mouse.dwMousePosition.X,
                                mouse.dwMousePosition.Y, kMenuBarRows);
        if (!selectDrag.active) {
            // A fresh right-button press â€” only act on the actual press
            // event, matching the left button's own isPressOrDoubleClick
            // guard below.
            if (mouse.dwEventFlags != 0 && mouse.dwEventFlags != DOUBLE_CLICK) return;
            if (!hit) return;
            Panel& clicked = hit->isLeftPanel ? left : right;
            leftActive = hit->isLeftPanel;
            int previousCursor = clicked.Cursor();
            int index = clicked.TopIndex() + hit->rowOffset;
            if (index < 0 || index >= static_cast<int>(clicked.Entries().size())) return;
            int delta = index - clicked.Cursor();
            if (delta != 0) clicked.MoveCursor(delta, ComputeVisibleRows(console, hintContext, hasStatusLine));

            if (mouse.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
                // IS-0004 addendum: Ctrl+right-click selects every entry
                // between wherever the cursor already was (still sitting on
                // the previous right-click's target, since that branch also
                // moves the cursor) and this click, the same anchor model
                // Ctrl+left-click already uses (Panel::SelectRange) â€” a
                // single discrete gesture, not draggable, so this returns
                // without starting selectDrag. Uses Ctrl, not the Shift the
                // issue originally asked for: classic console host
                // (conhost.exe) silently swallows a Shift-held mouse click
                // before it ever reaches the app's input queue (confirmed
                // against both buttons, with no popup/menu appearing either
                // â€” it's not intercepted for a visible reason, it's just
                // never delivered), so Shift can't work here regardless of
                // any SetConsoleMode flag. Ctrl isn't reserved the same way.
                clicked.SelectRange(previousCursor, index);
                return;
            }

            selectDrag.active = true;
            selectDrag.isLeftPanel = hit->isLeftPanel;
            selectDrag.targetSelected = clicked.ToggleEntrySelected(index);
            selectDrag.lastIndex = index;
            return;
        }
        // Continuing a drag â€” a right-drag stays confined to the panel it
        // started in (selection is a per-panel concept, unlike copy/move's
        // deliberately cross-panel drag); moving into the other panel or
        // outside both simply has no further effect until the mouse
        // returns to the origin panel.
        if (!hit || hit->isLeftPanel != selectDrag.isLeftPanel) return;
        Panel& target = hit->isLeftPanel ? left : right;
        int index = target.TopIndex() + hit->rowOffset;
        if (index < 0 || index >= static_cast<int>(target.Entries().size()) || index == selectDrag.lastIndex) return;
        int delta = index - target.Cursor();
        if (delta != 0) target.MoveCursor(delta, ComputeVisibleRows(console, hintContext, hasStatusLine));
        target.SetEntrySelected(index, selectDrag.targetSelected);
        selectDrag.lastIndex = index;
        return;
    }

    bool isPressOrDoubleClick = mouse.dwEventFlags == 0 || mouse.dwEventFlags == DOUBLE_CLICK;
    bool leftButtonHeld = (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;

    // IS-0003/IS-0007: a click anywhere on the (possibly multi-row) bottom
    // hint bar triggers that hint's action via a synthetic key press â€” see
    // SimulateKeyPress's own comment for why. Checked first since those
    // rows sit outside both panels' geometry anyway (HitTestPanel already
    // returns nullopt there), and only while no drag is in progress (a
    // drag/drop release is handled below instead, even if it happens to
    // land on one of those rows).
    SHORT hintRowsForClick = static_cast<SHORT>(HintBarRowCount(hintContext, console.Width()));
    if (isPressOrDoubleClick && leftButtonHeld && !drag.dragging && !drag.dragCandidate &&
        mouse.dwMousePosition.Y >= static_cast<SHORT>(console.Height() - hintRowsForClick)) {
        auto action = MatchHintBarClick(console.Width(), console.Height(), hintContext, mouse.dwMousePosition.X,
                                        mouse.dwMousePosition.Y);
        if (action) SimulateKeyPress(console, *action);
        return;
    }

    if (drag.dragging) {
        if (leftButtonHeld) {
            // Still dragging â€” move the cursor in whichever panel the mouse
            // currently hovers over, as visual feedback for where a drop
            // would land.
            if (mouse.dwEventFlags & MOUSE_MOVED) {
                auto hoverHit = HitTestPanel(mc::ComputePanelLayout(console.Width(), leftActive, gSinglePanelLayout), ComputeVisibleRows(console, hintContext, hasStatusLine), mouse.dwMousePosition.X,
                                             mouse.dwMousePosition.Y, kMenuBarRows);
                if (hoverHit) {
                    Panel& hovered = hoverHit->isLeftPanel ? left : right;
                    int index = hovered.TopIndex() + hoverHit->rowOffset;
                    if (index >= 0 && index < static_cast<int>(hovered.Entries().size())) {
                        int delta = index - hovered.Cursor();
                        if (delta != 0) hovered.MoveCursor(delta, ComputeVisibleRows(console, hintContext, hasStatusLine));
                    }
                }
            }
            return;
        }
        // The button is no longer held: this is the drop. A drop back onto
        // the source panel, or outside both panels, is a no-op â€” the drag
        // state just resets either way.
        auto dropHit = HitTestPanel(mc::ComputePanelLayout(console.Width(), leftActive, gSinglePanelLayout), ComputeVisibleRows(console, hintContext, hasStatusLine), mouse.dwMousePosition.X,
                                    mouse.dwMousePosition.Y, kMenuBarRows);
        if (dropHit && dropHit->isLeftPanel != drag.sourceIsLeftPanel) {
            const Panel& sourcePanel = drag.sourceIsLeftPanel ? left : right;
            const Panel& destPanel = dropHit->isLeftPanel ? left : right;
            bool isMove = (mouse.dwControlKeyState & SHIFT_PRESSED) != 0;
            PerformCopyOrMove(console, left, right, leftActive, sourcePanel, drag.items, destPanel.Path(), isMove,
                              logger);
        }
        drag = DragState{};
        return;
    }
    if (drag.dragCandidate) {
        if (!leftButtonHeld) {
            // Released before any movement â€” was just a click, already
            // handled at press time below.
            drag = DragState{};
            return;
        }
        if (mouse.dwEventFlags & MOUSE_MOVED) {
            const Panel& src = drag.sourceIsLeftPanel ? left : right;
            std::vector<std::wstring> items = src.SelectionOrCursor();
            if (items.empty()) {
                drag = DragState{};
            } else {
                drag.dragging = true;
                drag.items = std::move(items);
            }
        }
        return;
    }

    // Only a left-button press (or its double-click variant) selects or
    // activates â€” a right-click, a wheel-button click, or any other button
    // combination is ignored rather than guessed at.
    if (!isPressOrDoubleClick || !leftButtonHeld) return;

    auto hit = HitTestPanel(mc::ComputePanelLayout(console.Width(), leftActive, gSinglePanelLayout), ComputeVisibleRows(console, hintContext, hasStatusLine), mouse.dwMousePosition.X,
                            mouse.dwMousePosition.Y, kMenuBarRows);
    if (!hit) return;

    Panel& clicked = hit->isLeftPanel ? left : right;
    leftActive = hit->isLeftPanel;

    int previousCursor = clicked.Cursor();
    int index = clicked.TopIndex() + hit->rowOffset;
    if (index < 0 || index >= static_cast<int>(clicked.Entries().size())) return;
    int delta = index - clicked.Cursor();
    if (delta != 0) clicked.MoveCursor(delta, ComputeVisibleRows(console, hintContext, hasStatusLine));

    if (mouse.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) {
        // IS-0004: Ctrl+click selects every entry between wherever the
        // cursor already was and the clicked entry (inclusive) â€” not a
        // copy/move drag candidate (see DragState's own comment for why
        // left-drag means something different), so this returns instead of
        // falling into the drag.dragCandidate logic below. Uses Ctrl, not
        // Shift (see the right-button block's own comment on why: classic
        // console host silently swallows a Shift-held mouse click before it
        // reaches the app at all, on both buttons).
        clicked.SelectRange(previousCursor, index);
        return;
    }

    // Matches Enter's own precedence: activation only applies with an
    // empty command line â€” a non-empty one means the user is mid-typing a
    // command, which a click shouldn't interrupt or run.
    if (mouse.dwEventFlags == DOUBLE_CLICK && commandLine.empty()) {
        ActivateCursorEntry(console, left, right, leftActive, config, logger, pluginManager, pendingEdits);
    } else if (mouse.dwEventFlags == 0) {
        // IS-0003: remember this press as a possible drag start, promoted
        // to an actual drag only if a MOUSE_MOVED event arrives next while
        // the button is still held (see the drag.dragCandidate branch
        // above) â€” so a plain click with no movement never becomes one.
        drag.dragCandidate = true;
        drag.sourceIsLeftPanel = hit->isLeftPanel;
    }
}

// CLI-005: opens an interactive shell in the active panel's directory,
// reusing the same console-handoff Process/Console already built for F4
// Edit (per the precedent noted on Process.{h,cpp}) rather than anything new.
void DoOpenShell(Console& console, Panel& left, Panel& right, bool leftActive) {
    const Panel& target = leftActive ? left : right;
    std::wstring shell = ResolveShellCommand();

    console.SuspendForChildProcess();
    ProcessRunResult result = RunAndWait(shell, {}, target.Path());
    console.ResumeAfterChildProcess();

    left.Refresh();
    right.Refresh();
    if (!result.started) {
        auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
        ShowMessage(console, kStrCannotStartShellTitle, {kStrShellLabel + shell, result.error}, redraw);
    }
}

// CLI-002/CLI-006: runs `commandText` via `cmd /c` in the active panel's
// directory. The whole typed line is handed to cmd.exe as a single quoted
// argument, so shell metacharacters (pipes, redirection, &&, %VARS%) are
// interpreted by cmd.exe itself â€” the same predictable behavior a user
// already gets at a normal command prompt, not a custom parser of our own.
void RunShellCommand(Console& console, Panel& left, Panel& right, bool leftActive, const std::wstring& commandText) {
    const Panel& target = leftActive ? left : right;
    std::wstring shell = ResolveShellCommand();

    console.SuspendForChildProcess();
    ProcessRunResult result = RunAndWait(shell, {L"/c", commandText}, target.Path());
    if (result.started) PauseForOutput(console);
    console.ResumeAfterChildProcess();

    left.Refresh();
    right.Refresh();
    if (!result.started) {
        auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
        ShowMessage(console, kStrCannotRunCommandTitle, {kStrShellLabel + shell, result.error}, redraw);
    }
}

void DoCopyOrMove(Console& console, Panel& left, Panel& right, bool leftActive, bool isMove, Logger& logger) {
    const Panel& source = leftActive ? left : right;
    const Panel& dest = leftActive ? right : left;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    std::vector<std::wstring> items = source.SelectionOrCursor();
    if (items.empty()) return;

    std::wstring verb = isMove ? kStrVerbMove : kStrVerbCopy;
    std::wstring header = FormatItemsToDestinationHeader(verb, items.size());
    auto destPathOpt = ShowTextPrompt(console, header, dest.Path().wstring(), redraw);
    if (!destPathOpt || destPathOpt->empty()) return;

    PerformCopyOrMove(console, left, right, leftActive, source, items, std::filesystem::path(*destPathOpt), isMove,
                      logger);
}

// FOP-019 (Shift+F5): duplicates the cursor entry under a new name. Unlike
// F5, the prompt is pre-filled with the entry's own name and a bare name or
// relative path is resolved against the *source* directory, so the copy
// lands next to the original instead of a new directory of that name being
// created to hold it. A full path is used as given; an existing directory
// (or input ending in "\") receives the copy under its original name
// (ResolveCopyAsTarget, PathUtil.h).
void DoCopyAs(Console& console, Panel& left, Panel& right, bool leftActive, Logger& logger) {
    Panel& source = leftActive ? left : right;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    const auto& entries = source.Entries();
    int cursor = source.Cursor();
    if (cursor < 0 || cursor >= static_cast<int>(entries.size())) return;
    const std::wstring name = entries[cursor].name;
    if (name == L"..") return;

    auto typed = ShowTextPrompt(console, FormatCopyAsHeader(name), name, redraw);
    if (!typed) return;
    auto target = mc::ResolveCopyAsTarget(source.Path(), name, *typed);
    if (!target) return;
    if (PathsEquivalent(*target, source.Path() / name)) {
        ShowMessage(console, FormatCannotVerbTitle(kStrVerbCopy), {kStrCopyAsSameItem}, redraw);
        return;
    }

    PerformCopyOrMove(console, left, right, leftActive, source, {name}, target->parent_path(), /*isMove=*/false,
                      logger, *target);
    // IS-0001 precedent: put the cursor on the new copy when it landed in
    // the panel's own directory.
    if (PathsEquivalent(target->parent_path(), source.Path())) {
        source.NavigateTo(source.Path(), target->filename().wstring());
    }
}

// FOP-008: whether this delete needs an explicit Yes/No confirmation is
// itself a configurable safety rule (Config::confirmRecycleBinDelete/
// confirmPermanentDelete, both default true) rather than an unconditional
// dialog â€” the gap that kept FOP-008 at Partial, since every destructive
// action previously confirmed unconditionally with no way to tune that.
bool ConfirmDelete(Console& console, const Panel& left, const Panel& right, bool leftActive,
                   const std::vector<std::wstring>& items, bool permanent, const Config& config, bool background);

// FOP-014 completion: Alt+F8 / Alt+Shift+F8 queue a Recycle Bin / permanent
// delete as a background job. Same confirmation as F8/Shift+F8 (D7); the job
// then waits behind any job using the same files (OperationQueue::NextToRun).
void QueueDelete(Console& console, const Panel& left, const Panel& right, bool leftActive, bool permanent,
                 const Config& config, JobWorker& jobs) {
    const Panel& source = leftActive ? left : right;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    if (source.Content()) {
        ShowMessage(console, kStrJobsTitle, {kStrQueueDeletePluginRefused}, redraw);
        return;
    }
    std::vector<std::wstring> items = source.SelectionOrCursor();
    if (items.empty()) return;
    if (!ConfirmDelete(console, left, right, leftActive, items, permanent, config, /*background=*/true)) return;
    Job job;
    job.kind = JobKind::Delete;
    job.permanent = permanent;
    job.sources = ItemPaths(source.Path(), items);
    jobs.Enqueue(std::move(job));
}

// FOP-008: the one delete confirmation, shared by F8/Shift+F8 and the queued
// Alt+F8/Alt+Shift+F8 (FOP-014 D7) -- the same config switches decide
// whether to ask, so a background delete never skips a confirmation an
// immediate one would show. True if the delete may go ahead.
bool ConfirmDelete(Console& console, const Panel& left, const Panel& right, bool leftActive,
                   const std::vector<std::wstring>& items, bool permanent, const Config& config, bool background) {
    bool needsConfirmation = permanent ? config.confirmPermanentDelete : config.confirmRecycleBinDelete;
    if (!needsConfirmation) return true;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    std::vector<std::wstring> lines;
    if (items.size() == 1) {
        lines.push_back(L"\"" + items[0] + L"\"");
    } else {
        lines.push_back(FormatItemCount(items.size()));
    }
    lines.push_back(permanent ? kStrWillBePermanentlyDeleted : kStrWillBeSentToRecycleBin);
    std::wstring title = permanent ? kStrPermanentlyDeleteTitle : kStrDeleteTitle;
    if (background) title += kStrInTheBackgroundSuffix;
    int choice = ShowChoiceDialog(console, title, lines, {{L'Y', kStrYesLabel}, {L'N', kStrNoLabel}}, redraw);
    return choice == 0;
}

void DoDelete(Console& console, Panel& left, Panel& right, bool leftActive, bool permanent, const Config& config,
             Logger& logger) {
    Panel& target = leftActive ? left : right;
    Panel& other = leftActive ? right : left;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    std::vector<std::wstring> items = target.SelectionOrCursor();
    if (items.empty()) return;
    if (RefuseIfBusyWithJob(console, left, right, leftActive, ItemPaths(target.Path(), items))) return;
    if (!ConfirmDelete(console, left, right, leftActive, items, permanent, config, /*background=*/false)) return;

    // PER-002: live per-item progress plus a prompt Esc-cancel, the same
    // animated dialog Copy/Move use. IS-0018: both the permanent and the
    // Recycle Bin path delete item by item, so F8 shows it too, rather than
    // freezing the screen until one batched shell call returns.
    ProgressUi ui{console, left, right, leftActive, kStrVerbDeleting};
    ui.primaryPath = target.Path().wstring();
    auto onItemStart = [&](const std::wstring& n, int done, int total) { return ui.OnItemStart(n, done, total); };
    auto cancelPoll = [&]() { return ui.PollCancel(); };
    auto onError = [&](const std::wstring& n, const std::wstring& msg, DWORD win32Error) {
        return AskFileError(console, left, right, leftActive, n, msg, win32Error);
    };
    // SEC-003: see DoCopyOrMove's onElevateCopy/onElevateMove for the shape;
    // only reached for an access-denied failure the user chose to elevate.
    // Permanent delete only â€” DeleteItems never offers it for the Recycle
    // Bin path.
    auto onElevateDelete = [&](const std::filesystem::path& path, std::wstring& err) {
        logger.Log(LogLevel::Warning, L"Elevating delete: \"" + path.wstring() + L"\"");
        DWORD result = RunElevated(ElevatedRequest{ElevatedAction::DeletePermanent, path});
        if (result == ERROR_SUCCESS) return true;
        err = FormatWinError(result);
        logger.Log(LogLevel::Error, L"Elevated delete failed: \"" + path.wstring() + L"\": " + err);
        return false;
    };
    OperationOutcome outcome =
        DeleteItems(target.Path(), items, permanent, onItemStart, cancelPoll, onError, onElevateDelete);
    target.Refresh();
    SyncOtherPanelIfSamePath(target, other);
    ReportOutcome(console, left, right, leftActive, kStrVerbDelete, outcome, logger);
}

// CON-005: lets the user toggle a Windows file attribute (read-only, hidden,
// system, or archive) on the selection (or cursor entry) â€” the missing piece
// that kept CON-005 at Partial, since attributes were previously only ever
// touched incidentally (clearing read-only before a permanent delete).
void DoChangeAttributes(Console& console, Panel& left, Panel& right, bool leftActive, Logger& logger) {
    Panel& target = leftActive ? left : right;
    Panel& other = leftActive ? right : left;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    std::vector<std::wstring> items = target.SelectionOrCursor();
    if (items.empty()) return;
    if (RefuseIfBusyWithJob(console, left, right, leftActive, ItemPaths(target.Path(), items))) return;  // FOP-014

    std::wstring header = items.size() == 1 ? L"\"" + items[0] + L"\"" : std::to_wstring(items.size()) + L" item(s)";
    int choice = ShowChoiceDialog(console, FormatToggleAttributeOnHeader(header), {kStrEachItemsCurrentStateFlipped},
                                  {{L'R', kStrReadonlyLabel}, {L'H', kStrHiddenLabel}, {L'S', kStrSystemLabel},
                                   {L'A', kStrArchiveLabel}, {L'C', kStrCancelLabel}},
                                  redraw);

    FileAttributeFlag flag;
    switch (choice) {
        case 0:
            flag = FileAttributeFlag::ReadOnly;
            break;
        case 1:
            flag = FileAttributeFlag::Hidden;
            break;
        case 2:
            flag = FileAttributeFlag::System;
            break;
        case 3:
            flag = FileAttributeFlag::Archive;
            break;
        default:
            return;
    }

    // ERR-003: offers Retry/Skip/Skip all/Cancel for a failure, same as
    // copy/move/delete (FOP-010) â€” the missing piece that kept ERR-003 at
    // Partial, since attribute toggling was the one multi-item operation
    // that just recorded a failure and moved on with no way to retry it.
    // PER-002: live per-item progress plus a prompt Esc-cancel, same as
    // Copy/Move/Delete â€” a large selection no longer toggles with a frozen
    // screen and no way to interrupt it.
    ProgressUi ui{console, left, right, leftActive, kStrVerbSettingAttributes};
    ui.primaryPath = target.Path().wstring();
    auto onItemStart = [&](const std::wstring& n, int done, int total) { return ui.OnItemStart(n, done, total); };
    auto onError = [&](const std::wstring& n, const std::wstring& msg, DWORD win32Error) {
        return AskFileError(console, left, right, leftActive, n, msg, win32Error);
    };
    // SEC-003: see DoCopyOrMove's onElevateCopy/onElevateMove for the shape.
    auto onElevateAttribute = [&](const std::filesystem::path& path, FileAttributeFlag f, std::wstring& err) {
        logger.Log(LogLevel::Warning, L"Elevating attribute toggle: \"" + path.wstring() + L"\"");
        DWORD result = RunElevated(ElevatedRequest{ElevatedAction::ToggleAttribute, path, {}, false, f});
        if (result == ERROR_SUCCESS) return true;
        err = FormatWinError(result);
        logger.Log(LogLevel::Error, L"Elevated attribute toggle failed: \"" + path.wstring() + L"\": " + err);
        return false;
    };
    OperationOutcome outcome =
        ToggleAttributes(target.Path(), items, flag, onItemStart, onError, onElevateAttribute);
    target.Refresh();
    SyncOtherPanelIfSamePath(target, other);
    ReportOutcome(console, left, right, leftActive, kStrVerbToggleAttribute, outcome, logger);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // SEC-003: checked before anything else â€” Console, Config, Session,
    // CommandHistory, the log file â€” so a hidden elevated-helper invocation
    // (spawned only by RunElevated, in an already-elevated child process)
    // never touches any of that. Performs exactly the one requested
    // operation and exits immediately with its Win32 result as the exit
    // code; every other invocation (the normal, unprivileged app) falls
    // through unaffected.
    if (auto elevatedResult = RunAsElevatedHelperIfRequested(argc, argv)) {
        return static_cast<int>(*elevatedResult);
    }

    if (argc == 2 && (std::wcscmp(argv[1], L"--version") == 0 || std::wcscmp(argv[1], L"-v") == 0)) {
        wprintf(L"MyCommander %S\n", MC_VERSION_STR);
        return 0;
    }

    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    std::filesystem::path leftStart = std::filesystem::current_path();
    std::filesystem::path rightStart = std::filesystem::current_path();
    const SessionState previousSession = LoadSession();
    // UI-013 Part A: the layout comes back even when paths are given below.
    gSinglePanelLayout = previousSession.singlePanel;
    if (argc >= 2) {
        // Explicit command-line paths are intentional and take precedence
        // over a cosmetic clean-exit session restore.
        leftStart = argv[1];
        if (argc >= 3) rightStart = argv[2];
        else rightStart = argv[1];
    } else {
        if (previousSession.leftPath) leftStart = *previousSession.leftPath;
        if (previousSession.rightPath) rightStart = *previousSession.rightPath;
    }
    leftStart = ResolveStartPath(leftStart);
    rightStart = ResolveStartPath(rightStart);

    // FOP-008/CFG-*: a minimal per-user config file (%APPDATA%\MyCommander\
    // mycommander.ini) holding the destructive-operation confirmation
    // toggles; a fresh, fully-commented default is written on first run so
    // the settings are discoverable without already knowing the key names.
    //
    // ERR-006/NAV-008: Config remains preferences-only. The separate session
    // file above has only two cosmetic panel paths and is written exclusively
    // below after the normal input loop exits; it contains no operation,
    // command, selection, or other state a crash could cause to be replayed.
    EnsureConfigFileExists();
    auto loadedConfig = LoadConfig();
    Config config = loadedConfig.config;

    // UI-005: set once here, before the input loop starts, and never
    // modified again â€” see gVisibleColumns' own comment for why this is
    // module state rather than threaded through DrawFrame's every call site.
    gVisibleColumns = config.visibleColumns;
    gTypeColumnWidth = config.typeColumnWidth;
    gMtimeColumnWidth = config.mtimeColumnWidth;
    gSizeColumnWidth = config.sizeColumnWidth;

    // UI-010: same one-time-at-startup treatment as the column settings
    // above.
    ApplyThemePalette(config.colorTheme == ColorTheme::HighContrast ? kHighContrastThemePalette
                                                                     : kDefaultThemePalette);

    // ERR-004: a process-wide logger, on only if the user opted in via
    // Config::logLevel (default Off). ERR-005: every Log() call site below
    // passes only paths, Win32/error text, or setting names â€” never typed
    // command text, file content, or credentials.
    Logger logger(config.logLevel);
    {
        wchar_t startupMsg[64];
        swprintf_s(startupMsg, L"MyCommander %S starting", MC_VERSION_STR);
        logger.Log(LogLevel::Info, startupMsg);
    }

    // IS-0006: self-healing config file â€” a key EnsureConfigFileExists()
    // wouldn't have written (because the file already existed, from before
    // that key was introduced) gets appended now, with its own documented
    // default, so the file on disk stays complete rather than silently
    // relying on an in-memory default it never mentions. Startup-only â€”
    // see ReloadConfigLive's own comment for why a live reload doesn't
    // repeat this.
    auto addedConfigKeys = AppendMissingConfigKeys(ConfigFilePath(), loadedConfig.presentKeys);
    if (!addedConfigKeys.empty()) {
        logger.Log(LogLevel::Info, L"Config: added " + std::to_wstring(addedConfigKeys.size()) +
                                    L" missing setting(s) to " + ConfigFilePath().wstring());
    }

    Console console;
    // IS-0009: reuses the exact same version-string formatting the startup
    // log line above (and --version) already use -- one source of truth
    // for "what the current version looks like as text."
    {
        wchar_t title[64];
        swprintf_s(title, L"MyCommander - %S", MC_VERSION_STR);
        console.SetTitle(title);
    }
    console.SetUseBasicSymbols(config.useBasicSymbols);
    // LIST-008: main-screen panels enumerate in bounded batches so a large
    // or slow directory does not monopolize the console input loop.
    Panel left(leftStart, config.groupDirectoriesFirst, /*incrementalLoading=*/true);
    Panel right(rightStart, config.groupDirectoriesFirst, /*incrementalLoading=*/true);
    bool leftActive = true;
    // UI-013, Part B: one DirectoryTreeContent per side, entered fresh on
    // Ctrl+T and left (SetContent(nullptr)) on Enter/Esc from inside it --
    // nothing here needs to outlive a single tree session.
    DirectoryTreeContent leftTreeContent;
    DirectoryTreeContent rightTreeContent;
    // FOP-014, Phase 3: background copy/move jobs. Declared after the panels, so
    // it is destroyed first and its worker threads finish before the panels go away.
    JobWorker jobs;
    JobsWatch jobsWatch;
    gJobWorker = &jobs;  // FOP-014: lets synchronous operations refuse files a job is using
    // Cleared again before `jobs` is destroyed (declared after it, so it is
    // destroyed first): the global can never point at a dead JobWorker.
    struct JobWorkerRegistration {
        ~JobWorkerRegistration() { gJobWorker = nullptr; }
    } jobWorkerRegistration;

    if (!loadedConfig.warnings.empty()) {
        auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
        std::vector<std::wstring> lines{kStrUsingDefaultForSettings};
        lines.insert(lines.end(), loadedConfig.warnings.begin(), loadedConfig.warnings.end());
        ShowMessage(console, kStrConfigurationWarningTitle, lines, redraw);
        for (const auto& warning : loadedConfig.warnings) {
            logger.Log(LogLevel::Warning, L"Config: " + warning);
        }
    }

    // FEA-0009: first run writes the commented sample file-actions.conf
    // (CFG-001); loading it now surfaces any skipped-line warnings at
    // startup, like the config's own. Later edits reload lazily.
    mc::EnsureFileActionsFileExistsAt(mc::FileActionsFilePath());
    CurrentFileActions(console, left, right, leftActive, logger);

    // Internal command line (CLI-002/003/004): loaded from CommandHistory.h's
    // persisted file when Config::persistCommandHistory is on (the default),
    // so recall via Ctrl+Up/Down can reach commands from a previous run, not
    // just this session. CLI-007's "excluded from persistent history"
    // leading-space opt-out still skips adding a command to this in-memory
    // list at all, so it's never written out either.
    std::wstring commandLine;
    // IS-0015 Part 3: a real positionable cursor within `commandLine`,
    // alongside the existing string rather than replacing it with a
    // LineEdit outright -- `commandLine` is read in many other places
    // (DrawFrame, GetHintContext, ComputeVisibleRows, HandleMouseEvent, ...)
    // that only ever care whether it's empty, not where the cursor is.
    size_t commandLineCursor = 0;
    std::vector<std::wstring> commandHistory = config.persistCommandHistory ? LoadCommandHistory() : std::vector<std::wstring>{};
    int historyIndex = -1;  // -1 = not currently browsing history
    // IS-0002: files currently open in a background (non-blocking) F4
    // editor session, being watched for changes â€” see PollPendingEdits.
    std::vector<PendingEdit> pendingEdits;
    // IS-0003: cross-iteration drag-and-drop state â€” see DragState's own comment.
    DragState drag;
    // IS-0004: cross-iteration right-click/drag selection state â€” see SelectDragState's own comment.
    SelectDragState selectDrag;
    // IS-0006: cross-iteration config-file-edit watch state â€” see ConfigWatch's own comment.
    ConfigWatch configWatch;
    // FEA-0005: cross-iteration top-menu-bar state â€” see MenuBarState's own comment.
    MenuBarState menuState;

    // FEA-0002 Phase 1: discovers and activates every *.dll in a Plugins
    // folder next to this executable (not %APPDATA% -- plugins install
    // alongside the app, config/session/log data stays per-user).
    // SetAppContext must run before DiscoverAndLoad, since a plugin's own
    // McPlugin_Init may immediately call back into a dialog/theme/panel-
    // query host function.
    PluginManager pluginManager;
    gPluginManager = &pluginManager;  // EXT-003: viewer plugins for F3 / Shift+F3
    pluginManager.SetAppContext(console, left, right, leftActive,
                               [&]() { DrawFrame(console, left, right, leftActive); });
    RegisterThemeChangeListener(
        [&pluginManager](const ThemePalette& palette) { pluginManager.NotifyThemeChanged(palette); });
    // KEY-005: before plugins load, so their shortcuts are registered under
    // keys.conf's bindings and checked against the final built-in keys.
    mc::ActiveHintKeyMap() = &gKeys.map;
    ReloadKeyBindings(&console, left, right, leftActive, logger, pluginManager);
    {
        wchar_t exePath[MAX_PATH];
        DWORD exePathLen = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (exePathLen > 0 && exePathLen < MAX_PATH) {
            std::filesystem::path pluginsDir = std::filesystem::path(exePath).parent_path() / L"Plugins";
            std::set<std::wstring> disabledIds(config.disabledPluginIds.begin(), config.disabledPluginIds.end());
            pluginManager.DiscoverAndLoad(pluginsDir, disabledIds, logger);
        }
    }
    gMergedMenus = pluginManager.MergedMenus();
    ApplyMenuKeyTexts(gMergedMenus, pluginManager);

    bool running = true;
    while (running) {
        left.PumpRefresh();
        right.PumpRefresh();
        PollPendingEdits(pendingEdits, left, right);
        PollConfigWatch(configWatch, config, console, left, right, leftActive, logger);
        ServiceJobPrompts(console, left, right, leftActive, jobs);
        PollBackgroundJobs(console, left, right, leftActive, jobs, jobsWatch);
        // FEA-0002: kept fresh every iteration rather than only after
        // DiscoverAndLoad -- a plugin's own OnMenuAction/OnShortcut/
        // McPlugin_Init could still add/remove/enable a menu item at any
        // time, not only during its own initial load.
        PollKeyBindings(console, left, right, leftActive, logger, pluginManager);  // KEY-005
        gMergedMenus = pluginManager.MergedMenus();
        ApplyMenuKeyTexts(gMergedMenus, pluginManager);
        if (IsTerminalTooSmall(console)) {
            DrawTooSmallMessage(console);
        } else {
            DrawFrame(console, left, right, leftActive, commandLine, menuState, commandLineCursor);
            // IS-0003: a low-cost visual cue that a drag is in progress â€”
            // the console has no drag "ghost" rendering, so without this
            // the gesture would otherwise be entirely invisible.
            if (drag.dragging) {
                console.PutText(0, static_cast<SHORT>(console.Height() - 1),
                                PadOrTrim(FormatDragHint(drag.items.size()), console.Width()), kAttrStatusBar);
            }
        }
        console.Present();

        // A short wait, rather than a blocking ReadConsoleInputW, gives each
        // loading panel regular time slices even while the user is idle.
        if (WaitForSingleObject(console.InputHandle(), 16) == WAIT_TIMEOUT) continue;

        INPUT_RECORD record;
        DWORD read = 0;
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;

        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            console.UpdateSize();
            continue;
        }
        if (record.EventType == MOUSE_EVENT) {
            // UI-006: same "too small to interact with" guard the key
            // handling below applies â€” a mouse click can't do anything
            // meaningful once the too-small message has replaced the panels.
            if (!IsTerminalTooSmall(console)) {
                HandleMouseEvent(console, left, right, leftActive, commandLine, config, logger, pluginManager,
                                 pendingEdits, drag, selectDrag, menuState, record.Event.MouseEvent);
            }
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

        // UI-006: while below the minimum size, only quitting is honored â€”
        // everything else (dialogs, the command line, cursor movement) needs
        // more room than the too-small screen has to draw correctly.
        if (IsTerminalTooSmall(console)) {
            const auto& keyEvent = record.Event.KeyEvent;
            mc::KeyChord pressed{keyEvent.wVirtualKeyCode,
                                 (keyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0,
                                 (keyEvent.dwControlKeyState & SHIFT_PRESSED) != 0,
                                 (keyEvent.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0};
            if (keyEvent.wVirtualKeyCode == VK_F10 || gKeys.map.CommandFor(pressed) == mc::CommandId::Quit)
                running = false;
            continue;
        }

        Panel& target = leftActive ? left : right;
        int visibleRows =
            ComputeVisibleRows(console, GetHintContext(target, commandLine), !BuildStatusLine(target).empty());
        bool shiftHeld = (record.Event.KeyEvent.dwControlKeyState & SHIFT_PRESSED) != 0;
        bool ctrlHeld = (record.Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        bool altHeld = (record.Event.KeyEvent.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
        // IS-0017 Part 5: while a directory tree is open on one side, the
        // other side may not enter a folder (Enter) or go to its parent
        // (Backspace). Every other function there keeps working.
        const bool otherSideTreeOpen = leftActive ? (right.Content() == &rightTreeContent)
                                                  : (left.Content() == &leftTreeContent);
        // IS-0015 Part 3: inserts at the cursor (and advances it), not
        // always at the end -- the one shared ApplyLineEditKey primitive,
        // same as ShowTextPrompt/ShowFormDialog's own Text fields.
        auto appendToCommandLine = [&](wchar_t c) {
            LineEdit edit{commandLine, commandLineCursor};
            if (ApplyLineEditKey(edit, 0, c)) {
                commandLine = edit.text;
                commandLineCursor = edit.cursor;
                historyIndex = -1;
            }
        };
        WORD key = record.Event.KeyEvent.wVirtualKeyCode;

        // FEA-0005: the menu bar takes exclusive control of key handling
        // while a dropdown is open (the same "modal while showing" shape
        // Dialog.h's own input loops already have), and Alt+<mnemonic>
        // opens one from the closed state â€” both checked before anything
        // else so they take priority over the switch below. Matched
        // against `key` (wVirtualKeyCode), not UnicodeChar: a letter's
        // virtual key code is always its uppercase ASCII value regardless
        // of Alt/Shift state, the same thing every existing Ctrl+<letter>
        // case in the switch below already relies on, and UnicodeChar is
        // unreliable while Alt is held.
        const std::vector<TopMenu>& menus = gMergedMenus;
        if (menuState.active) {
            const TopMenu& openMenu = menus[static_cast<size_t>(menuState.openMenuIndex)];
            switch (key) {
                case VK_LEFT:
                    menuState = MoveToAdjacentTopMenu(menuState, menus, -1);
                    break;
                case VK_RIGHT:
                    menuState = MoveToAdjacentTopMenu(menuState, menus, 1);
                    break;
                case VK_UP:
                    menuState = MoveHighlight(menuState, menus, -1);
                    break;
                case VK_DOWN:
                    menuState = MoveHighlight(menuState, menus, 1);
                    break;
                case VK_ESCAPE:
                    menuState = CloseMenu();
                    break;
                case VK_RETURN:
                    ExecuteMenuItem(console, config, left, right, leftActive, logger, pluginManager,
                                    openMenu.items[static_cast<size_t>(menuState.highlightedItemIndex)]);
                    menuState = CloseMenu();
                    break;
                default: {
                    // Alt+<letter> is always tried as a top-menu switch
                    // first â€” before the open menu's own item mnemonics â€”
                    // so a menu whose own item happens to share a letter
                    // with a *different* top menu (e.g. Settings is E, and
                    // File has its own "Generate Check Code" item at E) can
                    // never shadow switching to that other menu. A bare
                    // letter (no Alt) always means "choose this item in the
                    // menu that's already open" instead.
                    if (!mc::IsMnemonicKey(key)) break;  // F-keys, numpad, ... alias letters
                    wchar_t ch = static_cast<wchar_t>(key);
                    if (altHeld) {
                        int topIdx = FindTopMenuByMnemonic(menus, ch);
                        if (topIdx >= 0) {
                            menuState = OpenMenu(topIdx);
                            break;
                        }
                    }
                    int itemIdx = FindItemByMnemonic(openMenu, ch);
                    if (itemIdx >= 0) {
                        ExecuteMenuItem(console, config, left, right, leftActive, logger, pluginManager,
                                        openMenu.items[static_cast<size_t>(itemIdx)]);
                        menuState = CloseMenu();
                    }
                    break;
                }
            }
            continue;
        }
        if (altHeld && mc::IsMnemonicKey(key)) {
            int topIdx = FindTopMenuByMnemonic(menus, static_cast<wchar_t>(key));
            if (topIdx >= 0) {
                menuState = OpenMenu(topIdx);
                continue;
            }
        }

        // KEY-005: the user's keys.conf bindings. A rebound key becomes its
        // command's default key, so the switch below (and a plugin or tree
        // panel, which use the same commander keys) runs that command; a
        // default key the user moved away does nothing on the main screen.
        // Injected keys (menus, hint clicks, a plugin's SimulateKey) are
        // already default keys and skip this; so do the command line's own
        // editing keys while it has text, which must keep editing it.
        {
            mc::KeyChord pressed{key, ctrlHeld, shiftHeld, altHeld};
            bool editingKey = !commandLine.empty() && !ctrlHeld &&
                              (key == VK_BACK || key == VK_DELETE || key == VK_HOME || key == VK_END ||
                               key == VK_ESCAPE || key == VK_RETURN || ((key == VK_LEFT || key == VK_RIGHT) && !altHeld));
            if (!mc::ConsumeInjectedKey(pressed) && !editingKey) {
                mc::KeyTranslation translated = gKeys.map.Translate(pressed, target.Content() != nullptr);
                if (translated.route == mc::KeyRoute::Dropped) continue;
                if (translated.route == mc::KeyRoute::Remapped) {
                    key = translated.chord.vk;
                    ctrlHeld = translated.chord.ctrl;
                    shiftHeld = translated.chord.shift;
                    altHeld = translated.chord.alt;
                    auto& keyEvent = record.Event.KeyEvent;
                    keyEvent.wVirtualKeyCode = key;
                    keyEvent.dwControlKeyState = (ctrlHeld ? LEFT_CTRL_PRESSED : 0) | (shiftHeld ? SHIFT_PRESSED : 0) |
                                                 (altHeld ? LEFT_ALT_PRESSED : 0);
                    keyEvent.uChar.UnicodeChar = key == VK_ADD ? L'+' : key == VK_SUBTRACT ? L'-'
                                                 : key == VK_MULTIPLY ? L'*' : 0;
                }
            }
        }

        // FEA-0002 Phase 3, Â§8: once a plugin has installed content on the
        // active panel, that panel's own body-level input (everything the
        // switch below would otherwise do -- navigation, selection, file
        // operations) is handed to the plugin instead. Tab (switch active
        // panel) and F10 (quit) are cross-panel/whole-app chrome, not this
        // one panel's own content, so they still work unconditionally
        // regardless; menu mnemonics/Alt+<letter> were already handled
        // above this point. Panel navigation history (Alt+Left/Right) is
        // deliberately not carved out as a further exception -- with
        // content covering the body, there is nothing on screen for a
        // history jump to visibly affect anyway, so it goes to the plugin
        // like any other key rather than adding a second silent exception.
        if (target.Content() && key != VK_TAB && key != VK_F10 && key != VK_F1) {
            target.Content()->OnKey(key, record.Event.KeyEvent.dwControlKeyState,
                                    record.Event.KeyEvent.uChar.UnicodeChar);
            continue;
        }

        // TST-005: navigation keys' decisions (Tab/panel history/command
        // history vs. cursor movement/Backspace) are classified by the
        // pure, independently-tested ClassifyNavigationKey â€” each case
        // below only carries out the intent, still owning any state
        // mutation (historyIndex, commandLine) that's local to this loop.
        NavKeyContext navCtx{ctrlHeld, altHeld, commandLine.empty(), !commandHistory.empty(), historyIndex != -1};
        // FEA-0002 §12 / KEY-005: plugin shortcuts first. RegisterShortcut
        // refuses every built-in key, so this can't shadow one -- but some
        // cases below ignore modifiers (Shift+F9 still opens the shell), so a
        // plugin given such a key in keys.conf would otherwise never fire.
        if (pluginManager.DispatchShortcut(key, ctrlHeld, shiftHeld, altHeld)) continue;
        switch (key) {
            case VK_TAB:
                if (ClassifyNavigationKey(key, navCtx) == NavIntent::SwitchActivePanel) leftActive = !leftActive;
                break;
            case VK_LEFT:
                // IS-0015 Part 3: a plain Left/Right with command-line text
                // moves its cursor instead -- Alt+Left/Right (panel
                // history) is untouched, matching ClassifyNavigationKey's
                // own existing altHeld-only gate for that.
                if (!altHeld && !commandLine.empty()) {
                    LineEdit edit{commandLine, commandLineCursor};
                    ApplyLineEditKey(edit, key, 0);
                    commandLineCursor = edit.cursor;
                    break;
                }
                if (ClassifyNavigationKey(key, navCtx) == NavIntent::PanelHistoryBack) target.NavigateBack();
                break;
            case VK_RIGHT:
                if (!altHeld && !commandLine.empty()) {
                    LineEdit edit{commandLine, commandLineCursor};
                    ApplyLineEditKey(edit, key, 0);
                    commandLineCursor = edit.cursor;
                    break;
                }
                if (ClassifyNavigationKey(key, navCtx) == NavIntent::PanelHistoryForward) target.NavigateForward();
                break;
            case VK_UP:
                // IS-0015: Shift+Up toggles selection as the cursor moves --
                // only when the command line is empty, the same gate
                // ClassifyNavigationKey already applies internally between
                // CursorUp and command-history browsing.
                if (shiftHeld && commandLine.empty()) {
                    target.ToggleSelectionAndMove(-1, visibleRows);
                    break;
                }
                switch (ClassifyNavigationKey(key, navCtx)) {
                    case NavIntent::CommandHistoryUp:
                        // CLI-004: browse older commands without disturbing the file cursor.
                        if (historyIndex == -1) historyIndex = static_cast<int>(commandHistory.size()) - 1;
                        else if (historyIndex > 0) --historyIndex;
                        commandLine = commandHistory[historyIndex];
                        commandLineCursor = commandLine.size();  // IS-0015 Part 3
                        break;
                    case NavIntent::CursorUp:
                        target.MoveCursor(-1, visibleRows);
                        break;
                    default:
                        break;
                }
                break;
            case VK_DOWN:
                // IS-0015: Shift+Down toggles selection as the cursor moves.
                if (shiftHeld && commandLine.empty()) {
                    target.ToggleSelectionAndMove(1, visibleRows);
                    break;
                }
                switch (ClassifyNavigationKey(key, navCtx)) {
                    case NavIntent::CommandHistoryDown:
                        if (historyIndex + 1 < static_cast<int>(commandHistory.size())) {
                            ++historyIndex;
                            commandLine = commandHistory[historyIndex];
                        } else {
                            historyIndex = -1;
                            commandLine.clear();
                        }
                        commandLineCursor = commandLine.size();  // IS-0015 Part 3
                        break;
                    case NavIntent::CursorDown:
                        target.MoveCursor(1, visibleRows);
                        break;
                    default:
                        break;
                }
                break;
            case VK_PRIOR:
                target.MoveCursor(-visibleRows, visibleRows);
                break;
            case VK_NEXT:
                target.MoveCursor(visibleRows, visibleRows);
                break;
            case VK_HOME:
                // IS-0015 Part 3: Home/End already had an unconditional
                // panel-cursor meaning (unlike Left/Right, which were free)
                // -- given the dual meaning the same way Backspace/Delete
                // already have one, gated on the command line's own state.
                if (!commandLine.empty()) {
                    commandLineCursor = 0;
                    break;
                }
                target.MoveCursor(-static_cast<int>(target.Entries().size()), visibleRows);
                break;
            case VK_END:
                if (!commandLine.empty()) {
                    commandLineCursor = commandLine.size();
                    break;
                }
                target.MoveCursor(static_cast<int>(target.Entries().size()), visibleRows);
                break;
            case VK_RETURN:
                if (altHeld) {
                    // FEA-0003: Alt+Enter, Explorer's own "show properties"
                    // convention â€” checked first, taking priority over
                    // every other VK_RETURN meaning below the same way
                    // Ctrl+Enter already does.
                    DoShowFileProperties(console, left, right, leftActive);
                } else if (ctrlHeld) {
                    // CLI-003: insert the active/selected name(s), or with
                    // Shift held, the active path, without running anything.
                    InsertActiveReference(target, /*fullPath=*/shiftHeld, commandLine);
                    commandLineCursor = commandLine.size();  // IS-0015 Part 3 -- always appended at the end
                    historyIndex = -1;
                } else if (shiftHeld && commandLine.empty() && CursorFilePath(target)) {
                    // FEA-0009: Shift+Enter on a file runs its default "open"
                    // action. With text in the command line it still runs the
                    // command (next branch); on a directory it's plain Enter.
                    DoOpenAction(console, left, right, leftActive, logger);
                } else if (!commandLine.empty()) {
                    // CLI-007: a leading space opts this command out of the history,
                    // whether it turns out to be a navigation (NAV-003) or a command
                    // (CLI-002).
                    if (commandLine.front() != L' ') {
                        commandHistory.push_back(commandLine);
                        if (commandHistory.size() > kMaxCommandHistoryEntries) commandHistory.erase(commandHistory.begin());
                    }
                    historyIndex = -1;
                    // NAV-003: a typed/pasted path that names an existing directory
                    // navigates the active panel there instead of being run as a
                    // shell command.
                    if (!TryNavigateToTypedPath(target, commandLine)) {
                        RunShellCommand(console, left, right, leftActive, commandLine);
                    }
                    commandLine.clear();
                    commandLineCursor = 0;  // IS-0015 Part 3
                } else {
                    const auto& entries = target.Entries();
                    int cursor = target.Cursor();
                    bool cursorIsFolder = cursor >= 0 && cursor < static_cast<int>(entries.size()) &&
                                          entries[static_cast<size_t>(cursor)].isDirectory;
                    if (!(otherSideTreeOpen && cursorIsFolder)) {
                        ActivateCursorEntry(console, left, right, leftActive, config, logger, pluginManager,
                                            pendingEdits);
                    }
                }
                break;
            case VK_BACK:
                if (ClassifyNavigationKey(key, navCtx) == NavIntent::ClearCommandLineChar) {
                    // IS-0015 Part 3: erases the character before the
                    // cursor (which may not be the last character anymore),
                    // not always the last one.
                    LineEdit edit{commandLine, commandLineCursor};
                    ApplyLineEditKey(edit, key, 0);
                    commandLine = edit.text;
                    commandLineCursor = edit.cursor;
                    historyIndex = -1;
                } else if (!otherSideTreeOpen) {
                    target.GoToParent();
                }
                break;
            case VK_INSERT:
                target.ToggleCursorSelection(visibleRows);
                break;
            case VK_ADD:
                if (ctrlHeld) {
                    target.SelectAll();
                } else {
                    DoSelectByMask(console, left, right, leftActive, /*select=*/true);
                }
                break;
            case VK_SUBTRACT:
                if (ctrlHeld) {
                    target.ClearSelection();
                } else {
                    DoSelectByMask(console, left, right, leftActive, /*select=*/false);
                }
                break;
            case VK_MULTIPLY:
                target.InvertSelection();
                break;
            case VK_F1:
                if (altHeld) {
                    DoSelectDrive(console, left, right, leftActive, /*selectLeftPanel=*/true);
                } else if (ctrlHeld) {
                    // UI-013 Part A: the active panel alone, full width. Tab
                    // switches which panel is shown; the hidden one keeps its
                    // folder, selection and content (and stays the F5/F6 target).
                    gSinglePanelLayout = !gSinglePanelLayout;
                } else {
                    // KEY-004: plain F1 (no modifier is otherwise bound to it) opens the
                    // in-application shortcut reference. Like DoView/DoEdit, no manual
                    // redraw here â€” the main loop's next iteration redraws the frame.
                    ShowShortcutReference(console, pluginManager.CollectHelpContent(), &gKeys.map);
                }
                break;
            case VK_F2:
                if (altHeld) {
                    DoSelectDrive(console, left, right, leftActive, /*selectLeftPanel=*/false);
                } else if (shiftHeld && ctrlHeld) {
                    DoSynchronizeDirectories(console, left, right, leftActive, logger);  // SRC-007
                } else if (shiftHeld) {
                    // SRC-006; F5 in its results list continues into SRC-007's sync.
                    if (auto preset = DoCompareDirectories(console, left, right, leftActive, config))
                        DoSynchronizeDirectories(console, left, right, leftActive, logger, preset);
                } else {
                    DoRename(console, left, right, leftActive);
                }
                break;
            case VK_F3:
                if (altHeld) {
                    // SRC-008: File > Compare contents; Shift forces the built-in viewer.
                    DoCompareFiles(console, left, right, leftActive, config, /*forceBuiltin=*/shiftHeld);
                } else if (ctrlHeld) {
                    DoSort(target, SortKey::Name);
                } else if (shiftHeld) {
                    DoChooseAction(console, left, right, leftActive, logger);  // FEA-0009
                } else {
                    DoViewAction(console, left, right, leftActive, logger);  // FEA-0009
                }
                break;
            case VK_F4:
                if (ctrlHeld) {
                    DoSort(target, SortKey::Extension);
                } else {
                    DoEdit(console, left, right, leftActive, config, logger, pendingEdits);
                }
                break;
            case VK_F7:
                if (altHeld) {
                    DoSearch(console, left, right, leftActive);
                } else if (shiftHeld) {
                    DoCreateNewFile(console, left, right, leftActive);
                } else {
                    DoMakeDirectory(console, left, right, leftActive);
                }
                break;
            case 'F':
                if (ctrlHeld) {
                    DoFilter(console, left, right, leftActive);
                } else {
                    appendToCommandLine(record.Event.KeyEvent.uChar.UnicodeChar);
                }
                break;
            case 'A':
                if (ctrlHeld) {
                    DoChangeAttributes(console, left, right, leftActive, logger);
                } else {
                    appendToCommandLine(record.Event.KeyEvent.uChar.UnicodeChar);
                }
                break;
            case 'H':
                if (ctrlHeld) {
                    target.ToggleHiddenAndSystem();
                } else {
                    appendToCommandLine(record.Event.KeyEvent.uChar.UnicodeChar);
                }
                break;
            case 'R':
                if (ctrlHeld) {
                    // IS-0012: re-reads the active panel's directory from
                    // disk. For a panel currently showing plugin content,
                    // this case never runs at all -- the key-routing check
                    // above already sent it straight to that plugin's own
                    // OnPanelKey, same as every key but Tab/F1/F10.
                    target.Refresh();
                } else {
                    appendToCommandLine(record.Event.KeyEvent.uChar.UnicodeChar);
                }
                break;
            case 'T':
                if (ctrlHeld) {
                    // UI-013, Part B: reached only when this panel has no
                    // content installed already (the key-routing check
                    // above would otherwise have sent Ctrl+T straight to
                    // whatever's already there, e.g. a plugin's own
                    // archive browser) -- entering is always safe here.
                    // Leaving (Enter/Esc/Ctrl+T again) is handled inside
                    // DirectoryTreeContent::OnKey instead, since by then
                    // this case is no longer the one receiving the key.
                    DirectoryTreeContent& treeContent = leftActive ? leftTreeContent : rightTreeContent;
                    treeContent.Enter(target, leftActive ? right : left);
                } else {
                    appendToCommandLine(record.Event.KeyEvent.uChar.UnicodeChar);
                }
                break;
            case 'M':
                if (ctrlHeld) {
                    DoGroupRename(console, left, right, leftActive, logger);  // FOP-017
                } else {
                    appendToCommandLine(record.Event.KeyEvent.uChar.UnicodeChar);
                }
                break;
            case 'D':
                if (ctrlHeld) {
                    DoDirectoryHotlist(console, left, right, leftActive, logger);  // NAV-007
                } else {
                    appendToCommandLine(record.Event.KeyEvent.uChar.UnicodeChar);
                }
                break;
            case 'Q':
                if (altHeld) {
                    ShowJobQueue(console, left, right, leftActive, jobs);
                } else {
                    appendToCommandLine(record.Event.KeyEvent.uChar.UnicodeChar);
                }
                break;
            case VK_F5:
                if (altHeld) {
                    // FOP-014: queued copy -- runs in the background, same destination prompt.
                    QueueCopyOrMove(console, left, right, leftActive, /*isMove=*/false, jobs);
                } else if (ctrlHeld) {
                    DoSort(target, SortKey::Size);
                } else if (shiftHeld) {
                    // FOP-019: copy the cursor entry under a new name, in
                    // place -- never routed to a plugin's receive-files.
                    DoCopyAs(console, left, right, leftActive, logger);
                } else {
                    // Reported bug: copying onto a panel showing plugin
                    // content (e.g. an open ZIP archive) silently copied
                    // into that content's real underlying directory instead
                    // of into the content itself -- Panel content replaces
                    // only drawing/key handling, not the panel's own real
                    // path underneath. Give the owning plugin first refusal
                    // (API 1.8, McPlugin_OnReceiveFiles); fall back to the
                    // ordinary real-filesystem copy unchanged if it declines
                    // or no plugin owns content on that side at all.
                    McPanelSide destSide = leftActive ? kMcPanelRight : kMcPanelLeft;
                    McPanelSide srcSide = leftActive ? kMcPanelLeft : kMcPanelRight;
                    if (!pluginManager.TryReceiveFiles(destSide, srcSide)) {
                        DoCopyOrMove(console, left, right, leftActive, /*isMove=*/false, logger);
                    }
                }
                break;
            case VK_F6:
                if (altHeld) {
                    QueueCopyOrMove(console, left, right, leftActive, /*isMove=*/true, jobs);
                } else if (ctrlHeld) {
                    DoSort(target, SortKey::ModifiedTime);
                } else {
                    DoCopyOrMove(console, left, right, leftActive, /*isMove=*/true, logger);
                }
                break;
            case VK_F8:
                if (altHeld) {
                    // FOP-014: Alt+F8 / Alt+Shift+F8 queue the delete as a background job.
                    QueueDelete(console, left, right, leftActive, /*permanent=*/shiftHeld, config, jobs);
                } else {
                    DoDelete(console, left, right, leftActive, /*permanent=*/shiftHeld, config, logger);
                }
                break;
            case VK_DELETE:
                // IS-0015 Part 3: forward-deletes in the command line when
                // it has text, mirroring Backspace's own already-shipped
                // dual-meaning convention exactly; file deletion only
                // applies once the command line is empty, no longer
                // sharing VK_F8's unconditional case.
                if (!commandLine.empty()) {
                    LineEdit edit{commandLine, commandLineCursor};
                    ApplyLineEditKey(edit, key, 0);
                    commandLine = edit.text;
                    commandLineCursor = edit.cursor;
                    historyIndex = -1;
                } else {
                    DoDelete(console, left, right, leftActive, /*permanent=*/shiftHeld, config, logger);
                }
                break;
            case VK_F9:
                DoOpenShell(console, left, right, leftActive);
                break;
            case VK_ESCAPE:
                // IS-0005: Esc no longer quits the app when the command
                // line is already empty â€” F10 is the sole quit key now.
                if (!commandLine.empty()) {
                    commandLine.clear();
                    commandLineCursor = 0;  // IS-0015 Part 3
                    historyIndex = -1;
                }
                break;
            case VK_F10:
                // FOP-014: quitting with background jobs still active asks first.
                if (ConfirmQuitWithJobs(console, left, right, leftActive, jobs, jobsWatch)) running = false;
                break;
            case VK_F11:
                // IS-0006: plain F11 is reserved by most terminal hosts
                // (Windows Terminal included) as a window-fullscreen
                // toggle and never reaches the app at all â€” confirmed by
                // the user, the same class of host interception IS-0004
                // hit with Shift-held mouse clicks. Ctrl+F11 isn't
                // reserved that way.
                if (ctrlHeld) DoEditConfig(console, left, right, leftActive, logger, configWatch);
                break;
            default:
                // FEA-0002 Â§12: a plugin-registered shortcut is tried
                // before falling back to ordinary command-line typing --
                // RegisterShortcut already refused any key a built-in
                // `case` above already owns, so this can never shadow one.
                if (!pluginManager.DispatchShortcut(key, ctrlHeld, shiftHeld, altHeld)) {
                    appendToCommandLine(record.Event.KeyEvent.uChar.UnicodeChar);
                }
                break;
        }
    }

    // FEA-0002 Â§5: explicit, ordered shutdown -- deactivates and unloads
    // every still-loaded plugin before the session/command-history writes
    // below, matching this project's existing "explicit, normal-exit-only
    // cleanup" convention rather than relying on PluginManager's own
    // destructor order relative to `left`/`right`/`console` (also local to
    // this function, so destructor order would happen to work today, but
    // explicit is what every other shutdown-only write here already does).
    pluginManager.Shutdown();
    gPluginManager = nullptr;

    // NAV-008: this is the sole write point for the session file. Paths are
    // captured only after an explicit, normal loop exit (F10, IS-0005's sole quit key), never
    // during a file operation or from an abnormal-termination handler.
    SaveSession(SessionState{left.Path(), right.Path(), gSinglePanelLayout});
    // CLI-004: same normal-exit-only write point as the session file, but
    // gated on the user's own setting â€” persistCommandHistory defaulting to
    // true is what makes history survive across runs at all.
    if (config.persistCommandHistory) SaveCommandHistory(commandHistory);
    logger.Log(LogLevel::Info, L"MyCommander exiting normally");
    CoUninitialize();
    return 0;
}
