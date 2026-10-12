#pragma once

// SRC-008: the text-file difference viewer's pure logic -- the line diff
// (linear-space Myers, with prefix/suffix trimming and a bounded edit count),
// the character-level highlight inside a changed line, difference
// navigation, the first differing byte of two binary files, and which two
// files "Compare contents" compares. No Console/Panel dependency (TST-005);
// tested by TextDiffTests.cpp. The screen is DiffViewer.{h,cpp}.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace mc {

enum class WhitespaceMode { Exact, Trailing, All };

struct DiffOptions {
    bool ignoreCase = false;  // invariant Unicode case folding (TextCase.h)
    WhitespaceMode whitespace = WhitespaceMode::Exact;
};

// The text two lines are compared by: case-folded and/or with trailing or
// all whitespace removed, per `options`.
std::wstring NormalizeForDiff(const std::wstring& line, const DiffOptions& options);

enum class RowKind { Same, Removed, Added, Changed };

// One aligned row of the side-by-side view: 0-based line numbers, absent on
// the side that has no line there (Removed: left only, Added: right only).
struct DiffRow {
    RowKind kind = RowKind::Same;
    std::optional<size_t> left;
    std::optional<size_t> right;
};

enum class DiffStatus { Ok, Cancelled, TooLarge };

struct DiffResult {
    DiffStatus status = DiffStatus::Ok;
    std::vector<DiffRow> rows;
    size_t removed = 0, added = 0, changed = 0;  // row counts
    bool Identical() const { return status == DiffStatus::Ok && removed == 0 && added == 0 && changed == 0; }
};

using DiffCancelPoll = std::function<bool()>;
using DiffProgress = std::function<void(size_t editsSoFar, size_t editLimit)>;

// More edits than this in one alignment step: the files are "too different
// to align" (DiffStatus::TooLarge) -- keeps a pathological pair bounded.
constexpr size_t kMaxDiffEdits = 20000;

// Aligns two files' lines. Each run of removed + added lines between equal
// lines is paired into Changed rows (left i beside right i); the remainder
// stays Removed or Added. `cancel` is polled while computing.
DiffResult ComputeLineDiff(const std::vector<std::wstring>& left, const std::vector<std::wstring>& right,
                           const DiffOptions& options, const DiffCancelPoll& cancel = {},
                           const DiffProgress& progress = {}, size_t maxEdits = kMaxDiffEdits);

// The differing characters of a Changed pair, as (start, length) ranges in
// each original line. Lines longer than `maxLength` are reported as wholly
// different (one span each).
struct LineSpans {
    std::vector<std::pair<size_t, size_t>> left;
    std::vector<std::pair<size_t, size_t>> right;
};
LineSpans ChangedSpans(const std::wstring& left, const std::wstring& right, const DiffOptions& options,
                       size_t maxLength = 2000);

// The first row of the next / previous difference block (a run of
// non-Same rows) after / before `from`; nullopt when there is none.
std::optional<size_t> NextDifference(const std::vector<DiffRow>& rows, size_t from);
std::optional<size_t> PreviousDifference(const std::vector<DiffRow>& rows, size_t from);

// Binary files: streamed byte comparison (64 KB chunks, cancel polled).
struct ByteCompareResult {
    bool ok = false;         // both files could be read to the end (or to the first difference)
    bool cancelled = false;
    bool identical = false;
    uint64_t firstDifference = 0;  // offset, when !identical (the shorter size when one is a prefix)
    uint64_t leftSize = 0, rightSize = 0;
    std::wstring error;
};
ByteCompareResult FirstDifferenceOffset(const std::filesystem::path& left, const std::filesystem::path& right,
                                        const DiffCancelPoll& cancel = {});

// --- Which two files (SRC-008, decided 2026-10-09) -----------------------------

struct ComparePanelState {
    std::filesystem::path folder;
    std::vector<std::wstring> selectedFiles;  // selected entries that are files, in panel order
    size_t selectedFolders = 0;
    std::wstring cursorFile;                  // the cursor entry if it's a file, else empty
    std::vector<std::wstring> fileNames;      // every file name in the panel's folder
};

struct FilesToCompare {
    std::optional<std::pair<std::filesystem::path, std::filesystem::path>> pair;  // shown left | right
    std::wstring problem;  // why there's no pair
};

// 1. Two files selected in the active panel -> those (panel order).
// 2. One file selected in each panel -> those (left | right).
// 3. Otherwise the active panel's file (its one selected file, or the
//    cursor file) and the file of the same name in the other panel.
// 4. Otherwise that file and the other panel's selected or cursor file.
// The file from the left panel is always shown on the left.
FilesToCompare ChooseFilesToCompare(const ComparePanelState& left, const ComparePanelState& right, bool leftActive);

// Config::diffProgram ("C:\...\tool.exe" "%1" "%2") -> the program and its
// argument string, %1 / %2 replaced by the left / right path verbatim (the
// user quotes them, like a file action -- a Windows path can't contain '"',
// SEC-004). Without %1/%2 both paths are appended, quoted. nullopt when the
// setting has no program.
struct ExternalDiffCommand {
    std::wstring program;
    std::wstring arguments;
};
std::optional<ExternalDiffCommand> BuildExternalDiffCommand(const std::wstring& setting,
                                                            const std::filesystem::path& left,
                                                            const std::filesystem::path& right);

}  // namespace mc
