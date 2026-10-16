#include "Labels.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "TextCase.h"
#include "Utf8TextFile.h"

#include <algorithm>

namespace mc {

std::wstring LabelPathKey(const std::filesystem::path& path) {
    std::wstring s = path.lexically_normal().wstring();
    std::replace(s.begin(), s.end(), L'/', L'\\');
    // "C:\" keeps its separator (size 3); every other trailing one goes.
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return ToLowerInvariant(s);
}

namespace {

// `key` followed by a separator: the prefix every path strictly beneath it has.
std::wstring BeneathPrefix(const std::wstring& key) {
    if (!key.empty() && key.back() == L'\\') return key;  // a drive root already ends with one
    return key + L"\\";
}

bool IsBeneath(const std::wstring& key, const std::wstring& prefix) {
    return key.size() > prefix.size() && key.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

int LabelStore::Get(const std::filesystem::path& path) const {
    auto it = entries_.find(LabelPathKey(path));
    return it == entries_.end() ? 0 : it->second.label;
}

bool LabelStore::Set(const std::filesystem::path& path, int label) {
    if (label < 0 || label > kLabelCount || path.empty()) return false;
    const std::wstring key = LabelPathKey(path);
    auto it = entries_.find(key);
    if (label == 0) {
        if (it != entries_.end()) entries_.erase(it);
        return true;
    }
    if (it != entries_.end()) {
        it->second = LabelEntry{path, label};
        return true;
    }
    if (entries_.size() >= cap_) return false;
    entries_.emplace(key, LabelEntry{path, label});
    return true;
}

bool LabelStore::HasAnyUnder(const std::filesystem::path& path) const {
    if (entries_.empty()) return false;
    const std::wstring key = LabelPathKey(path);
    if (entries_.count(key) != 0) return true;
    const std::wstring prefix = BeneathPrefix(key);
    auto it = entries_.lower_bound(prefix);
    return it != entries_.end() && IsBeneath(it->first, prefix);
}

size_t LabelStore::Move(const std::filesystem::path& from, const std::filesystem::path& to) {
    if (entries_.empty()) return 0;
    const std::wstring fromKey = LabelPathKey(from);
    const std::wstring prefix = BeneathPrefix(fromKey);
    const std::wstring toBase = to.lexically_normal().wstring();

    std::vector<LabelEntry> moved;
    auto self = entries_.find(fromKey);
    if (self != entries_.end()) {
        moved.push_back(LabelEntry{to, self->second.label});
        entries_.erase(self);
    }
    for (auto it = entries_.lower_bound(prefix); it != entries_.end() && IsBeneath(it->first, prefix);) {
        // The part below `from` is the same in the stored path and its key
        // (lower-casing is one character to one character for paths), so the
        // remainder is cut from the stored spelling.
        std::wstring stored = it->second.path.lexically_normal().wstring();
        std::replace(stored.begin(), stored.end(), L'/', L'\\');
        std::wstring rest = stored.size() > prefix.size() ? stored.substr(prefix.size()) : std::wstring();
        moved.push_back(LabelEntry{std::filesystem::path(toBase) / rest, it->second.label});
        it = entries_.erase(it);
    }
    for (const auto& m : moved) {
        entries_[LabelPathKey(m.path)] = m;  // an existing label at the destination is replaced
    }
    return moved.size();
}

size_t LabelStore::RemoveUnder(const std::filesystem::path& path) {
    if (entries_.empty()) return 0;
    const std::wstring key = LabelPathKey(path);
    const std::wstring prefix = BeneathPrefix(key);
    size_t removed = entries_.erase(key);
    for (auto it = entries_.lower_bound(prefix); it != entries_.end() && IsBeneath(it->first, prefix);) {
        it = entries_.erase(it);
        ++removed;
    }
    return removed;
}

std::vector<LabelEntry> LabelStore::Entries() const {
    std::vector<LabelEntry> out;
    out.reserve(entries_.size());
    for (const auto& kv : entries_) out.push_back(kv.second);
    return out;
}

void LabelStore::Replace(const std::vector<LabelEntry>& entries) {
    entries_.clear();
    for (const auto& e : entries) {
        if (e.path.empty() || e.label < 1 || e.label > kLabelCount) continue;
        const std::wstring key = LabelPathKey(e.path);
        if (entries_.count(key) == 0 && entries_.size() >= cap_) continue;
        entries_[key] = e;  // the last one wins
    }
}

ParseLabelsResult ParseLabelsFile(const std::wstring& text) {
    ParseLabelsResult result;
    int lineNumber = 0;
    size_t start = 0;
    auto trim = [](std::wstring s) {
        size_t a = s.find_first_not_of(L" \t\r");
        if (a == std::wstring::npos) return std::wstring();
        size_t b = s.find_last_not_of(L" \t\r");
        return s.substr(a, b - a + 1);
    };
    while (start <= text.size()) {
        size_t end = text.find(L'\n', start);
        std::wstring line = trim(text.substr(start, end == std::wstring::npos ? std::wstring::npos : end - start));
        ++lineNumber;
        if (!line.empty() && line[0] != L'#') {
            size_t eq = line.find(L'=');
            std::wstring number = eq == std::wstring::npos ? L"" : trim(line.substr(0, eq));
            std::wstring path = eq == std::wstring::npos ? L"" : trim(line.substr(eq + 1));
            int label = 0;
            if (number.size() == 1 && number[0] >= L'1' && number[0] <= L'0' + kLabelCount) label = number[0] - L'0';
            if (label == 0 || path.empty()) {
                result.warnings.push_back(L"Line " + std::to_wstring(lineNumber) + L": expected \"1-" +
                                          std::to_wstring(kLabelCount) + L" = path\"");
            } else {
                result.entries.push_back(LabelEntry{path, label});
            }
        }
        if (end == std::wstring::npos) break;
        start = end + 1;
    }
    return result;
}

std::wstring FormatLabelsFile(const std::vector<LabelEntry>& entries) {
    std::wstring out =
        L"# MyCommander colour labels (LIST-009): label number 1-6 = full path.\n"
        L"# Rewritten whenever a label changes; safe to edit by hand while the app is closed.\n";
    for (const auto& e : entries) out += std::to_wstring(e.label) + L" = " + e.path.wstring() + L"\n";
    return out;
}

std::filesystem::path LabelsFilePath() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetEnvironmentVariableW(L"APPDATA", buf, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return {};
    return std::filesystem::path(buf) / L"MyCommander" / L"labels.conf";
}

ParseLabelsResult LoadLabelsFrom(const std::filesystem::path& path) {
    if (path.empty()) return {};
    return ParseLabelsFile(ReadUtf8TextFile(path));
}

bool SaveLabelsTo(const std::filesystem::path& path, const std::vector<LabelEntry>& entries) {
    if (path.empty()) return false;
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    return WriteUtf8TextFileAtomic(path, FormatLabelsFile(entries));
}

}  // namespace mc
