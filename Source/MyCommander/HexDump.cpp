#include "HexDump.h"

#include <algorithm>

namespace mc {

namespace {

constexpr size_t kSearchChunkBytes = 64 * 1024;

wchar_t HexDigit(unsigned value) { return value < 10 ? L'0' + static_cast<wchar_t>(value) : L'A' + static_cast<wchar_t>(value - 10); }

int HexValue(wchar_t c) {
    if (c >= L'0' && c <= L'9') return c - L'0';
    if (c >= L'A' && c <= L'F') return c - L'A' + 10;
    if (c >= L'a' && c <= L'f') return c - L'a' + 10;
    return -1;
}

uint8_t FoldAscii(uint8_t c, bool fold) { return (fold && c >= 'A' && c <= 'Z') ? static_cast<uint8_t>(c + ('a' - 'A')) : c; }

// Scans match start offsets in [firstStart, endStart) — a match must begin
// within that range, but may extend past endStart.
std::optional<uint64_t> ScanStarts(std::istream& in, uint64_t fileSize, const std::vector<uint8_t>& pattern,
                                   uint64_t firstStart, uint64_t endStart, bool fold) {
    const size_t m = pattern.size();
    std::vector<uint8_t> buffer;
    uint64_t pos = firstStart;
    while (pos < endStart) {
        size_t startsThisChunk = static_cast<size_t>(std::min<uint64_t>(kSearchChunkBytes, endStart - pos));
        size_t wanted = std::min<uint64_t>(startsThisChunk + m - 1, fileSize - pos);
        buffer.resize(wanted);
        in.clear();
        in.seekg(static_cast<std::streamoff>(pos));
        in.read(reinterpret_cast<char*>(buffer.data()), static_cast<std::streamsize>(wanted));
        size_t got = static_cast<size_t>(in.gcount());
        buffer.resize(got);
        for (size_t i = 0; i + m <= got && i < startsThisChunk; ++i) {
            bool matched = true;
            for (size_t j = 0; j < m; ++j) {
                if (FoldAscii(buffer[i + j], fold) != FoldAscii(pattern[j], fold)) {
                    matched = false;
                    break;
                }
            }
            if (matched) return pos + i;
        }
        if (got < wanted) break;
        pos += startsThisChunk;
    }
    return std::nullopt;
}

} // namespace

size_t NextHexItemSetting(size_t setting) {
    if (setting == kHexFitSetting) return kHexItemCounts[0];
    const size_t count = sizeof(kHexItemCounts) / sizeof(kHexItemCounts[0]);
    for (size_t i = 0; i < count; ++i) {
        if (kHexItemCounts[i] == setting) {
            return i + 1 < count ? kHexItemCounts[i + 1] : kHexFitSetting;
        }
    }
    return kHexItemCounts[0];
}

size_t ResolveHexItems(size_t setting, size_t consoleWidth) {
    if (setting != kHexFitSetting) return setting;
    size_t best = kHexItemCounts[0];
    for (size_t count : kHexItemCounts) {
        if (HexRowColumns(count) <= consoleWidth) best = count;
    }
    return best;
}

size_t HexRowColumns(size_t bytesPerRow) { return 16 + 2 + bytesPerRow * 3 + 1 + bytesPerRow; }

std::wstring FormatHexRow(uint64_t offset, const uint8_t* bytes, size_t count, size_t bytesPerRow,
                          size_t firstColumn, size_t maxColumns) {
    std::wstring row;
    row.reserve(HexRowColumns(bytesPerRow));
    for (int shift = 60; shift >= 0; shift -= 4) {
        row.push_back(HexDigit(static_cast<unsigned>((offset >> shift) & 0xF)));
    }
    row += L"  ";
    for (size_t i = 0; i < bytesPerRow; ++i) {
        if (i < count) {
            row.push_back(HexDigit(bytes[i] >> 4));
            row.push_back(HexDigit(bytes[i] & 0xF));
        } else {
            row += L"  ";
        }
        row.push_back(L' ');
    }
    row.push_back(L' ');
    for (size_t i = 0; i < bytesPerRow; ++i) {
        if (i >= count) {
            row.push_back(L' ');
        } else {
            row.push_back((bytes[i] >= 0x20 && bytes[i] <= 0x7E) ? static_cast<wchar_t>(bytes[i]) : L'.');
        }
    }
    size_t start = std::min(firstColumn, row.size());
    return row.substr(start, maxColumns);
}

uint64_t HexRowStart(uint64_t byteOffset, size_t bytesPerRow) {
    return byteOffset - byteOffset % bytesPerRow;
}

uint64_t HexMaxAnchor(uint64_t fileSize, size_t bytesPerRow, size_t visibleRows) {
    uint64_t totalRows = (fileSize + bytesPerRow - 1) / bytesPerRow;
    return totalRows > visibleRows ? (totalRows - visibleRows) * bytesPerRow : 0;
}

size_t HexMaxPan(size_t bytesPerRow, size_t visibleColumns) {
    size_t columns = HexRowColumns(bytesPerRow);
    return columns > visibleColumns ? columns - visibleColumns : 0;
}

size_t LineIndexForByte(const std::vector<uint64_t>& lineStarts, uint64_t byteOffset) {
    auto it = std::upper_bound(lineStarts.begin(), lineStarts.end(), byteOffset);
    if (it == lineStarts.begin()) return 0;
    return static_cast<size_t>(std::distance(lineStarts.begin(), it) - 1);
}

std::optional<std::vector<uint8_t>> ParseHexPattern(const std::wstring& text) {
    std::vector<uint8_t> bytes;
    int pending = -1;
    for (wchar_t c : text) {
        if (c == L' ' || c == L'\t') continue;
        int value = HexValue(c);
        if (value < 0) return std::nullopt;
        if (pending < 0) {
            pending = value;
        } else {
            bytes.push_back(static_cast<uint8_t>(pending * 16 + value));
            pending = -1;
        }
    }
    if (pending >= 0 || bytes.empty()) return std::nullopt;
    return bytes;
}

std::optional<uint64_t> FindBytePattern(std::istream& in, uint64_t fileSize, const std::vector<uint8_t>& pattern,
                                        uint64_t fromByte, bool caseFoldAscii) {
    if (pattern.empty() || fileSize < pattern.size()) return std::nullopt;
    const uint64_t validStarts = fileSize - pattern.size() + 1;
    if (fromByte < validStarts) {
        if (auto hit = ScanStarts(in, fileSize, pattern, fromByte, validStarts, caseFoldAscii)) return hit;
    }
    uint64_t wrapEnd = std::min(fromByte, validStarts);
    if (wrapEnd > 0) return ScanStarts(in, fileSize, pattern, 0, wrapEnd, caseFoldAscii);
    return std::nullopt;
}

} // namespace mc
