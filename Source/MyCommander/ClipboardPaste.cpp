#include "ClipboardPaste.h"

#include "TextCase.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <cstring>

namespace fs = std::filesystem;

namespace mc {

std::vector<fs::path> ParseDropFiles(const void* data, size_t size) {
    std::vector<fs::path> out;
    constexpr size_t kHeader = 20;  // DWORD pFiles + POINT (8) + BOOL fNC + BOOL fWide
    if (!data || size < kHeader) return out;
    const unsigned char* bytes = static_cast<const unsigned char*>(data);
    uint32_t offset = 0, wide = 0;
    std::memcpy(&offset, bytes, 4);
    std::memcpy(&wide, bytes + 16, 4);
    if (offset < kHeader || offset >= size) return out;

    if (wide) {
        const size_t count = (size - offset) / sizeof(wchar_t);
        const wchar_t* p = reinterpret_cast<const wchar_t*>(bytes + offset);
        size_t i = 0;
        while (i < count && p[i] != L'\0') {
            size_t j = i;
            while (j < count && p[j] != L'\0') ++j;
            if (j >= count) break;  // not terminated: ignore the truncated tail
            out.emplace_back(std::wstring(p + i, j - i));
            i = j + 1;
        }
    } else {
        const char* p = reinterpret_cast<const char*>(bytes + offset);
        const size_t count = size - offset;
        size_t i = 0;
        while (i < count && p[i] != '\0') {
            size_t j = i;
            while (j < count && p[j] != '\0') ++j;
            if (j >= count) break;
            int n = MultiByteToWideChar(CP_ACP, 0, p + i, static_cast<int>(j - i), nullptr, 0);
            std::wstring w(static_cast<size_t>(n), L'\0');
            if (n > 0) MultiByteToWideChar(CP_ACP, 0, p + i, static_cast<int>(j - i), w.data(), n);
            out.emplace_back(std::move(w));
            i = j + 1;
        }
    }
    return out;
}

namespace {

// Lexical, case-insensitive key: the paths are compared as text so the plan
// stays pure and never touches the disk (an offline share cannot block it).
std::wstring Key(const fs::path& p) {
    std::wstring s = p.lexically_normal().wstring();
    for (wchar_t& c : s) {
        if (c == L'/') c = L'\\';
        c = ToLowerInvariant(std::wstring(1, c))[0];
    }
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

bool SameOrWithin(const std::wstring& base, const std::wstring& candidate) {
    if (candidate == base) return true;
    std::wstring prefix = base;
    if (prefix.empty() || prefix.back() != L'\\') prefix += L'\\';
    return candidate.size() > prefix.size() && candidate.compare(0, prefix.size(), prefix) == 0;
}

}  // namespace

PastePlan PlanPaste(const std::vector<fs::path>& sources, const fs::path& destination, bool isCut) {
    PastePlan plan;
    plan.isMove = isCut;
    for (const fs::path& src : sources) {
        if (src.empty() || !src.has_filename()) {
            plan.dropped.push_back({src, L"not a file or folder path"});
            continue;
        }
        fs::path parent = src.parent_path();
        const std::wstring parentKey = Key(parent);
        if (parentKey == Key(destination)) {
            plan.dropped.push_back({src, L"already in this folder"});
            continue;
        }
        if (SameOrWithin(Key(src), Key(destination))) {
            plan.dropped.push_back({src, L"a folder cannot be pasted into itself"});
            continue;
        }
        PasteGroup* group = nullptr;
        for (PasteGroup& g : plan.groups) {
            if (Key(g.sourceDir) == parentKey) {
                group = &g;
                break;
            }
        }
        if (!group) {
            plan.groups.push_back({parent, {}});
            group = &plan.groups.back();
        }
        group->names.push_back(src.filename().wstring());
    }
    return plan;
}

}  // namespace mc
