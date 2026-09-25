#include "Log.h"

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include <algorithm>
#include <ctime>
#include <cwctype>
#include <fstream>

namespace mc {

namespace {

std::wstring ToLower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

}  // namespace

bool ParseLogLevel(const std::wstring& text, LogLevel& outLevel) {
    std::wstring lower = ToLower(text);
    if (lower == L"off") {
        outLevel = LogLevel::Off;
        return true;
    }
    if (lower == L"error") {
        outLevel = LogLevel::Error;
        return true;
    }
    if (lower == L"warning") {
        outLevel = LogLevel::Warning;
        return true;
    }
    if (lower == L"info") {
        outLevel = LogLevel::Info;
        return true;
    }
    if (lower == L"debug") {
        outLevel = LogLevel::Debug;
        return true;
    }
    return false;
}

std::wstring LogLevelToString(LogLevel level) {
    switch (level) {
        case LogLevel::Off:
            return L"OFF";
        case LogLevel::Error:
            return L"ERROR";
        case LogLevel::Warning:
            return L"WARNING";
        case LogLevel::Info:
            return L"INFO";
        case LogLevel::Debug:
            return L"DEBUG";
    }
    return L"UNKNOWN";
}

std::filesystem::path LogFilePath() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path(buf) / L"MyCommander" / L"mycommander.log";
}

std::wstring FormatLogLine(LogLevel level, const std::wstring& message, std::chrono::system_clock::time_point when) {
    std::time_t t = std::chrono::system_clock::to_time_t(when);
    std::tm tmBuf{};
    localtime_s(&tmBuf, &t);
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d:%02d", tmBuf.tm_year + 1900, tmBuf.tm_mon + 1, tmBuf.tm_mday,
              tmBuf.tm_hour, tmBuf.tm_min, tmBuf.tm_sec);
    return L"[" + std::wstring(buf) + L"] [" + LogLevelToString(level) + L"] " + message + L"\n";
}

bool AppendToLogFile(const std::filesystem::path& path, const std::wstring& line) {
    if (path.empty()) return false;

    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) return false;

    std::wofstream out(path, std::ios::app);
    if (!out) return false;
    out << line;
    return static_cast<bool>(out);
}

}  // namespace mc
