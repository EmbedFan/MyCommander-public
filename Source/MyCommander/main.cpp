#include "CommandHistory.h"
#include "Config.h"
#include "Console.h"
#include "Dialog.h"
#include "Elevation.h"
#include "FileOps.h"
#include "Help.h"
#include "HintBar.h"
#include "Log.h"
#include "Navigation.h"
#include "Panel.h"
#include "PathUtil.h"
#include "Process.h"
#include "Search.h"
#include "Session.h"
#include "Strings.h"
#include "TextWidth.h"
#include "Theme.h"
#include "Version.h"
#include "Viewer.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cwchar>
#include <filesystem>
#include <optional>

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
using mc::ElevatedAction;
using mc::ElevatedRequest;
using mc::EnsureConfigFileExists;
using mc::Entry;
using mc::EnterFileAction;
using mc::ErrorAskResult;
using mc::ErrorChoice;
using mc::FileAttributeFlag;
using mc::FormatWinError;
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
using mc::ShowMessage;
using mc::ShowProgress;
using mc::ShowShortcutReference;
using mc::ShowTextFileViewer;
using mc::SortKey;
using mc::ToggleAttributes;
using mc::ShowTextPrompt;
using mc::StringDisplayWidth;

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
// or path contains full-width (CJK) characters — plain code-unit counting
// (the old std::wstring::size()-based version of this function) drifts out
// of alignment as soon as one appears.
std::wstring PadOrTrim(const std::wstring& text, size_t width) {
    return mc::PadToDisplayWidth(text, width);
}

// CON-005: a compact Total-Commander-style attribute column — one letter per
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
// Directories show nothing here — "<DIR>" already marks them in the size
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
// falling back to notepad.exe — always present, needs no setup, and safe as
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
// space-separated) to `commandLine`, quoting any that contain a space —
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
// never modified again — Config has no live-reload/in-app-settings path
// yet, so this is safe as module state rather than threading it through
// every one of DrawFrame's ~20 call sites (most of them dialog
// "redrawBackground" callbacks with no natural place to carry a Config
// reference through).
std::vector<ColumnId> gVisibleColumns = {ColumnId::Type, ColumnId::Attributes, ColumnId::ModifiedTime,
                                         ColumnId::Size};
int gTypeColumnWidth = 6;
int gMtimeColumnWidth = 16;
int gSizeColumnWidth = 10;

// Attributes is always exactly 4 characters (one slot per R/H/S/A flag) —
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
// priority — a user's own column order doubles as a drop priority) until
// what remains fits `availableWidth` alongside the selection marker and at
// least a sliver of name column. Generalizes UI-004's old hardcoded
// two-tier full/basic drop into an N-tier one driven by the configured
// column list.
std::vector<ColumnId> FitColumnsToWidth(const std::vector<ColumnId>& requested, SHORT availableWidth,
                                        size_t selectionMarkerWidth) {
    std::vector<ColumnId> cols = requested;
    auto reserved = [&] {
        size_t total = selectionMarkerWidth;
        for (ColumnId c : cols) total += 1 + ColumnDisplayWidth(c);
        return total;
    };
    while (!cols.empty() && static_cast<size_t>(availableWidth) <= reserved() + 1) {
        cols.pop_back();
    }
    return cols;
}

void DrawPanel(Console& console, const Panel& panel, bool active, SHORT x, SHORT y, SHORT width, SHORT height) {
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
    // — not just the shared bottom status bar, which only ever reflects the
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
        // LIST-006: a broken reparse point overrides dir/selected coloring —
        // more important to notice than either.
        if (entry.inaccessible) attr = kAttrBroken;
        if (isCursorRow) {
            // ACC-002: a broken entry's color (kAttrBroken: FOREGROUND_RED
            // only) combined with the ordinary active-panel cursor
            // highlight (BACKGROUND_BLUE) computes to ~4.0:1 contrast —
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
        // column — so columns drop out gracefully (from the end of the
        // user's own configured order) rather than truncating the name to
        // nothing.
        std::vector<ColumnId> cols = FitColumnsToWidth(gVisibleColumns, width, kSelectionMarkerWidth);
        size_t reserved = kSelectionMarkerWidth;
        for (ColumnId c : cols) reserved += 1 + ColumnDisplayWidth(c);
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

// Draws both panels, the command line/status/hint bars, but does not
// Present() — callers compose more onto the back buffer first (a dialog box,
// a progress line) and present once. Also reused as the "redrawBackground"
// callback dialogs call to repaint what's behind them, in which case
// `commandLine` is left at its default (empty) since those callers don't
// have access to the live buffer — a purely cosmetic gap, since the real
// buffer reappears correctly once the dialog closes and the main loop
// redraws again.
void DrawFrame(Console& console, const Panel& left, const Panel& right, bool leftActive,
              const std::wstring& commandLine = L"") {
    SHORT width = console.Width();
    SHORT height = console.Height();
    const Panel& active = leftActive ? left : right;
    // IS-0007: the hint bar grows to however many rows its current context
    // needs at the current width (recomputed every draw call, so a resize
    // or a context change — selecting an item, typing into the command
    // line — both take effect immediately, with no separate "on resize"
    // handling needed). The command line always gets one row; the status
    // line only gets one if it actually has something to say (BuildStatusLine
    // returns an empty string outside a selection/error — a reserved-but-
    // blank row there just reads as a dead strip of the hint bar's own
    // color sitting above it, once the hint bar is tall enough to make that
    // obvious, so it's skipped entirely rather than drawn empty).
    HintContext hintContext = GetHintContext(active, commandLine);
    SHORT hintRows = static_cast<SHORT>(HintBarRowCount(hintContext, width));
    std::wstring status = BuildStatusLine(active);
    bool hasStatusLine = !status.empty();
    SHORT reservedRows = static_cast<SHORT>(1 + (hasStatusLine ? 1 : 0) + hintRows);
    SHORT contentHeight = static_cast<SHORT>(height - reservedRows);
    SHORT leftWidth = static_cast<SHORT>(width / 2);
    SHORT rightWidth = static_cast<SHORT>(width - leftWidth - 1);
    SHORT rightX = static_cast<SHORT>(leftWidth + 1);

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
    console.FillRow(0, 0, leftWidth, horizontal, kAttrNormal);
    console.FillRow(0, leftWidth, 1, topJunction, kAttrNormal);
    console.FillRow(0, rightX, rightWidth, horizontal, kAttrNormal);
    console.PutText(0, 0, std::wstring(1, topLeft), kAttrNormal);
    console.PutText(static_cast<SHORT>(width - 1), 0, std::wstring(1, topRight), kAttrNormal);

    const SHORT bottom = static_cast<SHORT>(contentHeight - 1);
    console.FillRow(bottom, 0, leftWidth, horizontal, kAttrNormal);
    console.FillRow(bottom, leftWidth, 1, bottomJunction, kAttrNormal);
    console.FillRow(bottom, rightX, rightWidth, horizontal, kAttrNormal);
    console.PutText(0, bottom, std::wstring(1, bottomLeft), kAttrNormal);
    console.PutText(static_cast<SHORT>(width - 1), bottom, std::wstring(1, bottomRight), kAttrNormal);

    for (SHORT row = 1; row < bottom; ++row) {
        console.FillRow(row, 0, 1, vertical, kAttrNormal);
        console.FillRow(row, leftWidth, 1, vertical, kAttrNormal);
        console.FillRow(row, static_cast<SHORT>(width - 1), 1, vertical, kAttrNormal);
    }
    DrawPanel(console, left, leftActive, 1, 1, static_cast<SHORT>(leftWidth - 1),
              static_cast<SHORT>(contentHeight - 2));
    DrawPanel(console, right, !leftActive, rightX, 1, static_cast<SHORT>(rightWidth - 1),
              static_cast<SHORT>(contentHeight - 2));

    std::wstring cmdLine = active.Path().wstring() + L"> " + commandLine + L"_";
    SHORT cmdLineRow = static_cast<SHORT>(height - reservedRows);
    console.PutText(0, cmdLineRow, PadOrTrim(cmdLine, width), kAttrCommandLine);
    if (hasStatusLine) {
        console.PutText(0, static_cast<SHORT>(cmdLineRow + 1), PadOrTrim(status, width), kAttrStatusBar);
    }

    // IS-0007: one PutText per hint row rather than the old single fixed
    // row — each row is cleared to full width first so a row that shrinks
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

// IS-0007: `hintContext`/`hasStatusLine` must be the same ones DrawFrame's
// own layout used for this frame (GetHintContext(active panel, commandLine)
// / !BuildStatusLine(active panel).empty()) — this and DrawFrame have to
// agree on exactly how many rows are reserved, or mouse hit-testing and
// Page Up/Down will target rows that don't match what's actually drawn.
int ComputeVisibleRows(Console& console, HintContext hintContext, bool hasStatusLine) {
    SHORT hintRows = static_cast<SHORT>(HintBarRowCount(hintContext, console.Width()));
    SHORT reservedRows = static_cast<SHORT>(1 + (hasStatusLine ? 1 : 0) + hintRows);
    SHORT contentHeight = static_cast<SHORT>(console.Height() - reservedRows);
    // DrawFrame reserves the top/bottom pane borders, and DrawPanel now has
    // both a path row and a UI-003 volume-information row before file rows.
    return std::max<int>(1, contentHeight - 4);
}

// UI-012: HitTestPanel/PanelHit (which panel, and which visible row within
// it, a mouse event's console-cell coordinates landed on) live in
// Navigation.h/.cpp, not here — pure functions of width/visibleRows/mouse
// coordinates with no Console/Panel dependency, kept Console-independent
// (matching Navigation.h's existing ClassifyNavigationKey) so the test
// project can exercise them directly, the same reasoning TST-005 already
// applied to keyboard dispatch.

// Drains all pending input, applying any resize seen along the way, and
// reports whether Esc was among the drained keys. Used to let a long copy
// or move be cancelled without blocking on ReadConsoleInputW.
bool PeekEscapePressed(Console& console) {
    bool escapePressed = false;
    INPUT_RECORD record;
    DWORD count = 0;
    while (PeekConsoleInputW(console.InputHandle(), &record, 1, &count) && count > 0) {
        DWORD consumed = 0;
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &consumed) || consumed == 0) break;
        if (record.EventType == KEY_EVENT && record.Event.KeyEvent.bKeyDown &&
            record.Event.KeyEvent.wVirtualKeyCode == VK_ESCAPE) {
            escapePressed = true;
        } else if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            console.UpdateSize();
        }
    }
    return escapePressed;
}

// Bundles the per-operation progress dialog and cancel polling so
// CopyItems/MoveItems callbacks stay short one-line lambdas at the call site.
// FOP-009: a modal-style box (not just a bottom status line) showing source,
// destination, current item/progress, and — for Copy/Move, once at least one
// sample has arrived via OnByteProgress — the current file's transferred-
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

        auto redrawBackground = [&]() { DrawFrame(console, left, right, leftActive); };
        ShowProgress(console, verb, body, redrawBackground);
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
// cancellation — access denied, a sharing violation because something else
// has the file open, a full disk, and the like. Retry re-attempts the same
// item immediately (useful once the underlying cause — e.g. whatever had
// the file open — is fixed); Skip/Skip all mirror AskConflict's own
// Skip/Skip all.
// SEC-003: `win32Error == ERROR_ACCESS_DENIED` adds an extra "Elevate & Retry"
// hotkey that isn't offered for any other failure — elevation only ever
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
// logged (Error/Warning) — this is the one place all of Copy/Move/Delete/
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
// matching PathUtil.h's WildcardMatch semantics — unlike legacy DOS masking,
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

    auto patternOpt = ShowTextPrompt(console, kStrFindFilesPrompt, L"", redraw);
    if (!patternOpt || patternOpt->empty()) return;

    console.Clear(kAttrNormal);
    console.PutText(0, 0, PadOrTrim(FormatSearchingHeader(target.Path().wstring(), *patternOpt), console.Width()),
                    kAttrHeaderActive);
    console.Present();

    auto cancelPoll = [&]() { return PeekEscapePressed(console); };
    SearchOutcome outcome = SearchFiles(target.Path(), *patternOpt, cancelPoll);

    if (outcome.results.empty()) {
        ShowMessage(console, kStrFindFilesTitle, {outcome.cancelled ? kStrSearchCancelled : kStrNoMatchesFound}, redraw);
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
        std::wstring header = FormatSearchResultsHeader(outcome.results.size(), outcome.truncated, *patternOpt);
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
            // file result by kAttrDir's color alone — a trailing "\" (the
            // ordinary Windows convention for a directory path) makes that
            // status readable from the text itself.
            std::wstring resultLine = r.fullPath.wstring() + (r.isDirectory ? L"\\" : L"");
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
                    ShowTextFileViewer(console, outcome.results[cursor].fullPath);
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

// NAV-004: one Windows logical drive's identity and, where queryable,
// its free/total space — a removed removable drive or an unreachable
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

void DoRename(Console& console, Panel& left, Panel& right, bool leftActive) {
    Panel& target = leftActive ? left : right;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    const auto& entries = target.Entries();
    int cursor = target.Cursor();
    if (cursor < 0 || cursor >= static_cast<int>(entries.size())) return;
    const auto& entry = entries[cursor];
    if (entry.name == L"..") return;

    auto newNameOpt = ShowTextPrompt(console, kStrRenameTitle, entry.name, redraw);
    if (!newNameOpt || newNameOpt->empty() || *newNameOpt == entry.name) return;

    RenameResult result = RenameItem(target.Path(), entry.name, *newNameOpt);
    target.Refresh();
    if (!result.ok) {
        ShowMessage(console, kStrRenameFailedTitle, {result.error}, redraw);
    }
}

void DoMakeDirectory(Console& console, Panel& left, Panel& right, bool leftActive) {
    Panel& target = leftActive ? left : right;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    auto nameOpt = ShowTextPrompt(console, kStrNewFolderNamePrompt, L"", redraw);
    if (!nameOpt || nameOpt->empty()) return;

    auto result = CreateNewDirectory(target.Path(), *nameOpt);
    // IS-0001: select the new directory once the refresh lands, not just
    // preserve wherever the cursor already was.
    target.NavigateTo(target.Path(), *nameOpt);
    if (!result.ok) {
        ShowMessage(console, kStrCreateFolderFailedTitle, {result.error}, redraw);
    }
}

// FOP-005: creates a new, empty file in the active panel's directory.
void DoCreateNewFile(Console& console, Panel& left, Panel& right, bool leftActive) {
    Panel& target = leftActive ? left : right;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    auto nameOpt = ShowTextPrompt(console, kStrNewFileNamePrompt, L"", redraw);
    if (!nameOpt || nameOpt->empty()) return;

    auto result = CreateNewFile(target.Path(), *nameOpt);
    // IS-0001: select the new file once the refresh lands, not just
    // preserve wherever the cursor already was.
    target.NavigateTo(target.Path(), *nameOpt);
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

void DoView(Console& console, Panel& left, Panel& right, bool leftActive) {
    Panel& target = leftActive ? left : right;
    auto filePath = CursorFilePath(target);
    if (!filePath) return;
    ShowTextFileViewer(console, *filePath);
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
// it open as their own directory — neither, either, or both may match.
void RefreshPanelsForEditedFile(Panel& left, Panel& right, const std::filesystem::path& editedFile) {
    std::filesystem::path parent = editedFile.parent_path();
    std::wstring name = editedFile.filename().wstring();
    if (left.Path() == parent) left.NavigateTo(left.Path(), name);
    if (right.Path() == parent) right.NavigateTo(right.Path(), name);
}

// IS-0002: called once per main-loop iteration — the same ~16 ms cadence
// LIST-008's incremental panel loading already relies on — with
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
        if (!result.started) {
            auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
            ShowMessage(console, kStrCannotStartEditorTitle, {kStrEditorLabel + editor, result.error}, redraw);
        }
        return;
    }

    // IS-0002: launch detached and return immediately — no console
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
        // refuse an edit that's already started — log it and move on
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
// edit — but unlike PendingEdit (whose reaction to a change is "refresh a
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
// startup, to the already-running app — the fields read live via a
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

// IS-0006: reloads Config from disk and applies it live — shared by
// PollConfigWatch's two trigger points (a save, and the editor's final
// exit) so both go through the exact same reload-plus-apply-plus-report
// sequence. Deliberately does not re-run AppendMissingConfigKeys (that
// backfill is startup-only — see the fix plan's own scope note on why:
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
// config file's directory changed (most likely: it was saved) — reload
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
// Always launches detached, regardless of Config::waitForEditorToClose —
// that setting is specifically about F4's own file-editing UX trade-off (a
// console-subsystem %EDITOR% sharing this window); blocking the whole app
// until the config editor closes would defeat the point of a *live*
// reload while you keep working. EnsureConfigFileExists() has already run
// unconditionally before the main loop starts, so the file is always
// guaranteed to exist by the time Ctrl+F11 could ever be pressed.
void DoEditConfig(Console& console, Panel& left, Panel& right, bool leftActive, Logger& logger, ConfigWatch& watch) {
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
        // window closed) — close it out cleanly first rather than leak it.
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

// VEE-002/VEE-008: runs the cursor file through its own Windows file
// association (ShellExecuteW), the same as double-clicking it in Explorer
// — an .exe runs, a document opens in whatever app is registered for it.
// The only path in the app that ever runs a file's own code/association
// rather than viewing or editing it; only reachable via Enter, and only
// once the user has explicitly configured enterFileAction=execute, so a
// file is never run without a deliberate, explicit action (VEE-008).
void DoExecute(Console& console, Panel& left, Panel& right, bool leftActive) {
    Panel& target = leftActive ? left : right;
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
    if (!ok) {
        auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
        ShowMessage(console, kStrCannotOpenFileTitle, {filePath->filename().wstring(), FormatWinError(err)}, redraw);
    }
}

// EXT-001/CON-004/IS-0003: the actual copy/move engine call, shared by
// DoCopyOrMove (F5/F6, which first prompts for `destDir` via ShowTextPrompt)
// and IS-0003's drag-and-drop drop handler (which already knows `destDir` —
// it's the panel the item was dropped on — and so skips the prompt
// entirely). Everything from the destination existence check onward is
// identical either way: this UI layer only decides what to do about each
// outcome (ask, show an error), never touches std::filesystem directly.
void PerformCopyOrMove(Console& console, Panel& left, Panel& right, bool leftActive, Panel& source,
                       const std::vector<std::wstring>& items, const std::filesystem::path& destDir, bool isMove,
                       Logger& logger) {
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
    std::wstring verb = isMove ? kStrVerbMove : kStrVerbCopy;

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
    // an access-denied failure (AskFileError above) — a UAC prompt for this
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
              : CopyItems(source.Path(), items, destDir, onConflict, onItemStart, cancelPoll, onError, onByteProgress,
                          onElevateCopy);

    left.Refresh();
    right.Refresh();
    ReportOutcome(console, left, right, leftActive, verb, outcome, logger);
}

// VEE-002: activates the active panel's cursor entry — a directory (or
// "..") is entered via EnterSelected(); a file follows the configured
// default action (View/Edit/Execute), same as F3/F4 always meaning
// View/Edit regardless of the setting. Shared by plain Enter (with an
// empty command line) and a mouse double-click (UI-012), so both trigger
// byte-for-byte identical behavior rather than two parallel copies of the
// same switch.
void ActivateCursorEntry(Console& console, Panel& left, Panel& right, bool leftActive, const Config& config,
                         Logger& logger, std::vector<PendingEdit>& pendingEdits) {
    Panel& target = leftActive ? left : right;
    if (CursorFilePath(target)) {
        switch (config.enterFileAction) {
            case EnterFileAction::Edit:
                DoEdit(console, left, right, leftActive, config, logger, pendingEdits);
                break;
            case EnterFileAction::Execute:
                DoExecute(console, left, right, leftActive);
                break;
            case EnterFileAction::View:
            default:
                DoView(console, left, right, leftActive);
                break;
        }
    } else {
        target.EnterSelected();
    }
}

// IS-0003: drag-and-drop state, tracked across consecutive HandleMouseEvent
// calls (like PendingEdit's own cross-iteration lifetime) since Windows
// console mouse events carry no distinct "button released" event type — a
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
// stage — a right-press has no other meaning in this app to disambiguate
// against, so it commits to toggling that entry immediately.
struct SelectDragState {
    bool active = false;
    bool isLeftPanel = false;
    bool targetSelected = false;  // the state every further entry is forced to
    int lastIndex = -1;           // avoids re-processing the same cell repeatedly
};

// IS-0003: injects a synthetic key-down/key-up pair into the console's own
// input queue via WriteConsoleInputW, so a hint-bar click can trigger
// exactly what pressing that key for real would — reusing wmain's existing
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

    DWORD written = 0;
    WriteConsoleInputW(console.InputHandle(), records, 2, &written);
}

// IS-0003/IS-0007: which hint-bar action (if any) a click at (mouseX,
// mouseY) hits — recomputes LayOutHintBar's row/column layout fresh from
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
// selection for the two main panels — the optional, never-required mouse
// support named there and in KEY-001's invariant (see Console.cpp's
// constructor comment for how that invariant is preserved once
// ENABLE_MOUSE_INPUT is on). Left-click moves the cursor to the clicked
// entry and, if the click landed in the other panel, makes that panel
// active — matching Total Commander's own click-to-activate convention. A
// double-click additionally activates the entry (ActivateCursorEntry),
// identical to pressing Enter on it. Dragging a selection (or the cursor
// entry, if nothing is selected) from one panel to the other copies it
// there by default, or moves it if Shift is held at the moment of drop —
// see DragState's own comment for why this needs to be a small state
// machine rather than a single self-contained gesture. A right-click
// toggles the clicked entry's selection mark, and holding the right button
// while dragging extends that selection to every further entry the mouse
// passes over (SelectDragState) — deliberately the *right* button, not a
// second use of the left button, since a plain left-drag already means
// copy/move (IS-0003). A Ctrl+left-click, or a Ctrl+right-click on a
// second entry after an ordinary right-click on a first one (IS-0004's
// addendum), each select every entry between wherever the cursor already
// was and the clicked entry (see Panel::SelectRange's own comment on how
// its anchor differs from Explorer's) — Ctrl+right-click is a single
// discrete gesture, not draggable, so it never starts a SelectDragState.
// Ctrl, not the Shift the issue originally asked for, is what triggers
// this: classic console host (conhost.exe) silently swallows a Shift-held
// mouse click before it ever reaches the app's input queue (confirmed
// against both buttons; no SetConsoleMode flag changes that), so Shift
// can't be used for a mouse gesture there. Shift is still what a plain
// left-drag's own drop uses to mean "move instead of copy" (IS-0003) —
// that's a different moment (a button *release*, not a fresh click) and
// hasn't shown the same symptom, so it's deliberately left alone here.
// The mouse wheel moves the active panel's cursor, 3 rows per
// notch (an ordinary wheel-scroll granularity), regardless of which panel
// the wheel was over — the same "always acts on the active panel" rule
// every other unmodified navigation key already follows. Deliberately out
// of scope: dialogs (apart from IS-0003's own dialog-button clicks, handled
// entirely inside Dialog.cpp), the drive/search pickers, the viewer, and
// the help screen stay keyboard-only — this covers exactly what the
// requirement names (panel selection, scrolling, activation, drag-and-drop,
// hint-bar commands, multi-select), not every screen in the app.
void HandleMouseEvent(Console& console, Panel& left, Panel& right, bool& leftActive, const std::wstring& commandLine,
                      const Config& config, Logger& logger, std::vector<PendingEdit>& pendingEdits, DragState& drag,
                      SelectDragState& selectDrag, const MOUSE_EVENT_RECORD& mouse) {
    // IS-0007: computed once from the panel/command-line state as of this
    // event, matching DrawFrame's own hint-bar sizing for the frame this
    // event is handled within — see ComputeVisibleRows's own comment for
    // why the two must agree. A click that flips the active panel partway
    // through this function could leave later ComputeVisibleRows calls in
    // *this* call using a context computed from the panel active at entry,
    // not after; harmless (ComputeVisibleRows always returns a safe, >=1
    // value, and the very next frame recomputes fresh), so not worth
    // re-deriving mid-function.
    HintContext hintContext = GetHintContext(leftActive ? left : right, commandLine);
    bool hasStatusLine = !BuildStatusLine(leftActive ? left : right).empty();
    if (mouse.dwEventFlags == MOUSE_WHEELED) {
        // The wheel delta lives in dwButtonState's high word as a signed
        // 16-bit value: positive means scrolled toward the user (up),
        // negative away (down) — the opposite sign convention from
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
    // is already active (so its release — the button bit disappearing — is
    // still seen here rather than falling through to the left-button code).
    bool rightButtonHeld = (mouse.dwButtonState & RIGHTMOST_BUTTON_PRESSED) != 0;
    if (rightButtonHeld || selectDrag.active) {
        if (!rightButtonHeld) {
            // Released — every selection change already took effect live
            // as the mouse passed over each entry, so there's nothing left
            // to do but reset.
            selectDrag = SelectDragState{};
            return;
        }
        auto hit = HitTestPanel(console.Width(), ComputeVisibleRows(console, hintContext, hasStatusLine), mouse.dwMousePosition.X,
                                mouse.dwMousePosition.Y);
        if (!selectDrag.active) {
            // A fresh right-button press — only act on the actual press
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
                // Ctrl+left-click already uses (Panel::SelectRange) — a
                // single discrete gesture, not draggable, so this returns
                // without starting selectDrag. Uses Ctrl, not the Shift the
                // issue originally asked for: classic console host
                // (conhost.exe) silently swallows a Shift-held mouse click
                // before it ever reaches the app's input queue (confirmed
                // against both buttons, with no popup/menu appearing either
                // — it's not intercepted for a visible reason, it's just
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
        // Continuing a drag — a right-drag stays confined to the panel it
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
    // hint bar triggers that hint's action via a synthetic key press — see
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
            // Still dragging — move the cursor in whichever panel the mouse
            // currently hovers over, as visual feedback for where a drop
            // would land.
            if (mouse.dwEventFlags & MOUSE_MOVED) {
                auto hoverHit = HitTestPanel(console.Width(), ComputeVisibleRows(console, hintContext, hasStatusLine), mouse.dwMousePosition.X,
                                             mouse.dwMousePosition.Y);
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
        // the source panel, or outside both panels, is a no-op — the drag
        // state just resets either way.
        auto dropHit = HitTestPanel(console.Width(), ComputeVisibleRows(console, hintContext, hasStatusLine), mouse.dwMousePosition.X,
                                    mouse.dwMousePosition.Y);
        if (dropHit && dropHit->isLeftPanel != drag.sourceIsLeftPanel) {
            Panel& sourcePanel = drag.sourceIsLeftPanel ? left : right;
            Panel& destPanel = dropHit->isLeftPanel ? left : right;
            bool isMove = (mouse.dwControlKeyState & SHIFT_PRESSED) != 0;
            PerformCopyOrMove(console, left, right, leftActive, sourcePanel, drag.items, destPanel.Path(), isMove,
                              logger);
        }
        drag = DragState{};
        return;
    }
    if (drag.dragCandidate) {
        if (!leftButtonHeld) {
            // Released before any movement — was just a click, already
            // handled at press time below.
            drag = DragState{};
            return;
        }
        if (mouse.dwEventFlags & MOUSE_MOVED) {
            Panel& src = drag.sourceIsLeftPanel ? left : right;
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
    // activates — a right-click, a wheel-button click, or any other button
    // combination is ignored rather than guessed at.
    if (!isPressOrDoubleClick || !leftButtonHeld) return;

    auto hit = HitTestPanel(console.Width(), ComputeVisibleRows(console, hintContext, hasStatusLine), mouse.dwMousePosition.X,
                            mouse.dwMousePosition.Y);
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
        // cursor already was and the clicked entry (inclusive) — not a
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
    // empty command line — a non-empty one means the user is mid-typing a
    // command, which a click shouldn't interrupt or run.
    if (mouse.dwEventFlags == DOUBLE_CLICK && commandLine.empty()) {
        ActivateCursorEntry(console, left, right, leftActive, config, logger, pendingEdits);
    } else if (mouse.dwEventFlags == 0) {
        // IS-0003: remember this press as a possible drag start, promoted
        // to an actual drag only if a MOUSE_MOVED event arrives next while
        // the button is still held (see the drag.dragCandidate branch
        // above) — so a plain click with no movement never becomes one.
        drag.dragCandidate = true;
        drag.sourceIsLeftPanel = hit->isLeftPanel;
    }
}

// CLI-005: opens an interactive shell in the active panel's directory,
// reusing the same console-handoff Process/Console already built for F4
// Edit (per the precedent noted on Process.{h,cpp}) rather than anything new.
void DoOpenShell(Console& console, Panel& left, Panel& right, bool leftActive) {
    Panel& target = leftActive ? left : right;
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
// interpreted by cmd.exe itself — the same predictable behavior a user
// already gets at a normal command prompt, not a custom parser of our own.
void RunShellCommand(Console& console, Panel& left, Panel& right, bool leftActive, const std::wstring& commandText) {
    Panel& target = leftActive ? left : right;
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
    Panel& source = leftActive ? left : right;
    Panel& dest = leftActive ? right : left;
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

// FOP-008: whether this delete needs an explicit Yes/No confirmation is
// itself a configurable safety rule (Config::confirmRecycleBinDelete/
// confirmPermanentDelete, both default true) rather than an unconditional
// dialog — the gap that kept FOP-008 at Partial, since every destructive
// action previously confirmed unconditionally with no way to tune that.
void DoDelete(Console& console, Panel& left, Panel& right, bool leftActive, bool permanent, const Config& config,
             Logger& logger) {
    Panel& target = leftActive ? left : right;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    std::vector<std::wstring> items = target.SelectionOrCursor();
    if (items.empty()) return;

    bool needsConfirmation = permanent ? config.confirmPermanentDelete : config.confirmRecycleBinDelete;
    if (needsConfirmation) {
        std::vector<std::wstring> lines;
        if (items.size() == 1) {
            lines.push_back(L"\"" + items[0] + L"\"");
        } else {
            lines.push_back(FormatItemCount(items.size()));
        }
        lines.push_back(permanent ? kStrWillBePermanentlyDeleted : kStrWillBeSentToRecycleBin);

        int choice = ShowChoiceDialog(console, permanent ? kStrPermanentlyDeleteTitle : kStrDeleteTitle, lines,
                                      {{L'Y', kStrYesLabel}, {L'N', kStrNoLabel}}, redraw);
        if (choice != 0) return;
    }

    // PER-002: live per-item progress plus a prompt Esc-cancel, the same
    // responsiveness Copy/Move already have — the permanent path deletes
    // item-by-item, same as they do, so a large selection no longer runs
    // to completion with a frozen screen and no way to interrupt it.
    ProgressUi ui{console, left, right, leftActive, kStrVerbDeleting};
    ui.primaryPath = target.Path().wstring();
    auto onItemStart = [&](const std::wstring& n, int done, int total) { return ui.OnItemStart(n, done, total); };
    auto cancelPoll = [&]() { return ui.PollCancel(); };
    auto onError = [&](const std::wstring& n, const std::wstring& msg, DWORD win32Error) {
        return AskFileError(console, left, right, leftActive, n, msg, win32Error);
    };
    // SEC-003: see DoCopyOrMove's onElevateCopy/onElevateMove for the shape;
    // only reached for an access-denied failure the user chose to elevate.
    // Permanent delete only, matching DeleteItems' own onError/cancelPoll
    // restriction — the Recycle Bin path never reaches this at all.
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
    ReportOutcome(console, left, right, leftActive, kStrVerbDelete, outcome, logger);
}

// CON-005: lets the user toggle a Windows file attribute (read-only, hidden,
// system, or archive) on the selection (or cursor entry) — the missing piece
// that kept CON-005 at Partial, since attributes were previously only ever
// touched incidentally (clearing read-only before a permanent delete).
void DoChangeAttributes(Console& console, Panel& left, Panel& right, bool leftActive, Logger& logger) {
    Panel& target = leftActive ? left : right;
    auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };

    std::vector<std::wstring> items = target.SelectionOrCursor();
    if (items.empty()) return;

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
    // copy/move/delete (FOP-010) — the missing piece that kept ERR-003 at
    // Partial, since attribute toggling was the one multi-item operation
    // that just recorded a failure and moved on with no way to retry it.
    // PER-002: live per-item progress plus a prompt Esc-cancel, same as
    // Copy/Move/Delete — a large selection no longer toggles with a frozen
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
    ReportOutcome(console, left, right, leftActive, kStrVerbToggleAttribute, outcome, logger);
}

} // namespace

int wmain(int argc, wchar_t** argv) {
    // SEC-003: checked before anything else — Console, Config, Session,
    // CommandHistory, the log file — so a hidden elevated-helper invocation
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
    if (argc >= 2) {
        // Explicit command-line paths are intentional and take precedence
        // over a cosmetic clean-exit session restore.
        leftStart = argv[1];
        if (argc >= 3) rightStart = argv[2];
        else rightStart = argv[1];
    } else {
        const SessionState previousSession = LoadSession();
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
    // modified again — see gVisibleColumns' own comment for why this is
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
    // passes only paths, Win32/error text, or setting names — never typed
    // command text, file content, or credentials.
    Logger logger(config.logLevel);
    {
        wchar_t startupMsg[64];
        swprintf_s(startupMsg, L"MyCommander %S starting", MC_VERSION_STR);
        logger.Log(LogLevel::Info, startupMsg);
    }

    // IS-0006: self-healing config file — a key EnsureConfigFileExists()
    // wouldn't have written (because the file already existed, from before
    // that key was introduced) gets appended now, with its own documented
    // default, so the file on disk stays complete rather than silently
    // relying on an in-memory default it never mentions. Startup-only —
    // see ReloadConfigLive's own comment for why a live reload doesn't
    // repeat this.
    auto addedConfigKeys = AppendMissingConfigKeys(ConfigFilePath(), loadedConfig.presentKeys);
    if (!addedConfigKeys.empty()) {
        logger.Log(LogLevel::Info, L"Config: added " + std::to_wstring(addedConfigKeys.size()) +
                                    L" missing setting(s) to " + ConfigFilePath().wstring());
    }

    Console console;
    console.SetUseBasicSymbols(config.useBasicSymbols);
    // LIST-008: main-screen panels enumerate in bounded batches so a large
    // or slow directory does not monopolize the console input loop.
    Panel left(leftStart, config.groupDirectoriesFirst, /*incrementalLoading=*/true);
    Panel right(rightStart, config.groupDirectoriesFirst, /*incrementalLoading=*/true);
    bool leftActive = true;

    if (!loadedConfig.warnings.empty()) {
        auto redraw = [&]() { DrawFrame(console, left, right, leftActive); };
        std::vector<std::wstring> lines{kStrUsingDefaultForSettings};
        lines.insert(lines.end(), loadedConfig.warnings.begin(), loadedConfig.warnings.end());
        ShowMessage(console, kStrConfigurationWarningTitle, lines, redraw);
        for (const auto& warning : loadedConfig.warnings) {
            logger.Log(LogLevel::Warning, L"Config: " + warning);
        }
    }

    // Internal command line (CLI-002/003/004): loaded from CommandHistory.h's
    // persisted file when Config::persistCommandHistory is on (the default),
    // so recall via Ctrl+Up/Down can reach commands from a previous run, not
    // just this session. CLI-007's "excluded from persistent history"
    // leading-space opt-out still skips adding a command to this in-memory
    // list at all, so it's never written out either.
    std::wstring commandLine;
    std::vector<std::wstring> commandHistory = config.persistCommandHistory ? LoadCommandHistory() : std::vector<std::wstring>{};
    int historyIndex = -1;  // -1 = not currently browsing history
    // IS-0002: files currently open in a background (non-blocking) F4
    // editor session, being watched for changes — see PollPendingEdits.
    std::vector<PendingEdit> pendingEdits;
    // IS-0003: cross-iteration drag-and-drop state — see DragState's own comment.
    DragState drag;
    // IS-0004: cross-iteration right-click/drag selection state — see SelectDragState's own comment.
    SelectDragState selectDrag;
    // IS-0006: cross-iteration config-file-edit watch state — see ConfigWatch's own comment.
    ConfigWatch configWatch;

    bool running = true;
    while (running) {
        left.PumpRefresh();
        right.PumpRefresh();
        PollPendingEdits(pendingEdits, left, right);
        PollConfigWatch(configWatch, config, console, left, right, leftActive, logger);
        if (IsTerminalTooSmall(console)) {
            DrawTooSmallMessage(console);
        } else {
            DrawFrame(console, left, right, leftActive, commandLine);
            // IS-0003: a low-cost visual cue that a drag is in progress —
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
            // handling below applies — a mouse click can't do anything
            // meaningful once the too-small message has replaced the panels.
            if (!IsTerminalTooSmall(console)) {
                HandleMouseEvent(console, left, right, leftActive, commandLine, config, logger, pendingEdits, drag,
                                 selectDrag, record.Event.MouseEvent);
            }
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

        // UI-006: while below the minimum size, only quitting is honored —
        // everything else (dialogs, the command line, cursor movement) needs
        // more room than the too-small screen has to draw correctly.
        if (IsTerminalTooSmall(console)) {
            if (record.Event.KeyEvent.wVirtualKeyCode == VK_F10) running = false;
            continue;
        }

        Panel& target = leftActive ? left : right;
        int visibleRows =
            ComputeVisibleRows(console, GetHintContext(target, commandLine), !BuildStatusLine(target).empty());
        bool shiftHeld = (record.Event.KeyEvent.dwControlKeyState & SHIFT_PRESSED) != 0;
        bool ctrlHeld = (record.Event.KeyEvent.dwControlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
        bool altHeld = (record.Event.KeyEvent.dwControlKeyState & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED)) != 0;
        auto appendToCommandLine = [&](wchar_t c) {
            if (c >= 0x20 && commandLine.size() < 240) {
                commandLine.push_back(c);
                historyIndex = -1;
            }
        };
        WORD key = record.Event.KeyEvent.wVirtualKeyCode;
        // TST-005: navigation keys' decisions (Tab/panel history/command
        // history vs. cursor movement/Backspace) are classified by the
        // pure, independently-tested ClassifyNavigationKey — each case
        // below only carries out the intent, still owning any state
        // mutation (historyIndex, commandLine) that's local to this loop.
        NavKeyContext navCtx{ctrlHeld, altHeld, commandLine.empty(), !commandHistory.empty(), historyIndex != -1};
        switch (key) {
            case VK_TAB:
                if (ClassifyNavigationKey(key, navCtx) == NavIntent::SwitchActivePanel) leftActive = !leftActive;
                break;
            case VK_LEFT:
                if (ClassifyNavigationKey(key, navCtx) == NavIntent::PanelHistoryBack) target.NavigateBack();
                break;
            case VK_RIGHT:
                if (ClassifyNavigationKey(key, navCtx) == NavIntent::PanelHistoryForward) target.NavigateForward();
                break;
            case VK_UP:
                switch (ClassifyNavigationKey(key, navCtx)) {
                    case NavIntent::CommandHistoryUp:
                        // CLI-004: browse older commands without disturbing the file cursor.
                        if (historyIndex == -1) historyIndex = static_cast<int>(commandHistory.size()) - 1;
                        else if (historyIndex > 0) --historyIndex;
                        commandLine = commandHistory[historyIndex];
                        break;
                    case NavIntent::CursorUp:
                        target.MoveCursor(-1, visibleRows);
                        break;
                    default:
                        break;
                }
                break;
            case VK_DOWN:
                switch (ClassifyNavigationKey(key, navCtx)) {
                    case NavIntent::CommandHistoryDown:
                        if (historyIndex + 1 < static_cast<int>(commandHistory.size())) {
                            ++historyIndex;
                            commandLine = commandHistory[historyIndex];
                        } else {
                            historyIndex = -1;
                            commandLine.clear();
                        }
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
                target.MoveCursor(-static_cast<int>(target.Entries().size()), visibleRows);
                break;
            case VK_END:
                target.MoveCursor(static_cast<int>(target.Entries().size()), visibleRows);
                break;
            case VK_RETURN:
                if (ctrlHeld) {
                    // CLI-003: insert the active/selected name(s), or with
                    // Shift held, the active path, without running anything.
                    InsertActiveReference(target, /*fullPath=*/shiftHeld, commandLine);
                    historyIndex = -1;
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
                } else {
                    ActivateCursorEntry(console, left, right, leftActive, config, logger, pendingEdits);
                }
                break;
            case VK_BACK:
                if (ClassifyNavigationKey(key, navCtx) == NavIntent::ClearCommandLineChar) {
                    commandLine.pop_back();
                    historyIndex = -1;
                } else {
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
                } else {
                    // KEY-004: plain F1 (no modifier is otherwise bound to it) opens the
                    // in-application shortcut reference. Like DoView/DoEdit, no manual
                    // redraw here — the main loop's next iteration redraws the frame.
                    ShowShortcutReference(console);
                }
                break;
            case VK_F2:
                if (altHeld) {
                    DoSelectDrive(console, left, right, leftActive, /*selectLeftPanel=*/false);
                } else {
                    DoRename(console, left, right, leftActive);
                }
                break;
            case VK_F3:
                if (ctrlHeld) {
                    DoSort(target, SortKey::Name);
                } else {
                    DoView(console, left, right, leftActive);
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
            case VK_F5:
                if (ctrlHeld) {
                    DoSort(target, SortKey::Size);
                } else {
                    DoCopyOrMove(console, left, right, leftActive, /*isMove=*/false, logger);
                }
                break;
            case VK_F6:
                if (ctrlHeld) {
                    DoSort(target, SortKey::ModifiedTime);
                } else {
                    DoCopyOrMove(console, left, right, leftActive, /*isMove=*/true, logger);
                }
                break;
            case VK_F8:
                DoDelete(console, left, right, leftActive, /*permanent=*/shiftHeld, config, logger);
                break;
            case VK_F9:
                DoOpenShell(console, left, right, leftActive);
                break;
            case VK_ESCAPE:
                // IS-0005: Esc no longer quits the app when the command
                // line is already empty — F10 is the sole quit key now.
                if (!commandLine.empty()) {
                    commandLine.clear();
                    historyIndex = -1;
                }
                break;
            case VK_F10:
                running = false;
                break;
            case VK_F11:
                // IS-0006: plain F11 is reserved by most terminal hosts
                // (Windows Terminal included) as a window-fullscreen
                // toggle and never reaches the app at all — confirmed by
                // the user, the same class of host interception IS-0004
                // hit with Shift-held mouse clicks. Ctrl+F11 isn't
                // reserved that way.
                if (ctrlHeld) DoEditConfig(console, left, right, leftActive, logger, configWatch);
                break;
            default:
                appendToCommandLine(record.Event.KeyEvent.uChar.UnicodeChar);
                break;
        }
    }

    // NAV-008: this is the sole write point for the session file. Paths are
    // captured only after an explicit, normal loop exit (F10, IS-0005's sole quit key), never
    // during a file operation or from an abnormal-termination handler.
    SaveSession(SessionState{left.Path(), right.Path()});
    // CLI-004: same normal-exit-only write point as the session file, but
    // gated on the user's own setting — persistCommandHistory defaulting to
    // true is what makes history survive across runs at all.
    if (config.persistCommandHistory) SaveCommandHistory(commandHistory);
    logger.Log(LogLevel::Info, L"MyCommander exiting normally");
    CoUninitialize();
    return 0;
}
