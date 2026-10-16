#pragma once

// CLI-008: reverse incremental search through the command history (Alt+R).
// Pure matching and key classification -- the state and the drawing live in
// main.cpp, so this is unit-testable without Console.

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace mc {

// The newest entry at or before `startIndex` (scanning backward) that contains `text`, compared
// case-insensitively with invariant Unicode folding. An empty `text` matches the entry at `startIndex`.
// `history` is oldest first (as CommandHistory keeps it). `startIndex` is clamped to the last entry;
// nullopt when nothing matches or the history is empty.
std::optional<size_t> FindHistoryMatch(const std::vector<std::wstring>& history, const std::wstring& text,
                                       size_t startIndex);

// What a key means while the search is open.
enum class SearchKeyAction {
    StepOlder,  // Alt+R again: the next older match
    Accept,     // Enter: the match goes onto the command line (not run)
    Cancel,     // Esc: restore the command line as it was
    Backspace,  // shorten the search text
    Insert,     // a printable character extends the search text
    Other,      // anything else: the search ends (command line restored) and the key acts normally
};

SearchKeyAction ClassifySearchKey(unsigned short virtualKey, bool ctrl, bool shift, bool alt, wchar_t unicodeChar);

}  // namespace mc
