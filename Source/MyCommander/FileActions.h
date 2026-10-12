#pragma once

// FEA-0009 (VEE-009..013): per-extension file actions -- which program F3
// (View), Shift+Enter (Open) and the Shift+F3 chooser run for a given file.
// Pure logic (no Console/Dialog dependency, CLAUDE.md's pure-module
// precedent): parsing the user's file-actions.conf, matching a file name
// against it, picking a default, expanding argument placeholders, and the
// line-level text edits Phase 2's in-app manager ("Assign File to App")
// makes, so a user's own comments and layout are never rewritten.
//
// DEC-011: stored in its own %APPDATA%\MyCommander\file-actions.conf rather
// than as keys in mycommander.ini -- it's a list of multi-field records
// (several per extension), which mycommander.ini's flat one-line-per-key
// machinery (AppendMissingConfigKeys, UpdateConfigValue, IS-0006's live
// reload) doesn't fit. Same DEC-007 spirit: hand-rolled, dependency-free,
// '#'/';' comments. Format, one action per line, grouped in blocks:
//
//   [.txt .log *.tar.gz readme*]
//   view* = Built-in viewer | internal:viewer |
//   open  = Notepad         | notepad.exe     | "%f"
//
// '|' separates fields because Windows forbids it in file names, so it can
// never be part of a real program path.

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mc {

// D5: adding a type (e.g. Edit for F4) = one value here + one row in
// FileActions.cpp's keyword table. Unknown keywords in the file are skipped
// with a warning, never fatal (CFG-004/005), so a file written for a newer
// version still loads.
enum class FileActionType { View, Open };

constexpr const wchar_t* kFileActionProgramInternalViewer = L"internal:viewer";
constexpr const wchar_t* kFileActionProgramSystem = L"system:";
// EXT-003: "plugin:<plugin-id>" -- a viewer plugin (kMcPluginCapViewer);
// valid only for view actions.
constexpr const wchar_t* kFileActionProgramPluginPrefix = L"plugin:";

struct FileAction;
// The plugin id of a "plugin:<id>" program, or nullopt for any other program.
std::optional<std::wstring> FileActionPluginId(const FileAction& action);

struct FileAction {
    FileActionType type = FileActionType::View;
    bool isDefault = false;
    std::wstring description;
    std::wstring program;    // a path or bare name (PATH lookup), or one of the two kFileActionProgram* tokens
    std::wstring arguments;  // template: %f %n %b %e %d %%
    int sourceLine = 0;      // 1-based line in the text it was parsed from; 0 = not from a file
};

struct FileActionBlock {
    std::vector<std::wstring> patterns;  // ".txt", "*.tar.gz", "readme*", "*"
    std::vector<FileAction> actions;
    int headerLine = 0;
};

struct FileActionTable {
    std::vector<FileActionBlock> blocks;
};

struct ParseFileActionsResult {
    FileActionTable table;
    std::vector<std::wstring> warnings;  // CFG-005 style: line number + what was wrong
};

// "view" / "open".
const wchar_t* FileActionTypeKeyword(FileActionType type);
// Inverse of FileActionTypeKeyword, case-insensitive; nullopt for an unknown keyword.
std::optional<FileActionType> ParseFileActionTypeKeyword(const std::wstring& keyword);

ParseFileActionsResult ParseFileActions(const std::wstring& text);

// Reads `path` as UTF-8 (BOM optional). A missing or empty file is an empty
// table with no warnings (CFG-003) -- F3/Enter then behave exactly as before.
ParseFileActionsResult LoadFileActionsFrom(const std::filesystem::path& path);

// %APPDATA%\MyCommander\file-actions.conf, or empty if %APPDATA% can't be resolved.
std::filesystem::path FileActionsFilePath();

// The fully-commented first-run sample (CFG-001: discoverable by opening the file).
std::wstring DefaultFileActionsText();

// True if `text` is the 1.11.0 first-run sample left as written: its only
// active lines are that sample's "[.txt .log .md .ini .conf]" /
// "view* = Built-in viewer | internal:viewer |" pair, and every comment line
// is one of that sample's own (whitespace and line endings aside).
bool IsUnmodifiedLegacyFileActionsSample(const std::wstring& text);

// The 1.11.0 sample's one active block -- "[.txt .log .md .ini .conf]" with
// only "view* = Built-in viewer | internal:viewer |" under it -- merely
// restated the built-in fallback, yet as an early matching block it beat a
// user's own later .txt/.md blocks. Returns `text` with that exact block
// (and its preceding blank line) removed, or the whole current sample if
// `text` is IsUnmodifiedLegacyFileActionsSample; otherwise `text` unchanged.
// Nothing else in the file is touched.
std::wstring MigrateLegacyFileActionsText(const std::wstring& text);

// Writes DefaultFileActionsText() at `path` if nothing exists there yet; an
// existing file only ever gets MigrateLegacyFileActionsText applied (saved
// only if that changed it). A no-op on any failure.
void EnsureFileActionsFileExistsAt(const std::filesystem::path& path);

// Whether one block pattern matches `fileName`: ".ext" (no wildcard)
// matches that extension or that exact whole name (so ".gitignore" works);
// a pattern with '*'/'?' is a case-insensitive glob over the whole name;
// anything else must equal the whole name, case-insensitively.
bool FileActionPatternMatches(const std::wstring& pattern, const std::wstring& fileName);

// Every action of `type` from every block with a pattern matching
// `fileName`, in file order (D2 step 1).
std::vector<FileAction> FindFileActions(const FileActionTable& table, const std::wstring& fileName,
                                        FileActionType type);

// The first block (in file order) that matches `fileName` *and* has an
// action of `type` decides; within it, its '*'-marked action, else its first
// one. nullopt when no block qualifies -- the caller then applies the
// built-in fallback (D2 step 3).
std::optional<FileAction> DefaultFileAction(const FileActionTable& table, const std::wstring& fileName,
                                            FileActionType type);

// Expands %f (full path) %n (name) %b (name without extension) %e
// (extension without the dot) %d (directory) %% ('%'); an unknown %x is kept
// literally. If the template contains none of %f/%n/%b, a quoted "%f" is
// appended so the program always receives the file.
std::wstring ExpandFileActionArguments(const std::wstring& argumentTemplate, const std::filesystem::path& file);

// A display label for a chooser/manager row: the description, or the
// program's own file name when no description was given.
std::wstring FileActionLabel(const FileAction& action);

// The one canonical line FEA-0009 Phase 2 writes for `action`.
std::wstring FormatFileActionLine(const FileAction& action);

// --- Phase 2: line-level edits on the file's text ---------------------------
// Each returns the edited text; every line it doesn't need to touch -- the
// user's comments, blank lines, other blocks -- is preserved byte-for-byte.

// Appends `action` to the first block dedicated to exactly `pattern` alone
// ("[.md]", case-insensitive). A block shared with other patterns
// ("[.txt .md]") is never edited -- that would change the other extensions
// too; instead a new "[pattern]" block is inserted just before the first
// such shared block (so its default wins for `pattern`, first-default-wins),
// or appended at the end if no block lists `pattern` at all. If
// action.isDefault, every other same-type action in the dedicated block
// loses its '*'.
std::wstring AddFileActionLine(const std::wstring& text, const std::wstring& pattern, const FileAction& action);

// Removes 1-based line `sourceLine` (an action line). Out of range: unchanged.
std::wstring RemoveFileActionLine(const std::wstring& text, int sourceLine);

// Marks the action on line `sourceLine` as the default of its type within
// its own block, and unmarks every other same-type action in that block.
std::wstring SetDefaultFileActionLine(const std::wstring& text, int sourceLine);

// What Assign File to App's "Default" does for `pattern`. For an action in
// a block dedicated to `pattern`: SetDefaultFileActionLine. For one in a
// shared block (which must not be edited -- that would change the other
// patterns too): marks an identical copy in `pattern`'s own block as its
// default, or adds such a copy via AddFileActionLine (whose new dedicated
// block lands above the shared one, so it decides for `pattern`).
std::wstring SetDefaultFileActionForPattern(const std::wstring& text, const std::wstring& pattern, int sourceLine);

// Writes `text` to `path` as UTF-8 via a temp file + MoveFileExW (the
// Session/CommandHistory atomic-write precedent). False on any failure.
bool SaveFileActionsText(const std::filesystem::path& path, const std::wstring& text);

// Reads `path` as UTF-8 text (BOM stripped); empty string if missing/unreadable.
std::wstring ReadFileActionsText(const std::filesystem::path& path);

}  // namespace mc
