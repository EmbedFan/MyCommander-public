#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

namespace mc {

// The encoding a TextFileIndex was built with. Both cases still fall back to
// ANSI/Latin-1 byte-widening per line if UTF-8 decoding fails (see
// ReadIndexedLine) — this only distinguishes whether a BOM said the file is
// UTF-16LE, since that needs 2-bytes-per-code-unit decoding instead.
enum class TextEncoding { Utf8OrAnsi, Utf16Le };

// VEE-004's large-file handling: a byte-offset index of every line in a text
// file, built by IndexTextFileForViewing with a single bounded-memory scan —
// the file's content is never loaded into memory as a whole, so the viewer
// can page through a file of any size instead of capping what it will read.
// Individual lines are decoded on demand by ReadIndexedLine as they scroll
// into view or are searched, not up front. Deliberately independent of
// Console so it's unit-testable on its own (VEE-003/VEE-004).
struct TextFileIndex {
    bool ok = false;
    std::wstring error;
    std::filesystem::path path;
    TextEncoding encoding = TextEncoding::Utf8OrAnsi;
    uint64_t contentStart = 0;          // byte offset in the file past any BOM
    std::vector<uint64_t> lineStarts;   // byte offset of each line's first content byte
    std::vector<uint64_t> lineEnds;     // byte offset just past each line's last content byte
                                        // (i.e. before its \r\n/\n line ending)
    int tabWidth = 4;
};

// Scans `path` once to build a TextFileIndex: detects a UTF-8 or UTF-16LE
// byte-order mark (otherwise assumes UTF-8/ANSI, decided per line at read
// time), then walks the rest of the file in fixed-size chunks recording the
// byte offset of every line boundary. Never holds more than one chunk of the
// file's content in memory at a time, regardless of file size.
TextFileIndex IndexTextFileForViewing(const std::filesystem::path& path, int tabWidth = 4);

inline size_t LineCount(const TextFileIndex& index) { return index.lineStarts.size(); }

// Reads, decodes, and tab-expands line `lineNumber` (0-based) from `index`
// using `in` — an already-open binary ifstream on `index.path`, passed in
// (rather than opened internally) so scrolling or searching through a huge
// file's many lines reuses one open handle instead of reopening it per line.
// Returns an empty string for an out-of-range `lineNumber`.
std::wstring ReadIndexedLine(std::ifstream& in, const TextFileIndex& index, size_t lineNumber);

// SRC-005: how Find files decodes a file's content.
//   Auto    -- a BOM decides (UTF-8 / UTF-16LE / UTF-16BE); without one, each
//              line is UTF-8 if valid, else the system ANSI code page.
//   Utf8    -- UTF-8 (invalid bytes become U+FFFD).
//   Ansi    -- the system ANSI code page (CP_ACP).
//   Utf16Le / Utf16Be -- 2-byte code units, little / big endian.
// An explicit choice still skips that encoding's own BOM if the file has one.
enum class TextEncodingChoice { Auto, Utf8, Ansi, Utf16Le, Utf16Be };

// What ForEachTextLine reports at the start of a file, for binary
// detection: the first few KB of raw bytes and whether a UTF-16 BOM was seen.
struct TextFileProbe {
    std::vector<unsigned char> head;
    bool utf16Bom = false;
};

// SRC-004: one forward pass over `path` in fixed-size chunks -- bounded
// memory whatever the file size (VEE-004 precedent) -- calling `onLine` with
// each line's 1-based number and decoded text ('\r' stripped, tabs NOT
// expanded). A line longer than 256 KB of raw bytes is cut there (the rest
// is skipped up to its line break) so one huge line can't exhaust memory.
// `onProbe`, if given, is called once before any line with the file's head;
// returning false from it (e.g. "binary, skip") or from `onLine` stops
// reading. Returns false only if the file can't be opened or read.
bool ForEachTextLine(const std::filesystem::path& path, TextEncodingChoice encoding,
                     const std::function<bool(size_t lineNumber, const std::wstring& line)>& onLine,
                     const std::function<bool(const TextFileProbe&)>& onProbe = {});

} // namespace mc
