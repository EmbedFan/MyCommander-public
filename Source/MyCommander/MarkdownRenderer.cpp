#include "MarkdownRenderer.h"

#include <algorithm>
#include <iterator>

namespace mc {

namespace {

std::wstring TrimTrailingCR(std::wstring line) {
    if (!line.empty() && line.back() == L'\r') line.pop_back();
    return line;
}

// Removes every backtick from `text` -- a naive but deliberately minimal
// stand-in for inline-code-span recognition (see MarkdownRenderer.h's own
// doc comment for why sub-line coloring isn't attempted). A stray,
// unpaired backtick is also stripped rather than left as a literal
// character; an acceptable minimal-renderer limitation, since this parser
// has no per-line pairing state to distinguish "opening" from "closing".
std::wstring StripInlineCodeMarkers(const std::wstring& text) {
    std::wstring result;
    result.reserve(text.size());
    std::copy_if(text.begin(), text.end(), std::back_inserter(result), [](wchar_t c) { return c != L'`'; });
    return result;
}

MarkdownLine ClassifyLine(const std::wstring& rawLine) {
    size_t firstNonSpace = rawLine.find_first_not_of(L" \t");
    if (firstNonSpace == std::wstring::npos) {
        return MarkdownLine{MarkdownLineKind::Blank, L""};
    }

    if (rawLine[firstNonSpace] == L'#') {
        size_t hashEnd = rawLine.find_first_not_of(L'#', firstNonSpace);
        if (hashEnd != std::wstring::npos && rawLine[hashEnd] == L' ') {
            return MarkdownLine{MarkdownLineKind::Heading, StripInlineCodeMarkers(rawLine.substr(hashEnd + 1))};
        }
        // A run of '#' with no following space (or a line of only '#') isn't a
        // valid heading marker -- falls through to a plain paragraph.
    } else if ((rawLine[firstNonSpace] == L'-' || rawLine[firstNonSpace] == L'*') &&
              firstNonSpace + 1 < rawLine.size() && rawLine[firstNonSpace + 1] == L' ') {
        return MarkdownLine{MarkdownLineKind::Bullet, StripInlineCodeMarkers(rawLine.substr(firstNonSpace + 2))};
    }

    return MarkdownLine{MarkdownLineKind::Paragraph, StripInlineCodeMarkers(rawLine)};
}

}  // namespace

std::vector<MarkdownLine> ParseMinimalMarkdown(const std::wstring& source) {
    std::vector<MarkdownLine> result;
    if (source.empty()) return result;

    size_t pos = 0;
    while (true) {
        size_t next = source.find(L'\n', pos);
        std::wstring rawLine = TrimTrailingCR(next == std::wstring::npos ? source.substr(pos)
                                                                          : source.substr(pos, next - pos));
        result.push_back(ClassifyLine(rawLine));
        if (next == std::wstring::npos) break;
        pos = next + 1;
    }
    return result;
}

}  // namespace mc
