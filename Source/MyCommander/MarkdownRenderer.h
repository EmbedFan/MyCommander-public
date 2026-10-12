#pragma once

// FEA-0002 (MC Plugin), Phase 2, §7: a deliberately minimal Markdown parser
// for rendering a plugin's own help content inside Help.cpp's F1
// shortcut-reference screen. Not a general CommonMark implementation --
// recognizes only headings ("#"/"##"/... followed by a space, level not
// tracked), bullet list items ("-"/"*" followed by a space), and plain
// paragraphs, with inline code spans (single backticks) stripped rather
// than specially colored -- coloring a sub-line run would need Help.cpp's
// one-attribute-per-row paint loop to support multiple attribute runs per
// row, a much larger change than this minimal renderer is meant to
// justify. Anything more exotic in the source (tables, links, nested
// lists, fenced code blocks, emphasis) renders as a plain paragraph line --
// a documented, acceptable degradation. Pure, no Console/Panel/Dialog
// dependency, so directly unit-testable (TST-005 precedent).

#include <string>
#include <vector>

namespace mc {

enum class MarkdownLineKind {
    Blank,
    Heading,
    Bullet,
    Paragraph,
};

struct MarkdownLine {
    MarkdownLineKind kind = MarkdownLineKind::Blank;
    std::wstring text;  // already stripped of the leading "#.. "/"- "/"* " marker and of inline
                        // code backticks; empty for a Blank line
};

// Splits `source` on '\n' (a trailing '\r' on each line is trimmed, so
// either LF or CRLF source text parses identically) and classifies each
// line independently -- there is no multi-line state (no fenced code
// blocks, no paragraph joining), matching the deliberately minimal scope
// above. An empty `source` returns an empty vector.
std::vector<MarkdownLine> ParseMinimalMarkdown(const std::wstring& source);

}  // namespace mc
