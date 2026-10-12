#pragma once

// FOP-017: group rename (Ctrl+M / File > Group Rename) -- pure logic, no
// Console/Dialog/filesystem access (CLAUDE.md's pure-module precedent):
// the Total Commander-style mask language, search & replace, case
// conversion, new-name validation, the preview plan with its problem
// statuses, and the execution order that keeps swaps and chains from ever
// colliding. FileOps::RenameItems then runs the ordered steps.
//
// Mask tokens (name mask builds the part before the last dot, extension
// mask the part after it; an empty extension result means no dot):
//   [N] name without extension     [N2-5] chars 2..5 (1-based)   [N3] char 3   [N3-] char 3 to the end
//   [E] extension (same ranges)    [C] counter                   [P] parent folder name
//   [YMD] last-write date YYYYMMDD [hms] last-write time HHMMSS  [[ / ]] literal brackets

#include "FileOps.h"

#include <cstdint>
#include <string>
#include <vector>

namespace mc {

enum class RenameCase { Unchanged, Lower, Upper, FirstLetter, EachWord };

struct MultiRenameOptions {
    std::wstring nameMask = L"[N]";
    std::wstring extMask = L"[E]";
    std::wstring search;   // plain text, case-insensitive; empty = no replacement
    std::wstring replace;
    RenameCase caseMode = RenameCase::Unchanged;
    int64_t counterStart = 1;
    int64_t counterStep = 1;
    int counterDigits = 1;
};

// The local date/time fields [YMD]/[hms] read. Passed in already converted,
// so mask expansion stays deterministic (and testable) regardless of time zone.
struct RenameDateTime {
    int year = 0, month = 0, day = 0, hour = 0, minute = 0, second = 0;
};

// Converts a raw FILETIME (UTC, 100ns ticks) to local time fields; all zero for 0.
RenameDateTime LocalDateTimeFromFileTime(uint64_t fileTimeUtc);

struct RenameSource {
    std::wstring name;
    RenameDateTime lastWrite;
};

// Everything one item's masks can refer to.
struct MaskContext {
    std::wstring baseName;   // name without extension
    std::wstring extension;  // without the dot
    std::wstring parentName;
    std::wstring counter;    // already formatted
    RenameDateTime lastWrite;
};

// "Holiday.Photo.JPG" -> {"Holiday.Photo", "JPG"}; "README" -> {"README", ""};
// ".gitignore" -> {".gitignore", ""} (a leading dot isn't an extension).
void SplitNameAndExtension(const std::wstring& name, std::wstring& base, std::wstring& extension);

// The counter text for item `index`: start + index*step, zero-padded to
// `digits` (never truncated; a negative value keeps its '-').
std::wstring FormatRenameCounter(const MultiRenameOptions& options, size_t index);

// Expands `mask` for `context`. False, with a message naming the token and
// its position, for an unknown token, a reversed range, or an unclosed '['.
bool ExpandMask(const std::wstring& mask, const MaskContext& context, std::wstring& out, std::wstring& error);

// Every occurrence of `search`, compared case-insensitively, replaced with
// `replace`. An empty `search` returns `text` unchanged.
std::wstring ApplySearchReplace(const std::wstring& text, const std::wstring& search, const std::wstring& replace);

// Unicode-aware (LCMapStringEx, invariant locale) case conversion. EachWord
// capitalizes the first letter and every letter after a space, '_', '-' or '.'.
std::wstring ApplyRenameCase(const std::wstring& text, RenameCase mode);

// Empty if `name` is a valid new file/folder name, else the reason: empty,
// "." / "..", a forbidden character (\ / : * ? " < > | or a control
// character), a trailing dot or space (Windows silently strips them), a
// reserved device name (CON, PRN, AUX, NUL, COM1-9, LPT1-9 -- with or
// without an extension), or longer than 255 characters.
std::wstring ValidateNewFileName(const std::wstring& name);

enum class RenameStatus { Ok, Unchanged, CaseOnly, Invalid, DuplicateTarget, ClashesWithExisting };

struct RenamePlanItem {
    std::wstring oldName;
    std::wstring newName;
    RenameStatus status = RenameStatus::Ok;
    std::wstring message;  // the reason, for Invalid / DuplicateTarget / ClashesWithExisting
};

struct RenamePlan {
    std::vector<RenamePlanItem> items;
    std::wstring maskError;  // non-empty: a mask can't be expanded at all
    bool HasProblems() const;
    size_t RenameCount() const;  // Ok + CaseOnly
};

// The preview: each source's new name and status. `existingNames` is every
// entry currently in the folder (sources included); a new name only clashes
// with one that isn't itself part of this batch (a swap is not a clash).
RenamePlan BuildRenamePlan(const MultiRenameOptions& options, const std::vector<RenameSource>& sources,
                           const std::wstring& parentName, const std::vector<std::wstring>& existingNames);

// The steps that perform `plan` (which must have no problems) without any
// step ever targeting a name another batch item still holds: dependency
// order for chains (b->c before a->b), and a temporary name
// "~mcren.<n>.tmp" (unused in `existingNames`) to break each cycle.
// Independent renames keep their selection order.
std::vector<RenameStep> OrderRenames(const RenamePlan& plan, const std::vector<std::wstring>& existingNames);

}  // namespace mc
