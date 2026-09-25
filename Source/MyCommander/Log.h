#pragma once

#include <chrono>
#include <filesystem>
#include <string>

namespace mc {

// ERR-004: severities in increasing verbosity order. `Off` suppresses all
// logging outright, and is the default (CFG-003-style: a missing/default
// config file never turns on a feature that writes to disk on its own).
enum class LogLevel { Off, Error, Warning, Info, Debug };

// Parses a case-insensitive level name ("off"/"error"/"warning"/"info"/
// "debug"); returns false (leaving `outLevel` untouched) for anything else
// — the same reporting convention Config.cpp's other parsers use.
bool ParseLogLevel(const std::wstring& text, LogLevel& outLevel);

// The exact spelling ParseLogLevel accepts back, and what FormatLogLine
// tags each line with.
std::wstring LogLevelToString(LogLevel level);

// CFG-002-style per-user location — same `%APPDATA%\MyCommander` folder as
// Config/Session/CommandHistory, empty if %APPDATA% can't be resolved.
std::filesystem::path LogFilePath();

// Formats one line as `[YYYY-MM-DD HH:MM:SS] [LEVEL] message\n`. Pure and
// deterministic given `when`, so it's unit-testable without depending on
// the real clock — the only place real time enters is Logger::Log below.
std::wstring FormatLogLine(LogLevel level, const std::wstring& message, std::chrono::system_clock::time_point when);

// Appends `line` to `path`, creating its parent directory if needed.
// Returns false if the file couldn't be opened for appending; failure is
// otherwise silent to callers, the same convention Session/CommandHistory's
// saves use — logging is a diagnostic convenience, never a reason to
// interrupt whatever it's called from.
bool AppendToLogFile(const std::filesystem::path& path, const std::wstring& line);

// ERR-004: a small process-wide logger, constructed once at startup with
// the severity threshold from Config::logLevel and handed by reference to
// whichever operations want to record something. Log(level, message) is
// written only if `level` is at or more important than the configured
// threshold (Error is the most important, Debug the least; `Off` accepts
// nothing) — plain integer comparison on the enum's declaration order, so
// there's no separate "is this enabled" branch to keep in sync with it.
//
// ERR-005: nothing in this codebase routes command-line text, file
// content, or credentials through a Logger — every call site logs only
// paths, Win32/error text, and setting names, so there is nothing
// sensitive for it to ever record. Enforcing that is a wiring discipline
// at each call site, not something this class can detect on its own.
class Logger {
public:
    explicit Logger(LogLevel level, std::filesystem::path path = LogFilePath())
        : level_(level), path_(std::move(path)) {}

    void Log(LogLevel level, const std::wstring& message) {
        if (level_ == LogLevel::Off) return;
        if (static_cast<int>(level) > static_cast<int>(level_)) return;
        AppendToLogFile(path_, FormatLogLine(level, message, std::chrono::system_clock::now()));
    }

    LogLevel Level() const { return level_; }

    // IS-0006: lets a live config reload change the threshold of an
    // already-constructed, already-shared-by-reference Logger, instead of
    // needing to reconstruct one (which every call site holds a reference
    // to, not a copy).
    void SetLevel(LogLevel level) { level_ = level; }

private:
    LogLevel level_;
    std::filesystem::path path_;
};

} // namespace mc
