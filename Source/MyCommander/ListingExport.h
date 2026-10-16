#pragma once

// LIST-012: export a panel listing to a CSV or tab-separated text file. Pure
// formatting and rules -- no Console/Dialog dependency; the form, the row
// gathering and the file write live in main.cpp.

#include "Panel.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mc {

enum class ListingFormat { Csv, Text };

struct ListingRow {
    std::wstring name;          // relative to the exported folder (a path when subfolders are included)
    std::wstring typeText;      // extension without the dot, or "<DIR>"
    uint64_t sizeBytes = 0;     // 0 for a folder
    std::wstring modified;      // "YYYY-MM-DD HH:MM:SS" local time, empty if unknown
    std::wstring attributes;    // "RHSA" letters, '-' where unset
    int label = 0;              // LIST-009 number, 0 = none
    std::wstring fullPath;
};

// A folder shows "<DIR>"; a file shows its extension (no dot, none for a dotfile or no extension).
std::wstring ListingTypeText(const std::wstring& name, bool isDirectory);

// Same letters as the panel's attribute column: R H S A, '-' where unset.
std::wstring ListingAttributeLetters(uint32_t attributes);

// A raw FILETIME (UTC, 100 ns ticks) as local "YYYY-MM-DD HH:MM:SS"; empty for 0 or on failure.
std::wstring FormatListingTime(uint64_t filetime);

// One CSV field per RFC 4180: quoted when it holds a comma, quote, line break or a leading/trailing
// space (quotes doubled). A field starting with = + - @ (or a tab/CR) gets a leading apostrophe first,
// so a spreadsheet does not evaluate a file name as a formula.
std::wstring CsvField(const std::wstring& text, bool formulaGuard = true);

// One tab-separated field: tabs and line breaks become spaces.
std::wstring TextField(const std::wstring& text);

// Header row plus one line per row, CRLF line endings.
std::wstring FormatListing(const std::vector<ListingRow>& rows, ListingFormat format);

// Default output name "listing-<folder>-<yyyymmdd-hhmm>.<csv|txt>"; characters not allowed in a
// file name become '_'. `folderName` is the exported folder's own name (a drive root gives "drive").
std::wstring DefaultListingFileName(const std::wstring& folderName, int year, int month, int day, int hour,
                                    int minute, ListingFormat format);

// Label lookup by full path (LIST-009); null = no labels.
using ListingLabelOf = std::function<int(const std::filesystem::path&)>;

// One row from already-known facts. `relativeName` is shown as the Name column.
ListingRow MakeListingRow(const std::wstring& relativeName, const std::filesystem::path& fullPath, bool isDirectory,
                          uint64_t sizeBytes, uint32_t attributes, uint64_t filetime, int label);

// The panel's listing (no disk call), without "..". `selectionOnly` keeps only selected entries.
std::vector<ListingRow> GatherPanelRows(const std::vector<Entry>& entries, const std::filesystem::path& folder,
                                        bool selectionOnly, const ListingLabelOf& labelOf);

struct ListingGather {
    std::vector<ListingRow> rows;
    bool cancelled = false;
    bool truncated = false;  // hit the row cap
};

// Every file and folder below `root` (reparse points listed but not entered, SEC-006), names relative to
// `root`. At most `maxRows`; `cancelPoll` is polled per folder.
ListingGather GatherSubtreeRows(const std::filesystem::path& root, const ListingLabelOf& labelOf,
                                const std::function<bool()>& cancelPoll = {}, size_t maxRows = 200000);

// The output must never lie inside the exported folder (an export must not change what it lists).
bool IsExportOutputInsideFolder(const std::filesystem::path& folder, const std::filesystem::path& output);

}  // namespace mc
