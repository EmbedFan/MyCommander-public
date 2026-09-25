#pragma once

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include "Log.h"

#include <filesystem>
#include <set>
#include <string>
#include <vector>

namespace mc {

// VEE-002: what pressing Enter on a file (with an empty command line, and
// no other more specific handler — F3/F4 always mean View/Edit regardless
// of this) does. View (the historical, hardcoded behavior) is the default
// so a missing config file changes nothing (CFG-003).
enum class EnterFileAction { View, Edit, Execute };

// UI-005: an optional panel-row column. Name itself isn't included here —
// it's always shown first and isn't a meaningful "hide" choice, unlike
// these four.
enum class ColumnId { Type, Attributes, ModifiedTime, Size };

// UI-005: the minimum width each column's own content needs to display
// without truncation — see Config::typeColumnWidth and friends. Shared
// between Config.cpp (validates a configured value against these) and
// anything else that needs to know the floor (e.g. tests).
constexpr int kMinTypeColumnWidth = 6;
constexpr int kMinMtimeColumnWidth = 16;
constexpr int kMinSizeColumnWidth = 10;
constexpr int kMaxColumnWidth = 200;  // a generous ceiling against a typo like a missing digit

// UI-010: which named color theme (Theme.h's ThemePalette) the app draws
// with. Default is the historical, always-shipped palette, so a missing
// config file changes nothing (CFG-003).
enum class ColorTheme { Default, HighContrast };

// FOP-008: which destructive operations require an explicit Yes/No
// confirmation before proceeding. Both default to true (safe) so a
// missing config file, or one where a key is absent, never silently
// disables a safety prompt (CFG-003).
//
// ERR-006: this struct — and the file it's loaded from — must only ever
// hold user-set *preferences*, never panel paths, selection state, or
// anything describing an in-progress or interrupted file operation. It's
// read unconditionally on every startup, crash-recovery included, so if a
// field here ever influenced whether a file operation ran (rather than
// just whether the app *asks* before running one), a crash mid-operation
// could make the next launch silently repeat it. Session/path restoration
// (NAV-008) lives in a separate, purely-cosmetic store written only at a
// clean exit — not in this struct.
struct Config {
    bool confirmRecycleBinDelete = true;
    bool confirmPermanentDelete = true;
    EnterFileAction enterFileAction = EnterFileAction::View;
    // LIST-004: retain the familiar directories-before-files ordering by
    // default, while allowing a user to sort all entries in one sequence.
    bool groupDirectoriesFirst = true;
    // UI-014/ACC-006: selects the portable +, -, | frame style instead of
    // the default Unicode CP437-compatible box-drawing style.
    bool useBasicSymbols = false;
    // CLI-004: whether the internal command line's history (CommandHistory.h)
    // is loaded at startup and saved at a normal exit, so it survives across
    // runs, rather than staying session-only. Defaults on, matching how a
    // shell's own history file behaves; CLI-007's leading-space opt-out
    // already keeps a specific command out of history regardless of this.
    bool persistCommandHistory = true;
    // ERR-004: the diagnostic log's (Log.h) severity threshold. Off by
    // default — a fresh install never writes a log file until the user
    // opts in, matching this project's general preference for the
    // conservative default (CFG-003).
    LogLevel logLevel = LogLevel::Off;
    // UI-005: which optional columns a panel row shows, and in what order
    // — defaults to every column, in the same order the app always showed
    // them before this was configurable. DrawPanel's narrow-terminal
    // graceful degradation (UI-004) drops columns from the *end* of this
    // list under space pressure, so the chosen order doubles as a drop
    // priority.
    std::vector<ColumnId> visibleColumns = {ColumnId::Type, ColumnId::Attributes, ColumnId::ModifiedTime,
                                            ColumnId::Size};
    // UI-005: Type/ModifiedTime/Size column widths, in character cells.
    // Attributes stays a fixed 4 (one slot per R/H/S/A flag; narrower would
    // truncate a real flag letter, wider is only ever padding), so it isn't
    // configurable. Each has a hard minimum — below it the column can't
    // show its own content without truncation (ModifiedTime's fixed
    // "YYYY-MM-DD HH:MM" format is exactly 16 characters; Type's extension
    // is capped at 6; Size defaults to 10, generous for real file sizes) —
    // LoadConfigFrom rejects (CFG-005 warning, keeps the default) rather
    // than silently clamping a too-small configured value.
    int typeColumnWidth = 6;
    int mtimeColumnWidth = 16;
    int sizeColumnWidth = 10;
    // UI-010: which named color theme to draw with — see ColorTheme above.
    ColorTheme colorTheme = ColorTheme::Default;
    // IS-0002: false (default — a deliberate default-behavior *change*, see
    // the fix plan) launches F4's external editor without waiting for it to
    // close; MyCommander stays usable immediately, and the edited file's
    // panel entry is refreshed and re-selected automatically once a file-
    // change notification fires. true restores the original behavior
    // (block until the editor closes, then refresh) — needed for a
    // console-subsystem editor sharing this console window, which the
    // non-blocking path doesn't hand off to cleanly.
    bool waitForEditorToClose = false;
};

// CFG-005: one human-readable line per setting LoadConfigFrom couldn't
// parse — the setting name and the invalid value it found — so a typo
// is reported instead of silently guessed at or ignored.
struct LoadConfigResult {
    Config config;
    std::vector<std::wstring> warnings;
    // IS-0006: which recognized keys the file actually had a line for,
    // regardless of whether that line's value parsed successfully — lets a
    // caller (AppendMissingConfigKeys) tell "never mentioned in the file"
    // apart from "mentioned but with a typo'd value" (the latter already
    // has its own, separate handling via `warnings` above).
    std::set<std::wstring> presentKeys;
};

// CFG-002: the per-user config file location (roaming, so it follows the
// user across machines the way Windows apps conventionally do) — empty if
// %APPDATA% can't be resolved, in which case callers fall back to defaults.
std::filesystem::path ConfigFilePath();

// CFG-001/CFG-003/CFG-004/CFG-005: parses `path` as "key = value" lines
// (blank lines and lines starting with '#' or ';' ignored). A missing
// file, or one that exists but is empty, both yield Config{}'s defaults
// with no warnings (CFG-003) — that's the expected first-run state, not
// an error. An unrecognized key is silently ignored (CFG-004) rather than
// blocking startup; a recognized key with a value that isn't a valid
// boolean is reported in the result's `warnings` and that field keeps its
// default (CFG-005).
LoadConfigResult LoadConfigFrom(const std::filesystem::path& path);

// Convenience wrapper: LoadConfigFrom(ConfigFilePath()).
LoadConfigResult LoadConfig();

// Writes a fresh, fully-commented default config file at `path` if one
// doesn't already exist there — so CFG-001's "documented" is genuinely
// discoverable by opening the file, rather than requiring the user to
// already know the exact key names to type. A no-op if `path` is already
// present, or if its parent directory can't be created.
void EnsureConfigFileExistsAt(const std::filesystem::path& path);

// Convenience wrapper: EnsureConfigFileExistsAt(ConfigFilePath()).
void EnsureConfigFileExists();

// IS-0006: appends every currently-known setting not in `presentKeys` to the
// existing file at `path`, each with its own default value and the same
// documentation comment EnsureConfigFileExistsAt would give it in a brand
// new file — keeps an older config file (written before a newer setting
// existed) self-healing/self-documenting rather than silently relying on an
// in-memory default the file itself never mentions. No-op (returns an empty
// list) if nothing's missing, `path` is empty, or the file can't be opened
// for append.
std::vector<std::wstring> AppendMissingConfigKeys(const std::filesystem::path& path,
                                                   const std::set<std::wstring>& presentKeys);

}  // namespace mc
