#include "MultiRename.h"

#include "TextCase.h"

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <set>

namespace mc {

namespace {

std::wstring Lower(const std::wstring& s) { return ToLowerInvariant(s); }  // TextCase.h
std::wstring Upper(const std::wstring& s) { return ToUpperInvariant(s); }

bool EqualsNoCase(const std::wstring& a, const std::wstring& b) { return Lower(a) == Lower(b); }

std::wstring Pad2(int v) {
    std::wstring s = std::to_wstring(v);
    return s.size() < 2 ? L"0" + s : s;
}

// Parses "", "3", "3-", "2-5" after an N/E letter. False on bad syntax or a reversed range.
bool ParseRange(const std::wstring& spec, size_t& first, size_t& last, bool& toEnd, bool& whole) {
    whole = spec.empty();
    toEnd = false;
    if (whole) return true;
    size_t dash = spec.find(L'-');
    auto parseNumber = [](const std::wstring& s, size_t& v) {
        if (s.empty() || s.size() > 6 || !std::all_of(s.begin(), s.end(), iswdigit)) return false;
        v = static_cast<size_t>(std::stoul(s));
        return v >= 1;
    };
    if (dash == std::wstring::npos) {
        if (!parseNumber(spec, first)) return false;
        last = first;
        return true;
    }
    if (!parseNumber(spec.substr(0, dash), first)) return false;
    std::wstring rest = spec.substr(dash + 1);
    if (rest.empty()) {
        toEnd = true;
        return true;
    }
    return parseNumber(rest, last) && last >= first;
}

std::wstring Slice(const std::wstring& s, size_t first, size_t last, bool toEnd) {
    if (first > s.size()) return L"";
    size_t start = first - 1;
    size_t end = toEnd ? s.size() : (std::min)(last, s.size());  // parenthesized: FileOps.h's windows.h may define min
    return start < end ? s.substr(start, end - start) : L"";
}

const wchar_t* const kReservedNames[] = {L"CON",  L"PRN",  L"AUX",  L"NUL",  L"COM1", L"COM2", L"COM3",
                                         L"COM4", L"COM5", L"COM6", L"COM7", L"COM8", L"COM9", L"LPT1",
                                         L"LPT2", L"LPT3", L"LPT4", L"LPT5", L"LPT6", L"LPT7", L"LPT8",
                                         L"LPT9"};

}  // namespace

RenameDateTime LocalDateTimeFromFileTime(uint64_t fileTimeUtc) {
    RenameDateTime dt;
    if (fileTimeUtc == 0) return dt;
    FILETIME utc{static_cast<DWORD>(fileTimeUtc & 0xFFFFFFFFu), static_cast<DWORD>(fileTimeUtc >> 32)};
    FILETIME local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return dt;
    dt.year = st.wYear;
    dt.month = st.wMonth;
    dt.day = st.wDay;
    dt.hour = st.wHour;
    dt.minute = st.wMinute;
    dt.second = st.wSecond;
    return dt;
}

void SplitNameAndExtension(const std::wstring& name, std::wstring& base, std::wstring& extension) {
    size_t dot = name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) {
        base = name;
        extension.clear();
        return;
    }
    base = name.substr(0, dot);
    extension = name.substr(dot + 1);
}

std::wstring FormatRenameCounter(const MultiRenameOptions& options, size_t index) {
    int64_t value = options.counterStart + static_cast<int64_t>(index) * options.counterStep;
    bool negative = value < 0;
    std::wstring digits = std::to_wstring(negative ? -value : value);
    if (static_cast<int>(digits.size()) < options.counterDigits)
        digits.insert(0, static_cast<size_t>(options.counterDigits) - digits.size(), L'0');
    return negative ? L"-" + digits : digits;
}

bool ExpandMask(const std::wstring& mask, const MaskContext& context, std::wstring& out, std::wstring& error) {
    out.clear();
    for (size_t i = 0; i < mask.size(); ++i) {
        wchar_t c = mask[i];
        if (c == L']') {
            if (i + 1 < mask.size() && mask[i + 1] == L']') ++i;  // "]]" -> ']'
            out += L']';
            continue;
        }
        if (c != L'[') {
            out += c;
            continue;
        }
        if (i + 1 < mask.size() && mask[i + 1] == L'[') {  // "[[" -> '['
            out += L'[';
            ++i;
            continue;
        }
        size_t close = mask.find(L']', i + 1);
        if (close == std::wstring::npos) {
            error = L"Unclosed '[' at position " + std::to_wstring(i + 1) + L" in \"" + mask + L"\"";
            return false;
        }
        std::wstring token = mask.substr(i + 1, close - i - 1);
        std::wstring bad = L"Unknown token [" + token + L"] at position " + std::to_wstring(i + 1) + L" in \"" +
                           mask + L"\"";
        if (token == L"C") {
            out += context.counter;
        } else if (token == L"P") {
            out += context.parentName;
        } else if (token == L"YMD") {
            out += std::to_wstring(context.lastWrite.year) + Pad2(context.lastWrite.month) + Pad2(context.lastWrite.day);
        } else if (token == L"hms") {
            out += Pad2(context.lastWrite.hour) + Pad2(context.lastWrite.minute) + Pad2(context.lastWrite.second);
        } else if (!token.empty() && (token[0] == L'N' || token[0] == L'E')) {
            size_t first = 0, last = 0;
            bool toEnd = false, whole = false;
            if (!ParseRange(token.substr(1), first, last, toEnd, whole)) {
                error = L"Invalid range [" + token + L"] at position " + std::to_wstring(i + 1) + L" in \"" + mask +
                        L"\"";
                return false;
            }
            const std::wstring& source = token[0] == L'N' ? context.baseName : context.extension;
            out += whole ? source : Slice(source, first, last, toEnd);
        } else {
            error = bad;
            return false;
        }
        i = close;
    }
    return true;
}

std::wstring ApplySearchReplace(const std::wstring& text, const std::wstring& search, const std::wstring& replace) {
    if (search.empty()) return text;
    std::wstring lowerText = Lower(text);
    std::wstring lowerSearch = Lower(search);
    // Lower-casing is 1:1 per UTF-16 unit here; if not, fall back to exact matching.
    const std::wstring& hay = lowerText.size() == text.size() ? lowerText : text;
    const std::wstring& needle = lowerText.size() == text.size() ? lowerSearch : search;
    std::wstring out;
    size_t pos = 0;
    while (true) {
        size_t found = hay.find(needle, pos);
        if (found == std::wstring::npos) break;
        out += text.substr(pos, found - pos) + replace;
        pos = found + needle.size();
    }
    return out + text.substr(pos);
}

std::wstring ApplyRenameCase(const std::wstring& text, RenameCase mode) {
    switch (mode) {
        case RenameCase::Unchanged: return text;
        case RenameCase::Lower: return Lower(text);
        case RenameCase::Upper: return Upper(text);
        case RenameCase::FirstLetter: {
            std::wstring s = Lower(text);
            if (!s.empty()) s = Upper(s.substr(0, 1)) + s.substr(1);
            return s;
        }
        case RenameCase::EachWord: {
            std::wstring s = Lower(text);
            for (size_t i = 0; i < s.size(); ++i) {
                bool start = i == 0 || s[i - 1] == L' ' || s[i - 1] == L'_' || s[i - 1] == L'-' || s[i - 1] == L'.';
                if (start) s.replace(i, 1, Upper(s.substr(i, 1)));
            }
            return s;
        }
    }
    return text;
}

std::wstring ValidateNewFileName(const std::wstring& name) {
    if (name.empty()) return L"the new name is empty";
    if (name == L"." || name == L"..") return L"\".\" and \"..\" aren't valid names";
    if (name.size() > 255) return L"the new name is longer than 255 characters";
    for (wchar_t c : name) {
        if (c < 0x20 || std::wstring(L"\\/:*?\"<>|").find(c) != std::wstring::npos)
            return L"the new name contains a character Windows doesn't allow (\\ / : * ? \" < > |)";
    }
    if (name.back() == L'.' || name.back() == L' ') return L"the new name ends with a dot or a space";
    std::wstring stem = name.substr(0, name.find(L'.'));
    while (!stem.empty() && stem.back() == L' ') stem.pop_back();
    if (std::any_of(std::begin(kReservedNames), std::end(kReservedNames),
                    [&](const wchar_t* reserved) { return EqualsNoCase(stem, reserved); }))
        return L"\"" + stem + L"\" is a name reserved by Windows";
    return L"";
}

bool RenamePlan::HasProblems() const {
    if (!maskError.empty()) return true;
    return std::any_of(items.begin(), items.end(), [](const RenamePlanItem& i) {
        return i.status == RenameStatus::Invalid || i.status == RenameStatus::DuplicateTarget ||
               i.status == RenameStatus::ClashesWithExisting;
    });
}

size_t RenamePlan::RenameCount() const {
    return static_cast<size_t>(std::count_if(items.begin(), items.end(), [](const RenamePlanItem& i) {
        return i.status == RenameStatus::Ok || i.status == RenameStatus::CaseOnly;
    }));
}

RenamePlan BuildRenamePlan(const MultiRenameOptions& options, const std::vector<RenameSource>& sources,
                           const std::wstring& parentName, const std::vector<std::wstring>& existingNames) {
    RenamePlan plan;
    for (size_t i = 0; i < sources.size(); ++i) {
        RenamePlanItem item;
        item.oldName = sources[i].name;
        MaskContext ctx;
        SplitNameAndExtension(sources[i].name, ctx.baseName, ctx.extension);
        ctx.parentName = parentName;
        ctx.counter = FormatRenameCounter(options, i);
        ctx.lastWrite = sources[i].lastWrite;
        std::wstring base, ext, error;
        if (!ExpandMask(options.nameMask, ctx, base, error) || !ExpandMask(options.extMask, ctx, ext, error)) {
            plan.maskError = error;
            plan.items.clear();
            for (const auto& s : sources) plan.items.push_back({s.name, L"", RenameStatus::Invalid, error});
            return plan;
        }
        std::wstring full = ext.empty() ? base : base + L"." + ext;
        full = ApplyRenameCase(ApplySearchReplace(full, options.search, options.replace), options.caseMode);
        item.newName = full;
        if (full == item.oldName) {
            item.status = RenameStatus::Unchanged;
        } else if (std::wstring why = ValidateNewFileName(full); !why.empty()) {
            item.status = RenameStatus::Invalid;
            item.message = why;
        } else {
            item.status = EqualsNoCase(full, item.oldName) ? RenameStatus::CaseOnly : RenameStatus::Ok;
        }
        plan.items.push_back(std::move(item));
    }

    // Two batch items ending up with the same name (case-insensitive).
    for (size_t i = 0; i < plan.items.size(); ++i) {
        RenamePlanItem& a = plan.items[i];
        if (a.status == RenameStatus::Unchanged || a.status == RenameStatus::Invalid) continue;
        for (size_t j = 0; j < plan.items.size(); ++j) {
            if (j == i || plan.items[j].status == RenameStatus::Invalid) continue;
            if (EqualsNoCase(a.newName, plan.items[j].newName)) {
                a.status = RenameStatus::DuplicateTarget;
                a.message = L"\"" + plan.items[j].oldName + L"\" would get the same name";
                break;
            }
        }
    }

    // A new name taken by an entry that isn't part of this batch.
    std::set<std::wstring> batchOld;
    for (const auto& s : sources) batchOld.insert(Lower(s.name));
    std::set<std::wstring> existing;
    for (const auto& n : existingNames) existing.insert(Lower(n));
    for (auto& item : plan.items) {
        if (item.status != RenameStatus::Ok) continue;
        std::wstring key = Lower(item.newName);
        if (existing.count(key) && !batchOld.count(key)) {
            item.status = RenameStatus::ClashesWithExisting;
            item.message = L"\"" + item.newName + L"\" already exists here";
        }
    }
    return plan;
}

std::vector<RenameStep> OrderRenames(const RenamePlan& plan, const std::vector<std::wstring>& existingNames) {
    struct Pending {
        std::wstring current;
        std::wstring target;
        std::wstring original;
        bool atTemp = false;
    };
    std::vector<Pending> pending;
    std::set<std::wstring> taken;  // lower-cased names that exist or are used as temporaries
    for (const auto& n : existingNames) taken.insert(Lower(n));
    for (const auto& item : plan.items) {
        taken.insert(Lower(item.newName));
        if (item.status == RenameStatus::Ok || item.status == RenameStatus::CaseOnly)
            pending.push_back({item.oldName, item.newName, item.oldName, false});
    }

    std::vector<RenameStep> steps;
    int tempCounter = 1;
    while (!pending.empty()) {
        bool progress = false;
        for (size_t i = 0; i < pending.size();) {
            Pending& p = pending[i];
            std::wstring targetKey = Lower(p.target);
            bool blocked = false;
            if (targetKey != Lower(p.current)) {  // a case-only change is never blocked
                for (size_t j = 0; j < pending.size(); ++j)
                    if (j != i && Lower(pending[j].current) == targetKey) blocked = true;
            }
            if (blocked) {
                ++i;
                continue;
            }
            steps.push_back({p.current, p.target, p.original, false, p.atTemp});
            pending.erase(pending.begin() + static_cast<std::ptrdiff_t>(i));
            progress = true;
        }
        if (!progress) {
            // Every remaining item waits on another: a cycle. Park the first one under a temporary name.
            Pending& p = pending.front();
            std::wstring temp;
            do {
                temp = L"~mcren." + std::to_wstring(tempCounter++) + L".tmp";
            } while (taken.count(Lower(temp)));
            taken.insert(Lower(temp));
            steps.push_back({p.current, temp, p.original, true, false});
            p.current = temp;
            p.atTemp = true;
        }
    }
    return steps;
}

}  // namespace mc
