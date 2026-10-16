#pragma once

// NAV-018: path completion for the command line (Ctrl+Space). The decisions are pure and unit-tested;
// only ListFolderForCompletion touches the disk. No Console/Dialog dependency.

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace mc {

// The path being typed at the end of the command line: the text after the last unquoted space, or after an
// opening quote. `dir` is everything up to and including the last '\' or '/', `name` what follows it.
struct PathToken {
    size_t start = 0;      // index in the command line where `dir` begins (after an opening quote)
    std::wstring dir;
    std::wstring name;
    bool quoted = false;   // inside an unclosed quote
};

// `text` is the command line up to the cursor.
PathToken ExtractPathToken(const std::wstring& text);

struct CompletionEntry {
    std::wstring name;
    bool isDirectory = false;
    bool hidden = false;   // hidden or system
};

// Entries of `all` whose name starts with `namePart` (case-insensitive, invariant folding), sorted
// case-insensitively; hidden/system ones only when `includeHidden` or `namePart` starts with a dot.
std::vector<CompletionEntry> FilterCompletions(const std::vector<CompletionEntry>& all, const std::wstring& namePart,
                                               bool includeHidden);

// The longest common prefix, compared case-insensitively, spelled as the first candidate spells it.
std::wstring CommonCompletionPrefix(const std::vector<CompletionEntry>& candidates);

// The cycle for repeated presses: -1 (not started) -> 0 -> 1 ... -> count-1 -> 0.
int NextCompletionIndex(int index, size_t count);

// `text` with the token's name part replaced by `replacement` (and, for a folder candidate completed in
// full, a trailing '\'). `fullName` true appends the separator for a directory.
std::wstring ApplyCompletion(const std::wstring& text, const PathToken& token, const CompletionEntry& candidate,
                             bool fullName);
std::wstring ApplyCompletionPrefix(const std::wstring& text, const PathToken& token, const std::wstring& prefix);

// Lists `folder` (no "." / ".."), real Win32 I/O, bounded to 50,000 entries. Empty when it cannot be listed.
std::vector<CompletionEntry> ListFolderForCompletion(const std::filesystem::path& folder);

}  // namespace mc
