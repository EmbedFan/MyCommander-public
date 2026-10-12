#pragma once

#include <cstddef>
#include <cstdint>
#include <istream>
#include <optional>
#include <string>
#include <vector>

namespace mc {

// VEE-005: the selectable hex-view item counts (bytes per row), in cycle order.
inline constexpr size_t kHexItemCounts[] = {8, 16, 32};

// Setting value meaning "fit to the terminal width". Not a real item count.
inline constexpr size_t kHexFitSetting = 0;

// The next setting after `setting`: 8 -> 16 -> 32 -> fit -> 8. Any unknown
// value returns 8, the default.
size_t NextHexItemSetting(size_t setting);

// The items per row a setting resolves to. Fit returns the largest of
// kHexItemCounts whose full row fits `consoleWidth`, or 8 if none does. A
// fixed count is returned unchanged.
size_t ResolveHexItems(size_t setting, size_t consoleWidth);

// Columns one full row occupies: a 16-digit offset, two spaces, three columns
// per byte ("XX "), one separator space, then one ASCII column per byte.
size_t HexRowColumns(size_t bytesPerRow);

// Formats one hex-dump row covering `count` bytes at file offset `offset`,
// padded to `bytesPerRow` so a partial final row keeps its ASCII column
// aligned. Only the columns [firstColumn, firstColumn + maxColumns) of the
// full row are returned, for horizontal panning. Non-printable bytes, and
// anything outside 0x20-0x7E, appear as '.' in the ASCII column.
std::wstring FormatHexRow(uint64_t offset, const uint8_t* bytes, size_t count, size_t bytesPerRow,
                          size_t firstColumn, size_t maxColumns);

// Offset of the first byte of the row containing `byteOffset`. `bytesPerRow`
// must be nonzero.
uint64_t HexRowStart(uint64_t byteOffset, size_t bytesPerRow);

// Largest first-visible byte that still fills a screen of `visibleRows` rows,
// row-aligned to `bytesPerRow`. Zero when the whole file already fits.
uint64_t HexMaxAnchor(uint64_t fileSize, size_t bytesPerRow, size_t visibleRows);

// Largest horizontal pan, in columns, for a row shown in `visibleColumns`.
// Zero when the whole row already fits.
size_t HexMaxPan(size_t bytesPerRow, size_t visibleColumns);

// Index of the line containing `byteOffset`, given ascending line start
// offsets as built by TextFileIndex::lineStarts. Returns 0 for an empty index
// or a byte before the first line.
size_t LineIndexForByte(const std::vector<uint64_t>& lineStarts, uint64_t byteOffset);

// Parses a hex byte pattern such as "4D 5A" or "4d5a". Whitespace between
// digits is ignored. Returns nullopt for an empty pattern, an odd number of
// digits, or any character that isn't a hex digit or whitespace.
std::optional<std::vector<uint8_t>> ParseHexPattern(const std::wstring& text);

// Finds the first occurrence of `pattern` in a file of `fileSize` bytes read
// from `in`, searching match start offsets in [fromByte, fileSize) and then
// wrapping to [0, fromByte). Returns the match's start offset, or nullopt if
// `pattern` is empty or not found. When `caseFoldAscii` is true, A-Z match
// a-z in both the pattern and the file; other bytes compare exactly.
// Reads the file in fixed-size chunks, so memory use is bounded regardless of
// file size.
std::optional<uint64_t> FindBytePattern(std::istream& in, uint64_t fileSize, const std::vector<uint8_t>& pattern,
                                        uint64_t fromByte, bool caseFoldAscii);

} // namespace mc
