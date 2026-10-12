#include "TextFile.h"

#include "PathUtil.h"

#include <windows.h>

#include <algorithm>

namespace fs = std::filesystem;

namespace mc {

namespace {

// Strict UTF-8 decode; returns an empty string if `len` is 0 or the bytes
// are not valid UTF-8, so the caller can fall back to a raw byte widen.
std::wstring DecodeUtf8Strict(const char* data, size_t len) {
    if (len == 0) return L"";
    int wideLen = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(len), nullptr, 0);
    if (wideLen <= 0) return L"";
    std::wstring result(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, data, static_cast<int>(len), result.data(), wideLen);
    return result;
}

std::wstring DecodeWithFallback(const char* data, size_t len) {
    std::wstring text = DecodeUtf8Strict(data, len);
    if (text.empty() && len > 0) {
        // Byte-wise ANSI/Latin-1 widen as a last resort. `char` is signed on
        // MSVC, so widening it directly would sign-extend any byte >= 0x80
        // into 0xFF80.. instead of 0x0080.. — go through unsigned char first.
        text.resize(len);
        for (size_t i = 0; i < len; ++i) {
            text[i] = static_cast<wchar_t>(static_cast<unsigned char>(data[i]));
        }
    }
    return text;
}

std::wstring ExpandTabs(const std::wstring& line, int tabWidth) {
    std::wstring out;
    out.reserve(line.size());
    for (wchar_t ch : line) {
        if (ch == L'\t') {
            int spaces = tabWidth - (static_cast<int>(out.size()) % tabWidth);
            out.append(static_cast<size_t>(spaces), L' ');
        } else {
            out.push_back(ch);
        }
    }
    return out;
}

// Reads up to the first 3 bytes of `in` (already positioned at the start of
// the file) to detect a UTF-8 or UTF-16LE BOM, then seeks `in` to just past
// whatever was detected (0 bytes if neither matched) so the caller can carry
// on reading from exactly the start of the file's actual content.
struct BomInfo {
    TextEncoding encoding = TextEncoding::Utf8OrAnsi;
    uint64_t contentStart = 0;
};

BomInfo DetectBomAndSeekPastIt(std::ifstream& in) {
    char header[3] = {0, 0, 0};
    in.read(header, 3);
    std::streamsize got = in.gcount();

    BomInfo bom;
    if (got >= 3 && static_cast<unsigned char>(header[0]) == 0xEF && static_cast<unsigned char>(header[1]) == 0xBB &&
        static_cast<unsigned char>(header[2]) == 0xBF) {
        bom = {TextEncoding::Utf8OrAnsi, 3};
    } else if (got >= 2 && static_cast<unsigned char>(header[0]) == 0xFF &&
              static_cast<unsigned char>(header[1]) == 0xFE) {
        bom = {TextEncoding::Utf16Le, 2};
    }

    in.clear();
    in.seekg(static_cast<std::streamoff>(bom.contentStart));
    return bom;
}

// Records a line spanning [lineStart, lineEnd) into `index` and returns the
// byte offset the next line starts at (lineEnd plus however many bytes wide
// this encoding's line-ending sequence is).
uint64_t RecordLine(TextFileIndex& index, uint64_t lineStart, uint64_t lineEnd, uint64_t endingWidth) {
    index.lineStarts.push_back(lineStart);
    index.lineEnds.push_back(lineEnd);
    return lineEnd + endingWidth;
}

void IndexUtf8OrAnsiLines(std::ifstream& in, TextFileIndex& index) {
    constexpr size_t kChunkSize = 1 << 20; // 1 MB
    std::vector<char> chunk(kChunkSize);
    // Both start at the content start on purpose (chunk position vs line start), then diverge.
    // cppcheck-suppress duplicateAssignExpression
    uint64_t basePos = index.contentStart;
    uint64_t lineStart = index.contentStart;

    while (true) {
        in.read(chunk.data(), static_cast<std::streamsize>(kChunkSize));
        size_t got = static_cast<size_t>(in.gcount());
        if (got == 0) break;
        for (size_t i = 0; i < got; ++i) {
            if (static_cast<unsigned char>(chunk[i]) == 0x0A) {
                uint64_t nlPos = basePos + i;
                lineStart = RecordLine(index, lineStart, nlPos, 1);
            }
        }
        basePos += got;
    }
    if (basePos > lineStart) {
        RecordLine(index, lineStart, basePos, 0);
    }
}

// Same shape as IndexUtf8OrAnsiLines but scans 2-byte UTF-16LE code units
// for the pattern 0x0A,0x00 ('\n'). Reads are trimmed to an even byte count
// and any odd trailing byte is carried into the next chunk so a code unit
// straddling a chunk boundary is never split mid-scan.
void IndexUtf16LeLines(std::ifstream& in, TextFileIndex& index) {
    constexpr size_t kChunkSize = 1 << 20; // 1 MB
    std::vector<char> chunk(kChunkSize);
    std::string carry;
    // Both start at the content start on purpose (chunk position vs line start), then diverge.
    // cppcheck-suppress duplicateAssignExpression
    uint64_t basePos = index.contentStart;
    uint64_t lineStart = index.contentStart;

    while (true) {
        size_t carryLen = carry.size();
        std::copy(carry.begin(), carry.end(), chunk.begin());
        in.read(chunk.data() + carryLen, static_cast<std::streamsize>(kChunkSize - carryLen));
        size_t got = carryLen + static_cast<size_t>(in.gcount());
        if (got == 0) break;

        size_t usable = got - (got % 2);
        for (size_t i = 0; i + 2 <= usable; i += 2) {
            if (static_cast<unsigned char>(chunk[i]) == 0x0A && static_cast<unsigned char>(chunk[i + 1]) == 0x00) {
                uint64_t nlPos = basePos + i;
                lineStart = RecordLine(index, lineStart, nlPos, 2);
            }
        }
        carry.assign(chunk.begin() + usable, chunk.begin() + got);
        basePos += usable;
    }
    uint64_t fileEnd = basePos + carry.size();
    if (fileEnd > lineStart) {
        RecordLine(index, lineStart, fileEnd, 0);
    }
}

} // namespace

TextFileIndex IndexTextFileForViewing(const fs::path& path, int tabWidth) {
    TextFileIndex index;
    index.path = path;
    index.tabWidth = tabWidth;

    std::ifstream in(ToLongPath(path), std::ios::binary);
    if (!in) {
        index.error = L"cannot open file";
        return index;
    }

    BomInfo bom = DetectBomAndSeekPastIt(in);
    index.encoding = bom.encoding;
    index.contentStart = bom.contentStart;

    if (index.encoding == TextEncoding::Utf16Le) {
        IndexUtf16LeLines(in, index);
    } else {
        IndexUtf8OrAnsiLines(in, index);
    }

    index.ok = true;
    return index;
}

namespace {

std::wstring DecodeCodePage(UINT codePage, const char* data, size_t len, DWORD flags = 0) {
    if (len == 0) return L"";
    int wideLen = MultiByteToWideChar(codePage, flags, data, static_cast<int>(len), nullptr, 0);
    if (wideLen <= 0) return L"";
    std::wstring result(static_cast<size_t>(wideLen), L'\0');
    MultiByteToWideChar(codePage, flags, data, static_cast<int>(len), result.data(), wideLen);
    return result;
}

std::wstring DecodeEightBitLine(const std::string& bytes, TextEncodingChoice encoding) {
    size_t len = bytes.size();
    if (len > 0 && bytes[len - 1] == '\r') --len;
    if (encoding == TextEncodingChoice::Utf8) return DecodeCodePage(CP_UTF8, bytes.data(), len);
    if (encoding == TextEncodingChoice::Ansi) return DecodeCodePage(CP_ACP, bytes.data(), len);
    std::wstring text = DecodeUtf8Strict(bytes.data(), len);  // Auto: UTF-8 if valid...
    if (text.empty() && len > 0) text = DecodeCodePage(CP_ACP, bytes.data(), len);  // ...else ANSI
    return text;
}

constexpr size_t kMaxRawLineBytes = 256 * 1024;

}  // namespace

bool ForEachTextLine(const fs::path& path, TextEncodingChoice encoding,
                     const std::function<bool(size_t, const std::wstring&)>& onLine,
                     const std::function<bool(const TextFileProbe&)>& onProbe) {
    std::ifstream in(ToLongPath(path), std::ios::binary);
    if (!in) return false;

    constexpr size_t kChunkSize = 1 << 16;
    std::vector<char> chunk(kChunkSize);
    in.read(chunk.data(), static_cast<std::streamsize>(kChunkSize));
    size_t got = static_cast<size_t>(in.gcount());
    if (in.bad()) return false;

    // BOM: decides the encoding under Auto; skipped when it matches an explicit choice.
    auto byteAt = [&](size_t i) { return i < got ? static_cast<unsigned char>(chunk[i]) : 0; };
    bool bomUtf8 = got >= 3 && byteAt(0) == 0xEF && byteAt(1) == 0xBB && byteAt(2) == 0xBF;
    bool bomLe = got >= 2 && byteAt(0) == 0xFF && byteAt(1) == 0xFE;
    bool bomBe = got >= 2 && byteAt(0) == 0xFE && byteAt(1) == 0xFF;
    TextEncodingChoice effective = encoding;
    if (encoding == TextEncodingChoice::Auto) {
        if (bomLe) effective = TextEncodingChoice::Utf16Le;
        else if (bomBe) effective = TextEncodingChoice::Utf16Be;
        else if (bomUtf8) effective = TextEncodingChoice::Utf8;
    }
    size_t skip = 0;
    if (bomUtf8 && (effective == TextEncodingChoice::Utf8 || effective == TextEncodingChoice::Auto)) skip = 3;
    if (bomLe && effective == TextEncodingChoice::Utf16Le) skip = 2;
    if (bomBe && effective == TextEncodingChoice::Utf16Be) skip = 2;

    if (onProbe) {
        TextFileProbe probe;
        probe.head.assign(reinterpret_cast<unsigned char*>(chunk.data()),
                          reinterpret_cast<unsigned char*>(chunk.data()) + (std::min)(got, size_t{8192}));
        probe.utf16Bom = bomLe || bomBe;
        if (!onProbe(probe)) return true;
    }

    const bool wide = effective == TextEncodingChoice::Utf16Le || effective == TextEncodingChoice::Utf16Be;
    size_t lineNumber = 0;
    std::string line8;
    std::wstring line16;
    bool truncated = false;
    unsigned char pendingByte = 0;
    bool havePending = false;

    auto emit8 = [&]() {
        ++lineNumber;
        bool keep = onLine(lineNumber, DecodeEightBitLine(line8, effective));
        line8.clear();
        truncated = false;
        return keep;
    };
    auto emit16 = [&]() {
        ++lineNumber;
        if (!line16.empty() && line16.back() == L'\r') line16.pop_back();
        bool keep = onLine(lineNumber, line16);
        line16.clear();
        truncated = false;
        return keep;
    };

    size_t start = skip;
    while (got > 0) {
        for (size_t i = start; i < got; ++i) {
            unsigned char b = static_cast<unsigned char>(chunk[i]);
            if (!wide) {
                if (b == '\n') {
                    if (!emit8()) return true;
                } else if (!truncated) {
                    line8.push_back(static_cast<char>(b));
                    if (line8.size() >= kMaxRawLineBytes) truncated = true;
                }
                continue;
            }
            if (!havePending) {
                pendingByte = b;
                havePending = true;
                continue;
            }
            havePending = false;
            wchar_t unit = effective == TextEncodingChoice::Utf16Le ? static_cast<wchar_t>(pendingByte | (b << 8))
                                                                   : static_cast<wchar_t>((pendingByte << 8) | b);
            if (unit == L'\n') {
                if (!emit16()) return true;
            } else if (!truncated) {
                line16.push_back(unit);
                if (line16.size() * 2 >= kMaxRawLineBytes) truncated = true;
            }
        }
        start = 0;
        in.read(chunk.data(), static_cast<std::streamsize>(kChunkSize));
        got = static_cast<size_t>(in.gcount());
        if (in.bad()) return false;
    }
    // The last line, if the file doesn't end with a line break.
    if (wide ? !line16.empty() : !line8.empty()) wide ? emit16() : emit8();
    return true;
}

std::wstring ReadIndexedLine(std::ifstream& in, const TextFileIndex& index, size_t lineNumber) {
    if (lineNumber >= index.lineStarts.size()) return L"";

    uint64_t start = index.lineStarts[lineNumber];
    uint64_t end = index.lineEnds[lineNumber];
    size_t len = static_cast<size_t>(end - start);

    std::vector<char> buf(len);
    in.clear();
    in.seekg(static_cast<std::streamoff>(start));
    if (len > 0) in.read(buf.data(), static_cast<std::streamsize>(len));

    std::wstring content;
    if (index.encoding == TextEncoding::Utf16Le) {
        content.assign(reinterpret_cast<const wchar_t*>(buf.data()), len / 2);
        if (!content.empty() && content.back() == L'\r') content.pop_back();
    } else {
        if (len > 0 && buf[len - 1] == '\r') --len;
        content = DecodeWithFallback(buf.data(), len);
    }
    return ExpandTabs(content, index.tabWidth);
}

} // namespace mc
