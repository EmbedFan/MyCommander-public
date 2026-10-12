#include "TextDiff.h"

#include "PathUtil.h"
#include "TextCase.h"

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <unordered_map>

namespace mc {

namespace {

// Linear-space Myers diff (the "middle snake" bisection, as in
// diff-match-patch) over integer symbols, marking which symbols of `a` are
// removed and which of `b` are added. Throws nothing; reports TooLarge /
// Cancelled through `status`.
class Differ {
public:
    Differ(const std::vector<int>& a, const std::vector<int>& b, size_t maxEdits, const DiffCancelPoll& cancel,
           const DiffProgress& progress)
        : a_(a), b_(b), maxEdits_(maxEdits), cancel_(cancel), progress_(progress),
          removed_(a.size(), false), added_(b.size(), false) {}

    DiffStatus Run() {
        Compare(0, a_.size(), 0, b_.size());
        return status_;
    }
    const std::vector<bool>& Removed() const { return removed_; }
    const std::vector<bool>& Added() const { return added_; }

private:
    const std::vector<int>& a_;
    const std::vector<int>& b_;
    size_t maxEdits_;
    const DiffCancelPoll& cancel_;
    const DiffProgress& progress_;
    std::vector<bool> removed_, added_;
    DiffStatus status_ = DiffStatus::Ok;
    size_t editsSeen_ = 0;
    unsigned pollCounter_ = 0;

    bool Stopped() {
        if (status_ != DiffStatus::Ok) return true;
        if (cancel_ && (++pollCounter_ & 63) == 0 && cancel_()) status_ = DiffStatus::Cancelled;
        return status_ != DiffStatus::Ok;
    }

    void Compare(size_t aLo, size_t aHi, size_t bLo, size_t bHi) {
        if (status_ != DiffStatus::Ok) return;
        while (aLo < aHi && bLo < bHi && a_[aLo] == b_[bLo]) {
            ++aLo;
            ++bLo;
        }
        while (aHi > aLo && bHi > bLo && a_[aHi - 1] == b_[bHi - 1]) {
            --aHi;
            --bHi;
        }
        if (aLo == aHi) {
            for (size_t j = bLo; j < bHi; ++j) added_[j] = true;
            return;
        }
        if (bLo == bHi) {
            for (size_t i = aLo; i < aHi; ++i) removed_[i] = true;
            return;
        }
        Bisect(aLo, aHi, bLo, bHi);
    }

    void Bisect(size_t aLo, size_t aHi, size_t bLo, size_t bHi) {
        const long long n = static_cast<long long>(aHi - aLo), m = static_cast<long long>(bHi - bLo);
        const long long maxD = (n + m + 1) / 2;
        // Indexes reach offset +/- (d + 1) with d < maxD, so 2 * maxD + 2 slots
        // (the reference implementation never bisects one-line inputs; this does).
        const long long offset = maxD, length = 2 * maxD + 2;
        std::vector<long long> v1(static_cast<size_t>(length), -1), v2(static_cast<size_t>(length), -1);
        v1[static_cast<size_t>(offset + 1)] = 0;
        v2[static_cast<size_t>(offset + 1)] = 0;
        const long long delta = n - m;
        const bool front = (delta % 2) != 0;
        long long k1start = 0, k1end = 0, k2start = 0, k2end = 0;
        auto A = [&](long long i) { return a_[aLo + static_cast<size_t>(i)]; };
        auto B = [&](long long j) { return b_[bLo + static_cast<size_t>(j)]; };
        for (long long d = 0; d < maxD; ++d) {
            if (static_cast<size_t>(d) > maxEdits_) {
                status_ = DiffStatus::TooLarge;
                return;
            }
            if (Stopped()) return;
            if (progress_ && static_cast<size_t>(d) > editsSeen_ && (d & 255) == 0) {
                editsSeen_ = static_cast<size_t>(d);
                progress_(editsSeen_, maxEdits_);
            }
            for (long long k1 = -d + k1start; k1 <= d - k1end; k1 += 2) {
                const long long k1o = offset + k1;
                long long x1;
                if (k1 == -d || (k1 != d && v1[static_cast<size_t>(k1o - 1)] < v1[static_cast<size_t>(k1o + 1)]))
                    x1 = v1[static_cast<size_t>(k1o + 1)];
                else
                    x1 = v1[static_cast<size_t>(k1o - 1)] + 1;
                long long y1 = x1 - k1;
                while (x1 < n && y1 < m && A(x1) == B(y1)) {
                    ++x1;
                    ++y1;
                }
                v1[static_cast<size_t>(k1o)] = x1;
                if (x1 > n) {
                    k1end += 2;
                } else if (y1 > m) {
                    k1start += 2;
                } else if (front) {
                    const long long k2o = offset + delta - k1;
                    if (k2o >= 0 && k2o < length && v2[static_cast<size_t>(k2o)] != -1) {
                        const long long x2 = n - v2[static_cast<size_t>(k2o)];
                        if (x1 >= x2) return Split(aLo, aHi, bLo, bHi, x1, y1);
                    }
                }
            }
            for (long long k2 = -d + k2start; k2 <= d - k2end; k2 += 2) {
                const long long k2o = offset + k2;
                long long x2;
                if (k2 == -d || (k2 != d && v2[static_cast<size_t>(k2o - 1)] < v2[static_cast<size_t>(k2o + 1)]))
                    x2 = v2[static_cast<size_t>(k2o + 1)];
                else
                    x2 = v2[static_cast<size_t>(k2o - 1)] + 1;
                long long y2 = x2 - k2;
                while (x2 < n && y2 < m && A(n - x2 - 1) == B(m - y2 - 1)) {
                    ++x2;
                    ++y2;
                }
                v2[static_cast<size_t>(k2o)] = x2;
                if (x2 > n) {
                    k2end += 2;
                } else if (y2 > m) {
                    k2start += 2;
                } else if (!front) {
                    const long long k1o = offset + delta - k2;
                    if (k1o >= 0 && k1o < length && v1[static_cast<size_t>(k1o)] != -1) {
                        const long long x1 = v1[static_cast<size_t>(k1o)];
                        const long long y1 = offset + x1 - k1o;
                        if (x1 >= n - x2) return Split(aLo, aHi, bLo, bHi, x1, y1);
                    }
                }
            }
        }
        // No common symbol at all: everything is replaced.
        for (size_t i = aLo; i < aHi; ++i) removed_[i] = true;
        for (size_t j = bLo; j < bHi; ++j) added_[j] = true;
    }

    void Split(size_t aLo, size_t aHi, size_t bLo, size_t bHi, long long x, long long y) {
        const size_t aMid = aLo + static_cast<size_t>(x), bMid = bLo + static_cast<size_t>(y);
        Compare(aLo, aMid, bLo, bMid);
        Compare(aMid, aHi, bMid, bHi);
    }
};

// Interns normalized lines as integers so the diff compares ints, never
// strings (and equal ids mean exactly equal normalized text).
std::pair<std::vector<int>, std::vector<int>> Intern(const std::vector<std::wstring>& left,
                                                     const std::vector<std::wstring>& right, const DiffOptions& options) {
    std::unordered_map<std::wstring, int> ids;
    auto convert = [&](const std::vector<std::wstring>& lines) {
        std::vector<int> out;
        out.reserve(lines.size());
        for (const auto& line : lines) {
            auto [it, inserted] = ids.emplace(NormalizeForDiff(line, options), static_cast<int>(ids.size()));
            out.push_back(it->second);
        }
        return out;
    };
    auto a = convert(left);
    auto b = convert(right);
    return {std::move(a), std::move(b)};
}

std::vector<std::pair<size_t, size_t>> Ranges(const std::vector<bool>& marks) {
    std::vector<std::pair<size_t, size_t>> out;
    for (size_t i = 0; i < marks.size();) {
        if (!marks[i]) {
            ++i;
            continue;
        }
        size_t start = i;
        while (i < marks.size() && marks[i]) ++i;
        out.emplace_back(start, i - start);
    }
    return out;
}

bool IsDifference(const DiffRow& row) { return row.kind != RowKind::Same; }

std::wstring Lower(const std::wstring& s) { return ToLowerInvariant(s); }

}  // namespace

std::wstring NormalizeForDiff(const std::wstring& line, const DiffOptions& options) {
    std::wstring out = options.ignoreCase ? ToLowerInvariant(line) : line;
    if (options.whitespace == WhitespaceMode::Trailing) {
        while (!out.empty() && std::iswspace(out.back())) out.pop_back();
    } else if (options.whitespace == WhitespaceMode::All) {
        out.erase(std::remove_if(out.begin(), out.end(), [](wchar_t c) { return std::iswspace(c) != 0; }), out.end());
    }
    return out;
}

DiffResult ComputeLineDiff(const std::vector<std::wstring>& left, const std::vector<std::wstring>& right,
                           const DiffOptions& options, const DiffCancelPoll& cancel, const DiffProgress& progress,
                           size_t maxEdits) {
    DiffResult result;
    auto [a, b] = Intern(left, right, options);
    Differ differ(a, b, maxEdits, cancel, progress);
    result.status = differ.Run();
    if (result.status != DiffStatus::Ok) return result;
    const auto& removed = differ.Removed();
    const auto& added = differ.Added();
    size_t i = 0, j = 0;
    while (i < a.size() || j < b.size()) {
        if ((i < a.size() && removed[i]) || (j < b.size() && added[j])) {
            std::vector<size_t> gone, come;  // one difference block
            while ((i < a.size() && removed[i]) || (j < b.size() && added[j])) {
                if (i < a.size() && removed[i]) gone.push_back(i++);
                else come.push_back(j++);
            }
            size_t paired = std::min(gone.size(), come.size());
            for (size_t p = 0; p < paired; ++p) result.rows.push_back({RowKind::Changed, gone[p], come[p]});
            for (size_t p = paired; p < gone.size(); ++p) result.rows.push_back({RowKind::Removed, gone[p], std::nullopt});
            for (size_t p = paired; p < come.size(); ++p) result.rows.push_back({RowKind::Added, std::nullopt, come[p]});
            result.changed += paired;
            result.removed += gone.size() - paired;
            result.added += come.size() - paired;
            continue;
        }
        result.rows.push_back({RowKind::Same, i++, j++});
    }
    return result;
}

LineSpans ChangedSpans(const std::wstring& left, const std::wstring& right, const DiffOptions& options,
                       size_t maxLength) {
    LineSpans spans;
    if (left.size() > maxLength || right.size() > maxLength) {
        if (!left.empty()) spans.left.emplace_back(0, left.size());
        if (!right.empty()) spans.right.emplace_back(0, right.size());
        return spans;
    }
    const std::wstring l = options.ignoreCase ? Lower(left) : left;
    const std::wstring r = options.ignoreCase ? Lower(right) : right;
    std::vector<int> a(l.begin(), l.end()), b(r.begin(), r.end());
    if (l.size() != left.size() || r.size() != right.size()) {  // case mapping changed the length: no alignment
        a.assign(left.begin(), left.end());
        b.assign(right.begin(), right.end());
    }
    DiffCancelPoll noCancel;
    DiffProgress noProgress;
    Differ differ(a, b, maxLength, noCancel, noProgress);
    differ.Run();
    std::vector<bool> removed = differ.Removed(), added = differ.Added();
    if (options.whitespace != WhitespaceMode::Exact) {  // don't highlight ignored whitespace
        for (size_t i = 0; i < removed.size(); ++i)
            if (std::iswspace(left[i])) removed[i] = false;
        for (size_t j = 0; j < added.size(); ++j)
            if (std::iswspace(right[j])) added[j] = false;
    }
    spans.left = Ranges(removed);
    spans.right = Ranges(added);
    return spans;
}

std::optional<size_t> NextDifference(const std::vector<DiffRow>& rows, size_t from) {
    size_t i = from;
    while (i < rows.size() && IsDifference(rows[i])) ++i;  // leave the current block
    while (i < rows.size() && !IsDifference(rows[i])) ++i;
    if (i < rows.size()) return i;
    return std::nullopt;
}

std::optional<size_t> PreviousDifference(const std::vector<DiffRow>& rows, size_t from) {
    if (rows.empty() || from == 0) return std::nullopt;
    size_t i = std::min(from, rows.size());
    // Step into the previous block (skipping the start of a block `from` is in).
    while (i > 0 && !IsDifference(rows[i - 1])) --i;
    if (i == 0) return std::nullopt;
    while (i > 0 && IsDifference(rows[i - 1])) --i;
    return i;
}

ByteCompareResult FirstDifferenceOffset(const std::filesystem::path& left, const std::filesystem::path& right,
                                        const DiffCancelPoll& cancel) {
    ByteCompareResult result;
    std::error_code ec;
    result.leftSize = std::filesystem::file_size(ToLongPath(left), ec);
    if (ec) {
        result.error = L"Cannot read " + left.wstring();
        return result;
    }
    result.rightSize = std::filesystem::file_size(ToLongPath(right), ec);
    if (ec) {
        result.error = L"Cannot read " + right.wstring();
        return result;
    }
    std::ifstream a(ToLongPath(left), std::ios::binary), b(ToLongPath(right), std::ios::binary);
    if (!a || !b) {
        result.error = L"Cannot open " + (!a ? left.wstring() : right.wstring());
        return result;
    }
    constexpr size_t kChunk = 64 * 1024;
    std::vector<char> bufA(kChunk), bufB(kChunk);
    uint64_t offset = 0;
    for (;;) {
        if (cancel && cancel()) {
            result.cancelled = true;
            return result;
        }
        a.read(bufA.data(), static_cast<std::streamsize>(kChunk));
        b.read(bufB.data(), static_cast<std::streamsize>(kChunk));
        const size_t gotA = static_cast<size_t>(a.gcount()), gotB = static_cast<size_t>(b.gcount());
        const size_t common = std::min(gotA, gotB);
        for (size_t i = 0; i < common; ++i) {
            if (bufA[i] != bufB[i]) {
                result.ok = true;
                result.firstDifference = offset + i;
                return result;
            }
        }
        offset += common;
        if (gotA != gotB) {  // one file ends first
            result.ok = true;
            result.firstDifference = offset;
            return result;
        }
        if (gotA == 0) break;
    }
    result.ok = true;
    result.identical = true;
    return result;
}

FilesToCompare ChooseFilesToCompare(const ComparePanelState& left, const ComparePanelState& right, bool leftActive) {
    FilesToCompare out;
    const ComparePanelState& active = leftActive ? left : right;
    const ComparePanelState& other = leftActive ? right : left;
    auto make = [&](const std::filesystem::path& l, const std::filesystem::path& r) {
        std::error_code ec;
        if (std::filesystem::equivalent(ToLongPath(l), ToLongPath(r), ec) || l == r) {
            out.problem = L"That is the same file on both sides.";
            return out;
        }
        out.pair = std::make_pair(l, r);
        return out;
    };
    // 1. Two files selected in the active panel.
    if (active.selectedFiles.size() == 2)
        return make(active.folder / active.selectedFiles[0], active.folder / active.selectedFiles[1]);
    if (active.selectedFiles.size() > 2) {
        out.problem = std::to_wstring(active.selectedFiles.size()) +
                      L" files are selected. Select two files in one panel, or one file in each panel.";
        return out;
    }
    // 2. One file selected in each panel.
    if (left.selectedFiles.size() == 1 && right.selectedFiles.size() == 1)
        return make(left.folder / left.selectedFiles[0], right.folder / right.selectedFiles[0]);
    // 3. The active file and the same name in the other panel.
    std::wstring activeFile = active.selectedFiles.size() == 1 ? active.selectedFiles[0] : active.cursorFile;
    if (activeFile.empty()) {
        out.problem = active.selectedFolders > 0 ? L"Folders can't be compared here (use Compare Folders, Shift+F2)."
                                                 : L"Put the cursor on a file, or select two files.";
        return out;
    }
    auto sideBySide = [&](const std::wstring& otherName) {
        std::filesystem::path a = active.folder / activeFile, o = other.folder / otherName;
        return leftActive ? make(a, o) : make(o, a);
    };
    for (const auto& name : other.fileNames)
        if (_wcsicmp(name.c_str(), activeFile.c_str()) == 0) return sideBySide(name);
    // 4. The other panel's selected or cursor file.
    std::wstring otherFile = other.selectedFiles.size() == 1 ? other.selectedFiles[0] : other.cursorFile;
    if (!otherFile.empty()) return sideBySide(otherFile);
    out.problem = L"The other panel has no \"" + activeFile + L"\" and no file under its cursor to compare with.";
    return out;
}

std::optional<ExternalDiffCommand> BuildExternalDiffCommand(const std::wstring& setting,
                                                            const std::filesystem::path& left,
                                                            const std::filesystem::path& right) {
    auto trim = [](std::wstring s) {
        size_t b = s.find_first_not_of(L" \t");
        if (b == std::wstring::npos) return std::wstring();
        size_t e = s.find_last_not_of(L" \t");
        return s.substr(b, e - b + 1);
    };
    std::wstring text = trim(setting);
    if (text.empty()) return std::nullopt;
    ExternalDiffCommand command;
    size_t end;
    if (text[0] == L'"') {
        end = text.find(L'"', 1);
        if (end == std::wstring::npos) return std::nullopt;
        command.program = text.substr(1, end - 1);
        ++end;
    } else {
        end = text.find_first_of(L" \t");
        if (end == std::wstring::npos) end = text.size();
        command.program = text.substr(0, end);
    }
    if (command.program.empty()) return std::nullopt;
    std::wstring arguments = trim(text.substr(end));
    bool placeholders = arguments.find(L"%1") != std::wstring::npos || arguments.find(L"%2") != std::wstring::npos;
    if (!placeholders) {
        arguments += (arguments.empty() ? L"" : L" ") + std::wstring(L"\"%1\" \"%2\"");
    }
    std::wstring out;
    for (size_t i = 0; i < arguments.size(); ++i) {
        if (arguments[i] == L'%' && i + 1 < arguments.size() && (arguments[i + 1] == L'1' || arguments[i + 1] == L'2')) {
            out += (arguments[i + 1] == L'1' ? left : right).wstring();
            ++i;
        } else {
            out += arguments[i];
        }
    }
    command.arguments = out;
    return command;
}

}  // namespace mc
