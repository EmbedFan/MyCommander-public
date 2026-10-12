#include "FtpParse.h"

#include <algorithm>
#include <cctype>
#include <cwctype>

namespace ftp {

namespace {

bool IsDigits(const std::string& s) {
    return !s.empty() && std::all_of(s.begin(), s.end(), [](unsigned char c) { return std::isdigit(c) != 0; });
}

std::string Upper(std::string s) {
    for (auto& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

std::string TrimAscii(const std::string& s) {
    size_t b = s.find_first_not_of(" \t\r\n");
    if (b == std::string::npos) return "";
    size_t e = s.find_last_not_of(" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::optional<uint64_t> ToFileTime(int year, int month, int day, int hour, int minute, int second) {
    SYSTEMTIME st{};
    st.wYear = static_cast<WORD>(year);
    st.wMonth = static_cast<WORD>(month);
    st.wDay = static_cast<WORD>(day);
    st.wHour = static_cast<WORD>(hour);
    st.wMinute = static_cast<WORD>(minute);
    st.wSecond = static_cast<WORD>(second);
    FILETIME ft{};
    if (!SystemTimeToFileTime(&st, &ft)) return std::nullopt;
    return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
}

int MonthIndex(const std::string& token) {
    static const char* kMonths[] = {"JAN", "FEB", "MAR", "APR", "MAY", "JUN", "JUL", "AUG", "SEP", "OCT", "NOV", "DEC"};
    std::string u = Upper(token);
    for (int i = 0; i < 12; ++i)
        if (u == kMonths[i]) return i + 1;
    return 0;
}

// Octal "0755"/"755" -> "rwxr-xr-x".
std::wstring ModeToPermissions(const std::string& octal) {
    if (octal.empty() || octal.size() > 4) return L"";
    unsigned mode = 0;
    for (char c : octal) {
        if (c < '0' || c > '7') return L"";
        mode = mode * 8 + static_cast<unsigned>(c - '0');
    }
    std::wstring s;
    for (int shift = 6; shift >= 0; shift -= 3) {
        unsigned bits = (mode >> shift) & 7u;
        s += (bits & 4) ? L'r' : L'-';
        s += (bits & 2) ? L'w' : L'-';
        s += (bits & 1) ? L'x' : L'-';
    }
    return s;
}

struct Token {
    std::string text;
    size_t start = 0;
};

std::vector<Token> Tokenize(const std::string& line) {
    std::vector<Token> tokens;
    size_t i = 0;
    while (i < line.size()) {
        while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
        if (i >= line.size()) break;
        size_t start = i;
        while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
        tokens.push_back({line.substr(start, i - start), start});
    }
    return tokens;
}

int HexValue(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    return -1;
}

// %XX decoding of a URL part (bytes reassembled as UTF-8).
std::wstring PercentDecode(const std::wstring& s) {
    std::string bytes;
    for (size_t i = 0; i < s.size(); ++i) {
        if (s[i] == L'%' && i + 2 < s.size() && HexValue(s[i + 1]) >= 0 && HexValue(s[i + 2]) >= 0) {
            bytes += static_cast<char>(HexValue(s[i + 1]) * 16 + HexValue(s[i + 2]));
            i += 2;
        } else {
            bytes += EncodeName(std::wstring(1, s[i]), true);
        }
    }
    return DecodeName(bytes, true);
}

}  // namespace

std::optional<Reply> ReplyReader::Next() {
    size_t firstEnd = buffer_.find('\n');
    if (firstEnd == std::string::npos) return std::nullopt;
    auto lineAt = [&](size_t start, size_t end) {
        std::string line = buffer_.substr(start, end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        return line;
    };
    std::string first = lineAt(0, firstEnd);
    bool hasCode = first.size() >= 3 && std::isdigit(static_cast<unsigned char>(first[0])) &&
                   std::isdigit(static_cast<unsigned char>(first[1])) && std::isdigit(static_cast<unsigned char>(first[2]));
    if (!hasCode) {
        buffer_.erase(0, firstEnd + 1);
        return Reply{0, first};
    }
    int code = std::stoi(first.substr(0, 3));
    if (first.size() < 4 || first[3] != '-') {
        buffer_.erase(0, firstEnd + 1);
        return Reply{code, first.size() > 4 ? first.substr(4) : ""};
    }
    // Multi-line: until a line "ddd " with the same code.
    std::string text = first.substr(4);
    size_t pos = firstEnd + 1;
    while (true) {
        size_t end = buffer_.find('\n', pos);
        if (end == std::string::npos) return std::nullopt;  // incomplete -- wait for more bytes
        std::string line = lineAt(pos, end);
        pos = end + 1;
        if (line.size() >= 4 && line.compare(0, 3, first, 0, 3) == 0 && line[3] == ' ') {
            text += "\n" + line.substr(4);
            buffer_.erase(0, pos);
            return Reply{code, text};
        }
        text += "\n" + line;
    }
}

bool ParsePasvReply(const std::string& text, std::string& host, uint16_t& port) {
    // Six comma-separated numbers, wherever they appear.
    for (size_t i = 0; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) continue;
        if (i > 0 && std::isdigit(static_cast<unsigned char>(text[i - 1]))) continue;  // only at the start of a number
        int values[6];
        size_t p = i;
        int n = 0;
        for (; n < 6; ++n) {
            if (p >= text.size() || !std::isdigit(static_cast<unsigned char>(text[p]))) break;
            int v = 0;
            size_t digits = 0;
            while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p])) && digits < 4) {
                v = v * 10 + (text[p] - '0');
                ++p;
                ++digits;
            }
            if (v > 255) break;
            values[n] = v;
            if (n < 5) {
                if (p >= text.size() || text[p] != ',') break;
                ++p;
            }
        }
        if (n == 6) {
            host = std::to_string(values[0]) + "." + std::to_string(values[1]) + "." + std::to_string(values[2]) + "." +
                   std::to_string(values[3]);
            port = static_cast<uint16_t>(values[4] * 256 + values[5]);
            return port != 0;
        }
    }
    return false;
}

bool ParseEpsvReply(const std::string& text, uint16_t& port) {
    size_t open = text.find('(');
    if (open == std::string::npos || open + 4 >= text.size()) return false;
    char d = text[open + 1];
    if (text[open + 2] != d || text[open + 3] != d) return false;
    size_t p = open + 4;
    unsigned value = 0;
    size_t digits = 0;
    while (p < text.size() && std::isdigit(static_cast<unsigned char>(text[p]))) {
        value = value * 10 + static_cast<unsigned>(text[p] - '0');
        ++p;
        ++digits;
    }
    if (digits == 0 || digits > 5 || value == 0 || value > 65535 || p >= text.size() || text[p] != d) return false;
    port = static_cast<uint16_t>(value);
    return true;
}

std::optional<RemoteEntry> ParseMlsdLine(const std::string& rawLine, bool utf8) {
    std::string line = rawLine;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    size_t space = line.find(' ');
    if (space == std::string::npos || space + 1 >= line.size()) return std::nullopt;
    std::string facts = line.substr(0, space);
    RemoteEntry entry;
    entry.name = DecodeName(line.substr(space + 1), utf8);
    std::string type;
    size_t start = 0;
    while (start < facts.size()) {
        size_t semi = facts.find(';', start);
        if (semi == std::string::npos) semi = facts.size();
        std::string fact = facts.substr(start, semi - start);
        start = semi + 1;
        size_t eq = fact.find('=');
        if (eq == std::string::npos) continue;
        std::string key = Upper(fact.substr(0, eq));
        std::string value = fact.substr(eq + 1);
        if (key == "TYPE") type = Upper(value);
        else if ((key == "SIZE" || key == "SIZD") && IsDigits(value)) entry.size = std::stoull(value);
        else if (key == "MODIFY") entry.modifiedUtc = ParseFtpTime(value).value_or(0);
        else if (key == "UNIX.MODE") entry.permissions = ModeToPermissions(value);
    }
    if (type == "CDIR" || type == "PDIR") return std::nullopt;
    if (type == "DIR") entry.isDirectory = true;
    if (type.find("SYMLINK") != std::string::npos || type.find("SLINK") != std::string::npos) entry.isLink = true;
    if (entry.isDirectory) entry.size = 0;
    if (entry.name.empty() || entry.name == L"." || entry.name == L"..") return std::nullopt;
    return entry;
}

std::optional<RemoteEntry> ParseListLine(const std::string& rawLine, int currentYear, bool utf8) {
    std::string line = rawLine;
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
    if (TrimAscii(line).empty()) return std::nullopt;
    std::vector<Token> tokens = Tokenize(line);
    if (tokens.size() == 2 && Upper(tokens[0].text) == "TOTAL" && IsDigits(tokens[1].text)) return std::nullopt;

    RemoteEntry entry;
    auto finish = [&](const std::string& name) -> std::optional<RemoteEntry> {
        if (name.empty() || name == "." || name == "..") return std::nullopt;
        entry.name = DecodeName(name, utf8);
        return entry;
    };

    // Windows/IIS: "10-08-26  09:15AM       <DIR>          name"
    if (tokens.size() >= 4 && (tokens[0].text.size() == 8 || tokens[0].text.size() == 10) && tokens[0].text[2] == '-' &&
        tokens[0].text[5] == '-') {
        std::string date = tokens[0].text, time = Upper(tokens[1].text);
        int month = std::atoi(date.substr(0, 2).c_str()), day = std::atoi(date.substr(3, 2).c_str());
        int year = std::atoi(date.substr(6).c_str());
        if (year < 100) year += year < 70 ? 2000 : 1900;
        int hour = std::atoi(time.c_str()), minute = 0;
        size_t colon = time.find(':');
        if (colon != std::string::npos) minute = std::atoi(time.c_str() + colon + 1);
        bool pm = time.find("PM") != std::string::npos, am = time.find("AM") != std::string::npos;
        if (pm && hour < 12) hour += 12;
        if (am && hour == 12) hour = 0;
        if (Upper(tokens[2].text) == "<DIR>") entry.isDirectory = true;
        else if (IsDigits(tokens[2].text)) entry.size = std::stoull(tokens[2].text);
        else return finish(TrimAscii(line));
        entry.modifiedUtc = ToFileTime(year, month, day, hour, minute, 0).value_or(0);
        return finish(line.substr(tokens[3].start));
    }

    // Unix: "drwxr-xr-x  2 user group  4096 Jan 15 12:30 name" (owner/group optional).
    if (!tokens.empty() && tokens[0].text.size() >= 10 && std::string("-dlbcps").find(tokens[0].text[0]) != std::string::npos) {
        for (size_t m = 1; m + 2 < tokens.size(); ++m) {
            int month = MonthIndex(tokens[m].text);
            if (!month || !IsDigits(tokens[m + 1].text) || !IsDigits(tokens[m - 1].text)) continue;
            const std::string& timeOrYear = tokens[m + 2].text;
            int day = std::atoi(tokens[m + 1].text.c_str());
            int year = currentYear, hour = 0, minute = 0;
            size_t colon = timeOrYear.find(':');
            if (colon != std::string::npos) {
                hour = std::atoi(timeOrYear.c_str());
                minute = std::atoi(timeOrYear.c_str() + colon + 1);
            } else if (IsDigits(timeOrYear) && timeOrYear.size() == 4) {
                year = std::atoi(timeOrYear.c_str());
            } else {
                continue;
            }
            if (m + 3 >= tokens.size()) return std::nullopt;
            char kind = tokens[0].text[0];
            entry.isDirectory = kind == 'd';
            entry.isLink = kind == 'l';
            entry.size = entry.isDirectory ? 0 : std::stoull(tokens[m - 1].text);
            entry.permissions = DecodeName(tokens[0].text.substr(1, 9), false);
            entry.modifiedUtc = ToFileTime(year, month, day, hour, minute, 0).value_or(0);
            std::string name = line.substr(tokens[m + 3].start);
            if (entry.isLink) {
                size_t arrow = name.find(" -> ");
                if (arrow != std::string::npos) name.resize(arrow);
            }
            return finish(name);
        }
    }

    // Unknown format: show the line as a plain file name rather than hide it.
    return finish(TrimAscii(line));
}

std::vector<std::string> ParseFeatures(const std::string& text) {
    std::vector<std::string> features;
    size_t start = 0;
    bool first = true;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = text.substr(start, end - start);
        start = end + 1;
        if (first) {
            first = false;  // "Features:" header
            if (end == text.size()) break;
            continue;
        }
        std::string trimmed = TrimAscii(line);
        if (trimmed.empty() || Upper(trimmed) == "END") {
            if (end == text.size()) break;
            continue;
        }
        size_t sp = trimmed.find(' ');
        features.push_back(Upper(trimmed.substr(0, sp)));
        if (end == text.size()) break;
    }
    return features;
}

bool HasFeature(const std::vector<std::string>& features, const char* name) {
    return std::find(features.begin(), features.end(), Upper(name)) != features.end();
}

std::optional<uint64_t> ParseFtpTime(const std::string& rawText) {
    std::string text = TrimAscii(rawText);
    if (text.size() < 14 || !IsDigits(text.substr(0, 14))) return std::nullopt;
    auto num = [&](size_t pos, size_t len) { return std::atoi(text.substr(pos, len).c_str()); };
    return ToFileTime(num(0, 4), num(4, 2), num(6, 2), num(8, 2), num(10, 2), num(12, 2));
}

std::optional<std::string> ParsePwdReply(const std::string& text) {
    size_t open = text.find('"');
    if (open == std::string::npos) return std::nullopt;
    std::string path;
    for (size_t i = open + 1; i < text.size(); ++i) {
        if (text[i] == '"') {
            if (i + 1 < text.size() && text[i + 1] == '"') {
                path += '"';
                ++i;
                continue;
            }
            return path;
        }
        path += text[i];
    }
    return std::nullopt;
}

std::wstring DecodeName(const std::string& bytes, bool utf8) {
    if (bytes.empty()) return L"";
    UINT codePage = utf8 ? CP_UTF8 : CP_ACP;
    DWORD flags = utf8 ? MB_ERR_INVALID_CHARS : 0;
    int n = MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    if (n <= 0 && utf8) return DecodeName(bytes, false);  // not valid UTF-8 after all
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(codePage, flags, bytes.data(), static_cast<int>(bytes.size()), out.data(), n);
    return out;
}

std::string EncodeName(const std::wstring& name, bool utf8) {
    if (name.empty()) return "";
    UINT codePage = utf8 ? CP_UTF8 : CP_ACP;
    int n = WideCharToMultiByte(codePage, 0, name.data(), static_cast<int>(name.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(codePage, 0, name.data(), static_cast<int>(name.size()), out.data(), n, nullptr, nullptr);
    return out;
}

std::string MaskForLog(const std::string& commandLine) {
    if (commandLine.size() >= 4 && Upper(commandLine.substr(0, 4)) == "PASS") return "PASS ****";
    return commandLine;
}

std::optional<FtpUrl> ParseFtpUrl(const std::wstring& rawText) {
    std::wstring text = rawText;
    while (!text.empty() && std::iswspace(text.front())) text.erase(text.begin());
    while (!text.empty() && std::iswspace(text.back())) text.pop_back();
    if (text.size() >= 6 && _wcsnicmp(text.c_str(), L"ftp://", 6) == 0) text = text.substr(6);
    FtpUrl url;
    size_t slash = text.find(L'/');
    std::wstring authority = slash == std::wstring::npos ? text : text.substr(0, slash);
    url.path = slash == std::wstring::npos ? L"" : PercentDecode(text.substr(slash));
    size_t at = authority.rfind(L'@');
    if (at != std::wstring::npos) {
        std::wstring userInfo = authority.substr(0, at);
        authority = authority.substr(at + 1);
        size_t colon = userInfo.find(L':');
        url.user = PercentDecode(userInfo.substr(0, colon));
        if (colon != std::wstring::npos) url.password = PercentDecode(userInfo.substr(colon + 1));
    }
    std::wstring portText;
    if (!authority.empty() && authority.front() == L'[') {  // [IPv6]:port
        size_t close = authority.find(L']');
        if (close == std::wstring::npos) return std::nullopt;
        url.host = authority.substr(1, close - 1);
        if (close + 1 < authority.size() && authority[close + 1] == L':') portText = authority.substr(close + 2);
    } else {
        size_t colon = authority.rfind(L':');
        url.host = authority.substr(0, colon);
        if (colon != std::wstring::npos) portText = authority.substr(colon + 1);
    }
    if (!portText.empty()) {
        if (portText.size() > 5 || !std::all_of(portText.begin(), portText.end(), [](wchar_t c) { return std::iswdigit(c); }))
            return std::nullopt;
        unsigned port = static_cast<unsigned>(std::stoul(portText));
        if (port == 0 || port > 65535) return std::nullopt;
        url.port = static_cast<uint16_t>(port);
    }
    if (url.host.empty()) return std::nullopt;
    return url;
}

}  // namespace ftp
