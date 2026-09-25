#include "Config.h"
#include "Strings.h"

#include <algorithm>
#include <cstdlib>
#include <cwctype>
#include <fstream>

namespace mc {

namespace {

std::wstring Trim(const std::wstring& s) {
    size_t start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";
    size_t end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

// Parses a boolean setting value ("true"/"false"/"yes"/"no"/"1"/"0",
// case-insensitive); sets `ok` to false for anything else so the caller
// can report it (CFG-005) instead of silently guessing.
bool ParseBool(const std::wstring& value, bool& ok) {
    std::wstring lower = ToLower(value);
    ok = true;
    if (lower == L"true" || lower == L"1" || lower == L"yes") return true;
    if (lower == L"false" || lower == L"0" || lower == L"no") return false;
    ok = false;
    return false;
}

// Parses "view"/"edit"/"execute" (case-insensitive); sets `ok` to false for
// anything else, same convention as ParseBool.
EnterFileAction ParseEnterFileAction(const std::wstring& value, bool& ok) {
    std::wstring lower = ToLower(value);
    ok = true;
    if (lower == L"view") return EnterFileAction::View;
    if (lower == L"edit") return EnterFileAction::Edit;
    if (lower == L"execute") return EnterFileAction::Execute;
    ok = false;
    return EnterFileAction::View;
}

// UI-005: parses a comma-separated, ordered list of column names ("type",
// "attr", "mtime", "size", case-insensitive; a repeated name is collapsed
// to its first occurrence, since repeating it wouldn't do anything). An
// empty value (after trimming) is valid and means "no optional columns,
// name only" — not an error, same as CFG-003's general "absent/empty is a
// legitimate choice, not a mistake" stance. Sets `ok` to false only for an
// unrecognized token.
std::vector<ColumnId> ParseColumnList(const std::wstring& value, bool& ok) {
    ok = true;
    std::vector<ColumnId> result;
    std::wstring trimmedValue = Trim(value);
    if (trimmedValue.empty()) return result;

    size_t start = 0;
    while (start <= trimmedValue.size()) {
        size_t comma = trimmedValue.find(L',', start);
        size_t tokenLen = (comma == std::wstring::npos) ? std::wstring::npos : comma - start;
        std::wstring token = ToLower(Trim(trimmedValue.substr(start, tokenLen)));

        ColumnId id;
        if (token == L"type") {
            id = ColumnId::Type;
        } else if (token == L"attr") {
            id = ColumnId::Attributes;
        } else if (token == L"mtime") {
            id = ColumnId::ModifiedTime;
        } else if (token == L"size") {
            id = ColumnId::Size;
        } else {
            ok = false;
            return {};
        }
        if (std::find(result.begin(), result.end(), id) == result.end()) result.push_back(id);

        if (comma == std::wstring::npos) break;
        start = comma + 1;
    }
    return result;
}

// UI-005: parses a positive integer in [minWidth, kMaxColumnWidth]; sets
// `ok` to false for anything else (non-numeric, out of range). A too-small
// value is rejected rather than silently clamped — silently showing a
// truncated column the user didn't ask for is worse than keeping the
// documented default and reporting why (CFG-005).
int ParseColumnWidth(const std::wstring& value, int minWidth, bool& ok) {
    ok = false;
    if (value.empty()) return minWidth;
    wchar_t* end = nullptr;
    long parsed = std::wcstol(value.c_str(), &end, 10);
    if (end != value.c_str() + value.size() || parsed < minWidth || parsed > kMaxColumnWidth) return minWidth;
    ok = true;
    return static_cast<int>(parsed);
}

// Parses "default"/"highcontrast" (case-insensitive); sets `ok` to false
// for anything else, same convention as ParseEnterFileAction.
ColorTheme ParseColorTheme(const std::wstring& value, bool& ok) {
    std::wstring lower = ToLower(value);
    ok = true;
    if (lower == L"default") return ColorTheme::Default;
    if (lower == L"highcontrast") return ColorTheme::HighContrast;
    ok = false;
    return ColorTheme::Default;
}

// IS-0006: one recognized key plus the exact comment-plus-"key = default"
// text block that setting gets in a brand new config file.
struct ConfigKeyBlock {
    std::wstring key;
    std::wstring text;
};

// IS-0006: one entry per LoadConfigFrom-recognized key, each holding the
// exact comment-plus-"key = default"-line block that setting gets in a
// brand new config file — the single source of truth both
// EnsureConfigFileExistsAt (joins every block) and AppendMissingConfigKeys
// (appends only the ones `presentKeys` doesn't already have) build from,
// so the two can never document a setting differently. Order matches the
// file's existing, documented layout. Every block ends with its own
// "# Values: ..." line stating exactly what's acceptable and the default,
// so a block is self-contained even if it's ever read in isolation (e.g.
// appended by itself, far from the file's own header, by
// AppendMissingConfigKeys). typeColumnWidth's block additionally carries
// the shared prose explaining all three column-width settings together;
// mtimeColumnWidth/sizeColumnWidth each still get their own one-line
// "Values:" comment so their own constraint is never bare.
const std::vector<ConfigKeyBlock>& ConfigKeyBlocks() {
    static const std::vector<ConfigKeyBlock> blocks = {
        {L"confirmrecyclebindelete",
         L"# Safety rules (FOP-008): ask Yes/No before sending the selected item(s) to\n"
         L"# the Recycle Bin (F8).\n"
         L"# Values: true / false (default: true)\n"
         L"confirmRecycleBinDelete = true\n"},
        {L"confirmpermanentdelete",
         L"# Ask Yes/No before permanently deleting the selected item(s), bypassing\n"
         L"# the Recycle Bin (Shift+F8). Strongly recommended to leave enabled.\n"
         L"# Values: true / false (default: true)\n"
         L"confirmPermanentDelete = true\n"},
        {L"enterfileaction",
         L"# What pressing Enter on a file does (VEE-002). F3/F4 always mean\n"
         L"# View/Edit regardless of this setting; this only controls the plain\n"
         L"# Enter key's default action.\n"
         L"#   view    - open it in the built-in viewer (default)\n"
         L"#   edit    - open it in the external editor (%EDITOR%, or notepad.exe)\n"
         L"#   execute - run/open it with its own Windows file association, the\n"
         L"#             same as double-clicking it in Explorer (VEE-008: only ever\n"
         L"#             happens because you explicitly pressed Enter after choosing\n"
         L"#             this setting -- never automatically)\n"
         L"# Values: view / edit / execute (default: view)\n"
         L"enterFileAction = view\n"},
        {L"groupdirectoriesfirst",
         L"# Keep directories before files for every sort key (LIST-004). Set false to\n"
         L"# sort directories and files together by the active key.\n"
         L"# Values: true / false (default: true)\n"
         L"groupDirectoriesFirst = true\n"},
        {L"usebasicsymbols",
         L"# Use plain 7-bit ASCII (+, -, |) for panel/dialog frames instead of\n"
         L"# Unicode box-drawing characters. Set true only if the active terminal\n"
         L"# font cannot render box-drawing glyphs reliably (UI-014/ACC-006).\n"
         L"# Values: true / false (default: false)\n"
         L"useBasicSymbols = false\n"},
        {L"persistcommandhistory",
         L"# Save the internal command line's history (Ctrl+Up/Ctrl+Down) so it\n"
         L"# survives across runs, instead of it being session-only (CLI-004). A\n"
         L"# command typed with a leading space is still never added to history,\n"
         L"# persisted or not (CLI-007).\n"
         L"# Values: true / false (default: true)\n"
         L"persistCommandHistory = true\n"},
        {L"loglevel",
         L"# Diagnostic log severity (ERR-004). Off by default -- no log file is\n"
         L"# written at all until you opt in. Each level also includes every level\n"
         L"# above it (warning also logs error, and so on):\n"
         L"#   off     - no logging (default)\n"
         L"#   error   - failed file operations only\n"
         L"#   warning - also config-file problems (a bad line/value, CFG-005)\n"
         L"#   info    - also app start/exit\n"
         L"#   debug   - most verbose\n"
         L"# Written to %APPDATA%\\MyCommander\\mycommander.log. Never contains typed\n"
         L"# command text, file content, or credentials (ERR-005) -- only paths,\n"
         L"# error text, and setting names.\n"
         L"# Values: off / error / warning / info / debug (default: off)\n"
         L"logLevel = off\n"},
        {L"visiblecolumns",
         L"# Which optional panel-row columns to show, and in what order (UI-005).\n"
         L"# Name is always shown first and isn't included here. A comma-separated\n"
         L"# list from: type, attr, mtime, size -- leave empty to show only the name\n"
         L"# column. On a narrow terminal, columns drop out gracefully from the end\n"
         L"# of this list (last listed = first dropped) rather than squeezing the\n"
         L"# name unreadably thin.\n"
         L"# Values: a comma-separated list from type, attr, mtime, size, in any\n"
         L"# order, or empty (default: type,attr,mtime,size)\n"
         L"visibleColumns = type,attr,mtime,size\n"},
        {L"typecolumnwidth",
         L"# Column widths in character cells (UI-005). attr is always a fixed 4\n"
         L"# (one slot per read-only/hidden/system/archive flag) and isn't\n"
         L"# configurable. Each of these has a minimum below which its own content\n"
         L"# wouldn't fit without truncation -- type >= 6, mtime >= 16 (the fixed\n"
         L"# \"YYYY-MM-DD HH:MM\" format), size >= 10.\n"
         L"# Values: an integer, 6-200 (default: 6)\n"
         L"typeColumnWidth = 6\n"},
        {L"mtimecolumnwidth", L"# Values: an integer, 16-200 (default: 16)\nmtimeColumnWidth = 16\n"},
        {L"sizecolumnwidth", L"# Values: an integer, 10-200 (default: 10)\nsizeColumnWidth = 10\n"},
        {L"colortheme",
         L"# Color theme (UI-010):\n"
         L"#   default      - the app's usual colors (default)\n"
         L"#   highcontrast - maximizes contrast wherever the default theme only just\n"
         L"#                  passed WCAG AA (e.g. plain text goes from light gray to\n"
         L"#                  bright white, and the status bar drops its green\n"
         L"#                  background in favor of black) -- for a terminal font/\n"
         L"#                  color scheme where the default theme is hard to read\n"
         L"# Values: default / highcontrast (default: default)\n"
         L"colorTheme = default\n"},
        {L"waitforeditortoclose",
         L"# What F4 (edit) does while the external editor is open. false (default)\n"
         L"# launches it and returns immediately -- MyCommander stays usable right\n"
         L"# away, and the file's panel entry refreshes and stays selected\n"
         L"# automatically once you save. Set true to restore the original\n"
         L"# behavior (wait for the editor to close before refreshing) -- needed if\n"
         L"# %EDITOR% is a console-based program sharing this same window, which the\n"
         L"# non-blocking mode does not hand off to cleanly.\n"
         L"# Values: true / false (default: false)\n"
         L"waitForEditorToClose = false\n"},
    };
    return blocks;
}

}  // namespace

std::filesystem::path ConfigFilePath() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path(buf) / L"MyCommander" / L"mycommander.ini";
}

LoadConfigResult LoadConfigFrom(const std::filesystem::path& path) {
    LoadConfigResult result;
    if (path.empty()) return result;

    std::wifstream in(path);
    if (!in) return result;  // CFG-003: no file (or can't open it) -> defaults, no warning

    std::wstring line;
    int lineNumber = 0;
    while (std::getline(in, line)) {
        ++lineNumber;
        std::wstring trimmed = Trim(line);
        if (trimmed.empty() || trimmed.front() == L'#' || trimmed.front() == L';') continue;

        size_t eq = trimmed.find(L'=');
        if (eq == std::wstring::npos) {
            result.warnings.push_back(FormatConfigLineSyntaxWarning(lineNumber, trimmed));
            continue;
        }
        std::wstring key = ToLower(Trim(trimmed.substr(0, eq)));
        std::wstring value = Trim(trimmed.substr(eq + 1));

        bool ok = false;
        std::wstring expected = L"true/false";
        if (key == L"confirmrecyclebindelete") {
            bool parsed = ParseBool(value, ok);
            if (ok) result.config.confirmRecycleBinDelete = parsed;
        } else if (key == L"confirmpermanentdelete") {
            bool parsed = ParseBool(value, ok);
            if (ok) result.config.confirmPermanentDelete = parsed;
        } else if (key == L"enterfileaction") {
            expected = L"view/edit/execute";
            EnterFileAction parsed = ParseEnterFileAction(value, ok);
            if (ok) result.config.enterFileAction = parsed;
        } else if (key == L"groupdirectoriesfirst") {
            bool parsed = ParseBool(value, ok);
            if (ok) result.config.groupDirectoriesFirst = parsed;
        } else if (key == L"usebasicsymbols") {
            bool parsed = ParseBool(value, ok);
            if (ok) result.config.useBasicSymbols = parsed;
        } else if (key == L"persistcommandhistory") {
            bool parsed = ParseBool(value, ok);
            if (ok) result.config.persistCommandHistory = parsed;
        } else if (key == L"loglevel") {
            expected = L"off/error/warning/info/debug";
            LogLevel parsed = LogLevel::Off;
            ok = ParseLogLevel(value, parsed);
            if (ok) result.config.logLevel = parsed;
        } else if (key == L"visiblecolumns") {
            expected = L"a comma-separated list from: type, attr, mtime, size (or empty for name only)";
            std::vector<ColumnId> parsed = ParseColumnList(value, ok);
            if (ok) result.config.visibleColumns = parsed;
        } else if (key == L"typecolumnwidth") {
            expected = L"an integer >= " + std::to_wstring(kMinTypeColumnWidth);
            int parsed = ParseColumnWidth(value, kMinTypeColumnWidth, ok);
            if (ok) result.config.typeColumnWidth = parsed;
        } else if (key == L"mtimecolumnwidth") {
            expected = L"an integer >= " + std::to_wstring(kMinMtimeColumnWidth);
            int parsed = ParseColumnWidth(value, kMinMtimeColumnWidth, ok);
            if (ok) result.config.mtimeColumnWidth = parsed;
        } else if (key == L"sizecolumnwidth") {
            expected = L"an integer >= " + std::to_wstring(kMinSizeColumnWidth);
            int parsed = ParseColumnWidth(value, kMinSizeColumnWidth, ok);
            if (ok) result.config.sizeColumnWidth = parsed;
        } else if (key == L"colortheme") {
            expected = L"default/highcontrast";
            ColorTheme parsed = ParseColorTheme(value, ok);
            if (ok) result.config.colorTheme = parsed;
        } else if (key == L"waitforeditortoclose") {
            bool parsed = ParseBool(value, ok);
            if (ok) result.config.waitForEditorToClose = parsed;
        } else {
            continue;  // CFG-004: unrecognized key, ignored rather than blocking startup
        }

        // IS-0006: the key was recognized (every branch above except the
        // unrecognized-key one, which already `continue`d past this point)
        // -- present regardless of whether its value also parsed, since a
        // typo'd value is a separate, already-reported problem from the
        // key being absent altogether.
        result.presentKeys.insert(key);

        if (!ok) {
            result.warnings.push_back(FormatConfigInvalidValueWarning(lineNumber, key, value, expected));
        }
    }
    return result;
}

LoadConfigResult LoadConfig() { return LoadConfigFrom(ConfigFilePath()); }

void EnsureConfigFileExistsAt(const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) return;

    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return;

    std::wofstream out(path);
    if (!out) return;
    out << L"# MyCommander configuration\n"
           L"# Lines starting with # or ; are comments. Boolean values: true/false.\n"
           L"\n";
    const auto& blocks = ConfigKeyBlocks();
    for (size_t i = 0; i < blocks.size(); ++i) {
        out << blocks[i].text;
        if (i + 1 < blocks.size()) out << L"\n";
    }
}

void EnsureConfigFileExists() { EnsureConfigFileExistsAt(ConfigFilePath()); }

std::vector<std::wstring> AppendMissingConfigKeys(const std::filesystem::path& path,
                                                   const std::set<std::wstring>& presentKeys) {
    std::vector<std::wstring> missing;
    for (const auto& block : ConfigKeyBlocks()) {
        if (presentKeys.find(block.key) == presentKeys.end()) missing.push_back(block.key);
    }
    if (missing.empty() || path.empty()) return {};

    std::wofstream out(path, std::ios::app);
    if (!out) return {};
    out << L"\n# --- Added automatically: setting(s) missing from this file (IS-0006) ---\n";
    for (const auto& block : ConfigKeyBlocks()) {
        if (presentKeys.find(block.key) == presentKeys.end()) out << L"\n" << block.text;
    }
    return missing;
}

}  // namespace mc
