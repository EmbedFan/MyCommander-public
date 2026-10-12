#include "FileActions.h"

#include "PathUtil.h"
#include "Utf8TextFile.h"

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <iterator>

namespace mc {

namespace {

// D5: the one table to extend when adding an action type.
struct FileActionTypeInfo {
    FileActionType type;
    const wchar_t* keyword;
};
constexpr FileActionTypeInfo kFileActionTypes[] = {
    {FileActionType::View, L"view"},
    {FileActionType::Open, L"open"},
};

std::wstring Trim(const std::wstring& s) {
    size_t start = s.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return L"";
    size_t end = s.find_last_not_of(L" \t\r\n");
    return s.substr(start, end - start + 1);
}

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }

// Splits on '\n' only, leaving any '\r' inside each line, so JoinLines
// reproduces the original text byte-for-byte (CRLF or LF alike).
std::vector<std::wstring> SplitLines(const std::wstring& text) {
    std::vector<std::wstring> lines;
    size_t start = 0;
    while (true) {
        size_t nl = text.find(L'\n', start);
        if (nl == std::wstring::npos) {
            lines.push_back(text.substr(start));
            break;
        }
        lines.push_back(text.substr(start, nl - start));
        start = nl + 1;
    }
    return lines;
}

std::wstring JoinLines(const std::vector<std::wstring>& lines) {
    std::wstring out;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (i) out += L'\n';
        out += lines[i];
    }
    return out;
}

// The line ending a newly inserted line should use: CRLF if the file
// already uses it, so an edited file doesn't end up with mixed endings.
std::wstring LineEndingOf(const std::wstring& text) {
    return text.find(L"\r\n") != std::wstring::npos ? L"\r" : L"";
}


std::wstring WarningPrefix(int lineNumber) { return L"file-actions.conf line " + std::to_wstring(lineNumber) + L": "; }

bool IsKnownPlaceholder(wchar_t c) {
    return c == L'f' || c == L'n' || c == L'b' || c == L'e' || c == L'd' || c == L'%';
}

std::wstring ExtensionOf(const std::wstring& fileName) {
    size_t dot = fileName.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return L"";
    return fileName.substr(dot);  // with the dot
}

// Rewrites only the key part ("view", "view*") of an action line, keeping
// its indentation, the '=' and everything after it exactly as they were.
// Adds/removes one space after the key to keep a hand-aligned '=' column.
std::wstring RewriteActionKey(const std::wstring& line, const std::wstring& keyword, bool isDefault) {
    size_t eq = line.find(L'=');
    if (eq == std::wstring::npos) return line;
    std::wstring left = line.substr(0, eq);
    size_t keyStart = left.find_first_not_of(L" \t");
    if (keyStart == std::wstring::npos) return line;
    size_t keyEnd = left.find_last_not_of(L" \t") + 1;
    std::wstring oldKey = left.substr(keyStart, keyEnd - keyStart);
    std::wstring gap = left.substr(keyEnd);
    bool hadStar = !oldKey.empty() && oldKey.back() == L'*';
    if (hadStar && !isDefault) gap += L' ';
    if (!hadStar && isDefault && gap.size() > 1) gap.pop_back();
    return left.substr(0, keyStart) + keyword + (isDefault ? L"*" : L"") + gap + line.substr(eq);
}

const FileActionBlock* FindBlockOfLine(const FileActionTable& table, int sourceLine, const FileAction** actionOut) {
    for (const auto& block : table.blocks) {
        auto it = std::find_if(block.actions.begin(), block.actions.end(),
                               [&](const FileAction& action) { return action.sourceLine == sourceLine; });
        if (it != block.actions.end()) {
            *actionOut = &*it;
            return &block;
        }
    }
    return nullptr;
}

}  // namespace

const wchar_t* FileActionTypeKeyword(FileActionType type) {
    auto it = std::find_if(std::begin(kFileActionTypes), std::end(kFileActionTypes),
                           [&](const auto& info) { return info.type == type; });
    return it != std::end(kFileActionTypes) ? it->keyword : L"view";
}

std::optional<FileActionType> ParseFileActionTypeKeyword(const std::wstring& keyword) {
    for (const auto& info : kFileActionTypes)
        if (EqualsNoCase(keyword, info.keyword)) return info.type;
    return std::nullopt;
}

ParseFileActionsResult ParseFileActions(const std::wstring& text) {
    ParseFileActionsResult result;
    std::vector<std::wstring> lines = SplitLines(text);
    FileActionBlock* current = nullptr;

    for (size_t i = 0; i < lines.size(); ++i) {
        int lineNumber = static_cast<int>(i) + 1;
        std::wstring line = Trim(lines[i]);
        if (line.empty() || line.front() == L'#' || line.front() == L';') continue;
        // Leading BOM left by an editor on the very first line.
        if (lineNumber == 1 && line.front() == 0xFEFF) line = Trim(line.substr(1));
        if (line.empty()) continue;

        if (line.front() == L'[') {
            current = nullptr;
            size_t close = line.find(L']');
            if (close == std::wstring::npos) {
                result.warnings.push_back(WarningPrefix(lineNumber) + L"missing ']' in block header: " + line);
                continue;
            }
            FileActionBlock block;
            block.headerLine = lineNumber;
            std::wstring inner = line.substr(1, close - 1);
            size_t pos = 0;
            while (pos < inner.size()) {
                size_t start = inner.find_first_not_of(L" \t", pos);
                if (start == std::wstring::npos) break;
                size_t end = inner.find_first_of(L" \t", start);
                block.patterns.push_back(inner.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
                pos = end == std::wstring::npos ? inner.size() : end;
            }
            if (block.patterns.empty()) {
                result.warnings.push_back(WarningPrefix(lineNumber) + L"block header lists no extension or pattern");
                continue;
            }
            result.table.blocks.push_back(std::move(block));
            current = &result.table.blocks.back();
            continue;
        }

        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            result.warnings.push_back(WarningPrefix(lineNumber) + L"expected '<type> = description | program | arguments': " + line);
            continue;
        }
        if (!current) {
            result.warnings.push_back(WarningPrefix(lineNumber) + L"action outside any [extension] block: " + line);
            continue;
        }
        std::wstring key = Trim(line.substr(0, eq));
        FileAction action;
        action.sourceLine = lineNumber;
        if (!key.empty() && key.back() == L'*') {
            action.isDefault = true;
            key = Trim(key.substr(0, key.size() - 1));
        }
        auto type = ParseFileActionTypeKeyword(key);
        if (!type) {
            result.warnings.push_back(WarningPrefix(lineNumber) + L"unknown action type \"" + key + L"\" (expected view or open)");
            continue;
        }
        action.type = *type;

        // description | program | arguments -- arguments take everything
        // after the second '|', so they may contain '|' themselves.
        std::wstring rest = line.substr(eq + 1);
        size_t bar1 = rest.find(L'|');
        size_t bar2 = bar1 == std::wstring::npos ? std::wstring::npos : rest.find(L'|', bar1 + 1);
        if (bar2 == std::wstring::npos) {
            result.warnings.push_back(WarningPrefix(lineNumber) + L"expected 'description | program | arguments': " + line);
            continue;
        }
        action.description = Trim(rest.substr(0, bar1));
        action.program = Trim(rest.substr(bar1 + 1, bar2 - bar1 - 1));
        action.arguments = Trim(rest.substr(bar2 + 1));
        if (action.program.size() >= 2 && action.program.front() == L'"' && action.program.back() == L'"')
            action.program = action.program.substr(1, action.program.size() - 2);
        if (action.program.empty()) {
            result.warnings.push_back(WarningPrefix(lineNumber) + L"no program given");
            continue;
        }
        if (action.program.size() >= 7 && _wcsnicmp(action.program.c_str(), kFileActionProgramPluginPrefix, 7) == 0) {
            // EXT-003: a viewer plugin, for view actions only.
            if (action.type != FileActionType::View) {
                result.warnings.push_back(WarningPrefix(lineNumber) + L"\"" + action.program +
                                          L"\" is a viewer plugin and can only be used for a view action");
                continue;
            }
            if (Trim(action.program.substr(7)).empty()) {
                result.warnings.push_back(WarningPrefix(lineNumber) + L"\"plugin:\" needs a plugin id after it");
                continue;
            }
        }
        for (size_t p = 0; p + 1 < action.arguments.size(); ++p) {
            if (action.arguments[p] != L'%') continue;
            if (!IsKnownPlaceholder(action.arguments[p + 1])) {
                result.warnings.push_back(WarningPrefix(lineNumber) + L"unknown placeholder %" +
                                          std::wstring(1, action.arguments[p + 1]) + L" (kept as-is)");
                break;
            }
            ++p;  // skip the placeholder letter (and a "%%" pair)
        }
        if (action.isDefault) {
            if (std::any_of(current->actions.begin(), current->actions.end(), [&](const FileAction& other) {
                    return other.type == action.type && other.isDefault;
                })) {
                result.warnings.push_back(WarningPrefix(lineNumber) + L"a second default " + key +
                                          L" action in the same block (the first one wins)");
            }
        }
        current->actions.push_back(std::move(action));
    }
    return result;
}

std::wstring ReadFileActionsText(const std::filesystem::path& path) { return ReadUtf8TextFile(path); }

ParseFileActionsResult LoadFileActionsFrom(const std::filesystem::path& path) {
    return ParseFileActions(ReadFileActionsText(path));
}

std::filesystem::path FileActionsFilePath() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path(buf) / L"MyCommander" / L"file-actions.conf";
}

std::wstring DefaultFileActionsText() {
    return L"# MyCommander file actions (FEA-0009). Save this file and the changes apply\n"
           L"# immediately. File > Assign File to App edits it from inside MyCommander too.\n"
           L"# (This is not mycommander.ini -- Ctrl+F11 opens that one; file actions\n"
           L"# only work here.)\n"
           L"#\n"
           L"#   F3          runs a file's default \"view\" action (no match: built-in viewer)\n"
           L"#   Shift+Enter runs its default \"open\" action (no match: the Windows association)\n"
           L"#   Shift+F3    lists every action for the file, to pick a non-default one\n"
           L"#\n"
           L"# [<patterns>]  one or more extensions (.txt) or wildcard patterns (*.tar.gz,\n"
           L"#               readme*), space-separated; [*] matches every file. Blocks are\n"
           L"#               searched top to bottom: the first matching block that has a\n"
           L"#               view (or open) action decides, so put catch-alls last.\n"
           L"# <type>[*] = <description> | <program> | <arguments>\n"
           L"#   type      : view | open\n"
           L"#   *         : picks the default of that type within its block (optional --\n"
           L"#               without one, the block's first action of that type is used)\n"
           L"#   program   : an executable path or bare name (found via PATH), or:\n"
           L"#                 internal:viewer   the built-in text viewer\n"
           L"#                 system:           the Windows file association\n"
           L"#   arguments : %f full path  %n file name  %b name without extension\n"
           L"#               %e extension (no dot)  %d directory  %% a literal %\n"
           L"#               If none of %f/%n/%b appears, \"%f\" is added automatically.\n"
           L"#               Quote paths yourself where they may contain spaces: \"%f\"\n"
           L"#\n"
           L"# Examples (remove the leading '#' to use them):\n"
           L"#\n"
           L"# [.txt .log .md .ini]\n"
           L"# view  = Notepad++ (read-only) | C:\\Program Files\\Notepad++\\notepad++.exe | -ro \"%f\"\n"
           L"# open* = Notepad               | notepad.exe | \"%f\"\n"
           L"#\n"
           L"# [.pdf .jpg .png]\n"
           L"# open* = Windows default       | system: |\n";
}

namespace {

// The first-run sample as 1.11.0 first wrote it: the same comments plus one
// active block that only restated the built-in fallback, yet -- because it
// matched .txt/.md/... -- got in the way of a user's own blocks.
// Frozen verbatim -- never edit, or the cleanup below stops recognizing it.
std::wstring LegacyDefaultFileActionsText() {
    return L"# MyCommander file actions (FEA-0009). Save this file and the changes apply\n"
           L"# immediately. File > Assign File to App edits it from inside MyCommander too.\n"
           L"#\n"
           L"#   F3          runs a file's default \"view\" action (no match: built-in viewer)\n"
           L"#   Shift+Enter runs its default \"open\" action (no match: the Windows association)\n"
           L"#   Shift+F3    lists every action for the file, to pick a non-default one\n"
           L"#\n"
           L"# [<patterns>]  one or more extensions (.txt) or wildcard patterns (*.tar.gz,\n"
           L"#               readme*), space-separated; [*] matches every file. The first\n"
           L"#               matching block wins, so put catch-alls last.\n"
           L"# <type>[*] = <description> | <program> | <arguments>\n"
           L"#   type      : view | open\n"
           L"#   *         : marks the default action of that type in this block\n"
           L"#   program   : an executable path or bare name (found via PATH), or:\n"
           L"#                 internal:viewer   the built-in text viewer\n"
           L"#                 system:           the Windows file association\n"
           L"#   arguments : %f full path  %n file name  %b name without extension\n"
           L"#               %e extension (no dot)  %d directory  %% a literal %\n"
           L"#               If none of %f/%n/%b appears, \"%f\" is added automatically.\n"
           L"#               Quote paths yourself where they may contain spaces: \"%f\"\n"
           L"#\n"
           L"# Examples (remove the leading '#' to use them):\n"
           L"#\n"
           L"# [.txt .log .md .ini]\n"
           L"# view  = Notepad++ (read-only) | C:\\Program Files\\Notepad++\\notepad++.exe | -ro \"%f\"\n"
           L"# open* = Notepad               | notepad.exe | \"%f\"\n"
           L"#\n"
           L"# [.pdf .jpg .png]\n"
           L"# open* = Windows default       | system: |\n"
           L"\n"
           L"[.txt .log .md .ini .conf]\n"
           L"view* = Built-in viewer | internal:viewer |\n";
}

}  // namespace

bool IsUnmodifiedLegacyFileActionsSample(const std::wstring& text) {
    std::vector<std::wstring> legacy = SplitLines(LegacyDefaultFileActionsText());
    for (auto& line : legacy) line = Trim(line);
    std::vector<std::wstring> active;
    for (const auto& raw : SplitLines(text)) {
        std::wstring line = Trim(raw);
        if (!line.empty() && line.front() == 0xFEFF) line = Trim(line.substr(1));
        if (line.empty()) continue;
        if (line.front() == L'#' || line.front() == L';') {
            // Any comment the sample didn't have means the user touched it.
            if (std::find(legacy.begin(), legacy.end(), line) == legacy.end()) return false;
            continue;
        }
        active.push_back(line);
    }
    return active == std::vector<std::wstring>{L"[.txt .log .md .ini .conf]", L"view* = Built-in viewer | internal:viewer |"};
}

std::wstring MigrateLegacyFileActionsText(const std::wstring& text) {
    if (IsUnmodifiedLegacyFileActionsSample(text)) return DefaultFileActionsText();
    std::vector<std::wstring> lines = SplitLines(text);
    for (size_t i = 0; i + 1 < lines.size(); ++i) {
        if (Trim(lines[i]) != L"[.txt .log .md .ini .conf]" ||
            Trim(lines[i + 1]) != L"view* = Built-in viewer | internal:viewer |")
            continue;
        // Only the generated block exactly: nothing else was added under it.
        if (i + 2 < lines.size()) {
            std::wstring next = Trim(lines[i + 2]);
            if (!next.empty() && next.front() != L'[' && next.front() != L'#' && next.front() != L';') continue;
        }
        size_t first = (i > 0 && Trim(lines[i - 1]).empty()) ? i - 1 : i;  // its separating blank line too
        lines.erase(lines.begin() + static_cast<ptrdiff_t>(first), lines.begin() + static_cast<ptrdiff_t>(i + 2));
        return JoinLines(lines);
    }
    return text;
}

void EnsureFileActionsFileExistsAt(const std::filesystem::path& path) {
    if (path.empty()) return;
    std::error_code ec;
    if (std::filesystem::exists(path, ec)) {
        std::wstring text = ReadFileActionsText(path);
        std::wstring migrated = MigrateLegacyFileActionsText(text);
        if (migrated != text) SaveFileActionsText(path, migrated);
        return;
    }
    SaveFileActionsText(path, DefaultFileActionsText());
}

bool FileActionPatternMatches(const std::wstring& pattern, const std::wstring& fileName) {
    if (pattern.empty() || fileName.empty()) return false;
    if (pattern.find_first_of(L"*?") != std::wstring::npos)
        return detail::GlobMatchCaseInsensitive(fileName.c_str(), pattern.c_str());
    if (pattern.front() == L'.' && EqualsNoCase(ExtensionOf(fileName), pattern)) return true;
    return EqualsNoCase(fileName, pattern);
}

std::vector<FileAction> FindFileActions(const FileActionTable& table, const std::wstring& fileName,
                                        FileActionType type) {
    std::vector<FileAction> found;
    for (const auto& block : table.blocks) {
        bool matches = std::any_of(block.patterns.begin(), block.patterns.end(),
                                   [&](const std::wstring& p) { return FileActionPatternMatches(p, fileName); });
        if (!matches) continue;
        std::copy_if(block.actions.begin(), block.actions.end(), std::back_inserter(found),
                     [&](const FileAction& action) { return action.type == type; });
    }
    return found;
}

std::optional<FileAction> DefaultFileAction(const FileActionTable& table, const std::wstring& fileName,
                                            FileActionType type) {
    // Block order decides (the file's own documented rule: "the first
    // matching block wins"); '*' only picks *within* that block. An earlier
    // version let any '*' anywhere beat an unmarked action in an earlier
    // block, which surprised users whose own blocks had no '*'.
    for (const auto& block : table.blocks) {
        bool matches = std::any_of(block.patterns.begin(), block.patterns.end(),
                                   [&](const std::wstring& p) { return FileActionPatternMatches(p, fileName); });
        if (!matches) continue;
        const FileAction* first = nullptr;
        for (const auto& action : block.actions) {
            if (action.type != type) continue;
            if (action.isDefault) return action;
            if (!first) first = &action;
        }
        if (first) return *first;
    }
    return std::nullopt;
}

std::wstring ExpandFileActionArguments(const std::wstring& argumentTemplate, const std::filesystem::path& file) {
    std::wstring name = file.filename().wstring();
    std::wstring ext = ExtensionOf(name);
    std::wstring base = ext.empty() ? name : name.substr(0, name.size() - ext.size());
    std::wstring extNoDot = ext.empty() ? L"" : ext.substr(1);

    std::wstring out;
    bool hasFile = false;
    for (size_t i = 0; i < argumentTemplate.size(); ++i) {
        wchar_t c = argumentTemplate[i];
        if (c != L'%' || i + 1 >= argumentTemplate.size()) {
            out += c;
            continue;
        }
        wchar_t p = argumentTemplate[i + 1];
        switch (p) {
            case L'f': out += file.wstring(); hasFile = true; break;
            case L'n': out += name; hasFile = true; break;
            case L'b': out += base; hasFile = true; break;
            case L'e': out += extNoDot; break;
            case L'd': out += file.parent_path().wstring(); break;
            case L'%': out += L'%'; break;
            default: out += c; out += p; break;  // unknown: kept literally
        }
        ++i;
    }
    if (!hasFile) {
        if (!out.empty()) out += L' ';
        out += L"\"" + file.wstring() + L"\"";
    }
    return out;
}

std::optional<std::wstring> FileActionPluginId(const FileAction& action) {
    if (action.program.size() <= 7 || _wcsnicmp(action.program.c_str(), kFileActionProgramPluginPrefix, 7) != 0)
        return std::nullopt;
    std::wstring id = Trim(action.program.substr(7));
    if (id.empty()) return std::nullopt;
    return id;
}

std::wstring FileActionLabel(const FileAction& action) {
    if (!action.description.empty()) return action.description;
    if (auto pluginId = FileActionPluginId(action)) return L"Plugin viewer (" + *pluginId + L")";
    if (action.program == kFileActionProgramInternalViewer) return L"Built-in viewer";
    if (action.program == kFileActionProgramSystem) return L"Windows default";
    return std::filesystem::path(action.program).filename().wstring();
}

std::wstring FormatFileActionLine(const FileAction& action) {
    std::wstring line = std::wstring(FileActionTypeKeyword(action.type)) + (action.isDefault ? L"* = " : L" = ");
    line += action.description + L" | " + action.program + L" |";
    if (!action.arguments.empty()) line += L" " + action.arguments;
    return line;
}

std::wstring AddFileActionLine(const std::wstring& text, const std::wstring& pattern, const FileAction& action) {
    ParseFileActionsResult parsed = ParseFileActions(text);
    std::vector<std::wstring> lines = SplitLines(text);
    std::wstring cr = LineEndingOf(text);
    std::wstring newLine = FormatFileActionLine(action) + cr;

    int insertedLine = 0;  // 1-based
    // Only a block dedicated to exactly this one pattern is appended to -- a
    // shared block ([.txt .log .md]) is never touched, or adding a .md
    // action would silently change .txt/.log too.
    for (const auto& block : parsed.table.blocks) {
        if (block.patterns.size() != 1 || !EqualsNoCase(block.patterns[0], pattern)) continue;
        int after = block.actions.empty() ? block.headerLine : block.actions.back().sourceLine;
        // Inserted as the very last line of a file with no final newline:
        // no trailing '\r' either, matching the line it follows.
        std::wstring toInsert = after == static_cast<int>(lines.size()) ? FormatFileActionLine(action) : newLine;
        lines.insert(lines.begin() + after, toInsert);
        insertedLine = after + 1;
        break;
    }
    if (insertedLine == 0) {
        // No dedicated block yet: create one *before* the first shared block
        // listing this pattern, so (first default wins, D2) its actions take
        // priority for this pattern without editing the shared block.
        for (const auto& block : parsed.table.blocks) {
            bool listed = std::any_of(block.patterns.begin(), block.patterns.end(),
                                      [&](const std::wstring& p) { return EqualsNoCase(p, pattern); });
            if (!listed) continue;
            int at = block.headerLine - 1;  // 0-based index of the shared block's header
            lines.insert(lines.begin() + at, {L"[" + pattern + L"]" + cr, newLine, cr});
            insertedLine = at + 2;
            break;
        }
    }
    if (insertedLine == 0) {
        // New block at the end, separated by a blank line.
        if (lines.size() == 1 && lines[0].empty()) lines.clear();
        else if (lines.back().empty()) lines.pop_back();  // drop the split's trailing "" (text ended with '\n')
        if (!lines.empty()) lines.push_back(cr);
        lines.push_back(L"[" + pattern + L"]" + cr);
        lines.push_back(newLine);
        insertedLine = static_cast<int>(lines.size());
        lines.push_back(L"");  // keep a final newline
    }
    std::wstring result = JoinLines(lines);
    return action.isDefault ? SetDefaultFileActionLine(result, insertedLine) : result;
}

std::wstring RemoveFileActionLine(const std::wstring& text, int sourceLine) {
    std::vector<std::wstring> lines = SplitLines(text);
    if (sourceLine < 1 || sourceLine > static_cast<int>(lines.size())) return text;
    lines.erase(lines.begin() + (sourceLine - 1));
    return JoinLines(lines);
}

std::wstring SetDefaultFileActionLine(const std::wstring& text, int sourceLine) {
    ParseFileActionsResult parsed = ParseFileActions(text);
    const FileAction* target = nullptr;
    const FileActionBlock* block = FindBlockOfLine(parsed.table, sourceLine, &target);
    if (!block) return text;
    std::vector<std::wstring> lines = SplitLines(text);
    for (const auto& action : block->actions) {
        if (action.type != target->type) continue;
        bool shouldBeDefault = action.sourceLine == sourceLine;
        if (action.isDefault == shouldBeDefault) continue;
        std::wstring& line = lines[static_cast<size_t>(action.sourceLine - 1)];
        line = RewriteActionKey(line, FileActionTypeKeyword(action.type), shouldBeDefault);
    }
    return JoinLines(lines);
}

std::wstring SetDefaultFileActionForPattern(const std::wstring& text, const std::wstring& pattern, int sourceLine) {
    ParseFileActionsResult parsed = ParseFileActions(text);
    const FileAction* target = nullptr;
    const FileActionBlock* targetBlock = FindBlockOfLine(parsed.table, sourceLine, &target);
    if (!targetBlock) return text;
    if (targetBlock->patterns.size() == 1) return SetDefaultFileActionLine(text, sourceLine);

    // A shared block: re-mark an identical copy in the pattern's own block
    // if there is one, else add one there as its default (AddFileActionLine
    // places a new dedicated block above the shared one, so it decides).
    for (const auto& block : parsed.table.blocks) {
        if (block.patterns.size() != 1 || !EqualsNoCase(block.patterns[0], pattern)) continue;
        auto same = std::find_if(block.actions.begin(), block.actions.end(), [&](const FileAction& action) {
            return action.type == target->type && action.program == target->program &&
                   action.arguments == target->arguments;
        });
        if (same != block.actions.end()) return SetDefaultFileActionLine(text, same->sourceLine);
    }
    FileAction copy = *target;
    copy.isDefault = true;
    return AddFileActionLine(text, pattern, copy);
}

bool SaveFileActionsText(const std::filesystem::path& path, const std::wstring& text) {
    return WriteUtf8TextFileAtomic(path, text);
}

}  // namespace mc
