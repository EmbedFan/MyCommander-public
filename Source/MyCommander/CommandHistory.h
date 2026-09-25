#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace mc {

// CLI-004: the number of past commands retained, in memory and (when
// Config::persistCommandHistory is enabled) in the persisted history file.
// Matches the cap the command line's Ctrl+Up/Down recall has always used.
constexpr size_t kMaxCommandHistoryEntries = 100;

// Separate from both Config (preferences) and Session (cosmetic panel paths,
// which NAV-008/ERR-006 deliberately keeps free of anything like a typed
// command). A list of past commands carries none of Session's "must never
// be replayed after a crash" concern — nothing here re-runs automatically,
// it's only ever recalled by the user pressing Ctrl+Up/Down — so an ordinary
// durable file, written at the same normal-exit point Session's is, is
// enough (CLI-004).
std::filesystem::path CommandHistoryFilePath();

// CLI-004/CLI-007: reads one command per line from `path`, oldest first. A
// missing or unreadable file yields an empty history — the expected
// first-run state, not an error — and a file holding more than
// kMaxCommandHistoryEntries lines is silently trimmed to the most recent
// ones. Lines are taken verbatim (only a trailing '\r' is stripped, for a
// CRLF-saved file) since a command's own content, unlike a "key = value"
// setting, must never be reinterpreted.
std::vector<std::wstring> LoadCommandHistoryFrom(const std::filesystem::path& path);
std::vector<std::wstring> LoadCommandHistory();

// Writes `history` one command per line, oldest first, atomically where
// possible — trimmed to the most recent kMaxCommandHistoryEntries if the
// caller passes more. Failure is intentionally silent, the same convention
// Session.h's save uses: history persistence is a convenience, never a
// reason to interrupt a normal exit.
bool SaveCommandHistoryTo(const std::filesystem::path& path, const std::vector<std::wstring>& history);
bool SaveCommandHistory(const std::vector<std::wstring>& history);

}  // namespace mc
