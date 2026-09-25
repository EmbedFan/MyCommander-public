#include "TestFixtures.h"
#include "TestFramework.h"

#include "Log.h"

#include <ctime>

using mc::AppendToLogFile;
using mc::FormatLogLine;
using mc::LogLevel;
using mc::Logger;
using mc::LogLevelToString;
using mc::ParseLogLevel;

namespace {

// A fixed instant, so FormatLogLine's output is deterministic in tests
// rather than depending on when the test happens to run.
std::chrono::system_clock::time_point FixedTime() {
    std::tm tmValue{};
    tmValue.tm_year = 126;  // 2026 (years since 1900)
    tmValue.tm_mon = 8;     // September (0-based)
    tmValue.tm_mday = 22;
    tmValue.tm_hour = 14;
    tmValue.tm_min = 5;
    tmValue.tm_sec = 9;
    tmValue.tm_isdst = -1;
    std::time_t t = std::mktime(&tmValue);
    return std::chrono::system_clock::from_time_t(t);
}

}  // namespace

// --- ParseLogLevel / LogLevelToString (ERR-004) -----------------------------

TEST_CASE(ParseLogLevel_ParsesEveryRecognizedName_CaseInsensitive_ERR004) {
    LogLevel level;
    CHECK(ParseLogLevel(L"off", level) && level == LogLevel::Off);
    CHECK(ParseLogLevel(L"ERROR", level) && level == LogLevel::Error);
    CHECK(ParseLogLevel(L"Warning", level) && level == LogLevel::Warning);
    CHECK(ParseLogLevel(L"info", level) && level == LogLevel::Info);
    CHECK(ParseLogLevel(L"DEBUG", level) && level == LogLevel::Debug);
}

TEST_CASE(ParseLogLevel_RejectsUnrecognizedText_ERR004) {
    LogLevel level = LogLevel::Debug;
    CHECK(!ParseLogLevel(L"verbose", level));
    CHECK(level == LogLevel::Debug);  // untouched on failure
}

TEST_CASE(LogLevelToString_RoundTripsThroughParseLogLevel_ERR004) {
    for (LogLevel level : {LogLevel::Off, LogLevel::Error, LogLevel::Warning, LogLevel::Info, LogLevel::Debug}) {
        LogLevel parsed;
        CHECK(ParseLogLevel(LogLevelToString(level), parsed));
        CHECK(parsed == level);
    }
}

// --- FormatLogLine -----------------------------------------------------------

TEST_CASE(FormatLogLine_IncludesTimestampLevelAndMessage_ERR004) {
    std::wstring line = FormatLogLine(LogLevel::Error, L"copy failed", FixedTime());

    CHECK(line.find(L"2026-09-22 14:05:09") != std::wstring::npos);
    CHECK(line.find(L"[ERROR]") != std::wstring::npos);
    CHECK(line.find(L"copy failed") != std::wstring::npos);
    CHECK(!line.empty() && line.back() == L'\n');
}

// --- AppendToLogFile ----------------------------------------------------------

TEST_CASE(AppendToLogFile_CreatesParentDirectoryAndAppends_ERR004) {
    test::TempDir root;
    auto path = root.Path() / L"nested" / L"mycommander.log";

    CHECK(AppendToLogFile(path, L"first\n"));
    CHECK(AppendToLogFile(path, L"second\n"));

    std::string content = test::ReadFileContent(path);
    CHECK(content.find("first") != std::string::npos);
    CHECK(content.find("second") != std::string::npos);
    CHECK(content.find("first") < content.find("second"));  // append order preserved
}

TEST_CASE(AppendToLogFile_EmptyPathFailsWithoutCrashing_ERR004) {
    CHECK(!AppendToLogFile(std::filesystem::path{}, L"anything\n"));
}

// --- Logger (severity filtering, ERR-004) -------------------------------------

TEST_CASE(Logger_OffLevelWritesNothing_ERR004) {
    test::TempDir root;
    auto path = root.Path() / L"mycommander.log";
    Logger logger(LogLevel::Off, path);

    logger.Log(LogLevel::Error, L"should not appear");

    CHECK(!std::filesystem::exists(path));
}

TEST_CASE(Logger_ErrorLevelSuppressesLessImportantMessages_ERR004) {
    test::TempDir root;
    auto path = root.Path() / L"mycommander.log";
    Logger logger(LogLevel::Error, path);

    logger.Log(LogLevel::Error, L"error message");
    logger.Log(LogLevel::Warning, L"warning message");
    logger.Log(LogLevel::Info, L"info message");
    logger.Log(LogLevel::Debug, L"debug message");

    std::string content = test::ReadFileContent(path);
    CHECK(content.find("error message") != std::string::npos);
    CHECK(content.find("warning message") == std::string::npos);
    CHECK(content.find("info message") == std::string::npos);
    CHECK(content.find("debug message") == std::string::npos);
}

TEST_CASE(Logger_DebugLevelWritesEverything_ERR004) {
    test::TempDir root;
    auto path = root.Path() / L"mycommander.log";
    Logger logger(LogLevel::Debug, path);

    logger.Log(LogLevel::Error, L"error message");
    logger.Log(LogLevel::Warning, L"warning message");
    logger.Log(LogLevel::Info, L"info message");
    logger.Log(LogLevel::Debug, L"debug message");

    std::string content = test::ReadFileContent(path);
    CHECK(content.find("error message") != std::string::npos);
    CHECK(content.find("warning message") != std::string::npos);
    CHECK(content.find("info message") != std::string::npos);
    CHECK(content.find("debug message") != std::string::npos);
}

TEST_CASE(Logger_WarningLevelIncludesErrorButNotInfo_ERR004) {
    test::TempDir root;
    auto path = root.Path() / L"mycommander.log";
    Logger logger(LogLevel::Warning, path);

    logger.Log(LogLevel::Error, L"error message");
    logger.Log(LogLevel::Warning, L"warning message");
    logger.Log(LogLevel::Info, L"info message");

    std::string content = test::ReadFileContent(path);
    CHECK(content.find("error message") != std::string::npos);
    CHECK(content.find("warning message") != std::string::npos);
    CHECK(content.find("info message") == std::string::npos);
}

TEST_CASE(Logger_ReportsItsConfiguredLevel_ERR004) {
    Logger logger(LogLevel::Info);
    CHECK(logger.Level() == LogLevel::Info);
}

TEST_CASE(Logger_SetLevel_ChangesThresholdOfAnAlreadyConstructedLogger_IS0006) {
    test::TempDir root;
    auto path = root.Path() / L"mycommander.log";
    Logger logger(LogLevel::Off, path);

    logger.Log(LogLevel::Info, L"before raising the level");
    CHECK(!std::filesystem::exists(path));

    logger.SetLevel(LogLevel::Info);
    CHECK(logger.Level() == LogLevel::Info);
    logger.Log(LogLevel::Info, L"after raising the level");

    std::string content = test::ReadFileContent(path);
    CHECK(content.find("before raising the level") == std::string::npos);
    CHECK(content.find("after raising the level") != std::string::npos);
}
