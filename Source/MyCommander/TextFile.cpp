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
