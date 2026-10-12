#include "DiffViewer.h"

#include "Strings.h"
#include "TextWidth.h"
#include "Theme.h"
#include "Viewer.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <optional>

namespace mc {

namespace {

constexpr int kTabWidth = 4;
constexpr int kUnifiedBelowWidth = 100;

// One screen line. Side by side: the whole DiffRow. Unified: one side of it
// (a Changed row becomes its '-' line then its '+' line).
struct DisplayLine {
    size_t row = 0;
    int side = 0;  // 0 = both (side by side, or a Same line in unified), -1 = left only, +1 = right only
};

std::wstring WhitespaceLabel(WhitespaceMode mode) {
    switch (mode) {
        case WhitespaceMode::Trailing: return kStrDiffWhitespaceTrailing;
        case WhitespaceMode::All: return kStrDiffWhitespaceAll;
        default: return kStrDiffWhitespaceExact;
    }
}

WORD KindAttr(RowKind kind) {
    switch (kind) {
        case RowKind::Removed: return kAttrDiffRemoved;
        case RowKind::Added: return kAttrDiffAdded;
        case RowKind::Changed: return kAttrDiffChanged;
        default: return kAttrNormal;
    }
}

wchar_t KindMarker(RowKind kind) {
    switch (kind) {
        case RowKind::Removed: return L'<';
        case RowKind::Added: return L'>';
        case RowKind::Changed: return L'~';
        default: return L' ';
    }
}

// Draws `text` from display column `offset` into `width` cells at (x, y):
// tabs expanded, full-width characters measured (UI-011), the characters in
// `highlight` drawn with `highlightAttr`. The rest of the field is blanked.
void DrawCells(Console& console, SHORT x, SHORT y, int width, const std::wstring& text,
               const std::vector<std::pair<size_t, size_t>>& highlight, WORD attr, WORD highlightAttr, int offset) {
    if (width <= 0) return;
    std::vector<bool> marked(text.size(), false);
    for (const auto& [start, length] : highlight)
        for (size_t i = start; i < start + length && i < marked.size(); ++i) marked[i] = true;
    console.FillRow(y, x, static_cast<SHORT>(width), L' ', attr);
    int column = 0, used = 0;
    auto put = [&](wchar_t c, int w, WORD a) {
        if (column >= offset) {
            if (used + w > width) return false;
            console.PutText(static_cast<SHORT>(x + used), y, std::wstring(1, c), a);
            used += w;
        } else if (column + w > offset) {  // a wide character straddling the scroll edge
            console.PutText(static_cast<SHORT>(x + used), y, L" ", a);
            used += column + w - offset;
        }
        column += w;
        return true;
    };
    for (size_t i = 0; i < text.size(); ++i) {
        WORD a = marked[i] ? highlightAttr : attr;
        if (text[i] == L'\t') {
            do {
                if (!put(L' ', 1, a)) return;
            } while (column % kTabWidth != 0);
        } else if (!put(text[i], CharDisplayWidth(text[i]), a)) {
            return;
        }
    }
}

std::wstring Number(std::optional<size_t> line, int width) {
    std::wstring n = line ? std::to_wstring(*line + 1) : L"";
    if (static_cast<int>(n.size()) < width) n.insert(0, static_cast<size_t>(width) - n.size(), L' ');
    return n;
}

// The path cut from the left to `width` columns ("...\dir\file.txt").
std::wstring FitPath(const std::wstring& path, int width) {
    if (width <= 3) return L"";
    if (StringDisplayWidth(path) <= width) return path;
    std::wstring tail = path;
    while (!tail.empty() && StringDisplayWidth(tail) + 3 > width) tail.erase(0, 1);
    return L"..." + tail;
}

}  // namespace

void ShowTextDiffViewer(Console& console, const DiffDocument& document, DiffOptions options, DiffResult result,
                        const DiffRecompute& recompute) {
    bool unified = console.Width() < kUnifiedBelowWidth;
    std::vector<DisplayLine> lines;
    std::vector<size_t> firstLineOfRow;
    std::map<size_t, LineSpans> spanCache;
    std::wstring note;  // a one-off message in the status line (e.g. "too different to align")
    int top = 0, offset = 0;
    std::optional<size_t> current;  // the difference F7 / Shift+F7 last went to (cleared by scrolling)

    auto rebuild = [&]() {
        lines.clear();
        firstLineOfRow.assign(result.rows.size(), 0);
        for (size_t r = 0; r < result.rows.size(); ++r) {
            firstLineOfRow[r] = lines.size();
            const DiffRow& row = result.rows[r];
            if (!unified || row.kind == RowKind::Same) {
                lines.push_back({r, 0});
            } else {
                if (row.left) lines.push_back({r, -1});
                if (row.right) lines.push_back({r, +1});
            }
        }
        spanCache.clear();
    };
    auto rows = [&]() { return std::max(1, console.Height() - 2); };
    auto clampTop = [&]() { top = std::clamp(top, 0, std::max(0, static_cast<int>(lines.size()) - rows())); };
    auto rowAtTop = [&]() -> size_t {
        if (lines.empty()) return 0;
        return lines[static_cast<size_t>(std::min<int>(top, static_cast<int>(lines.size()) - 1))].row;
    };
    auto showRow = [&](size_t row) {  // a difference a third of the way down the screen
        top = static_cast<int>(firstLineOfRow[row]) - rows() / 3;
        clampTop();
    };
    auto spansFor = [&](size_t r) -> const LineSpans& {
        auto it = spanCache.find(r);
        if (it == spanCache.end()) {
            const DiffRow& row = result.rows[r];
            it = spanCache.emplace(r, ChangedSpans(document.left[*row.left], document.right[*row.right], options)).first;
        }
        return it->second;
    };

    rebuild();
    if (!result.rows.empty()) {  // open on the first difference
        if (result.rows[0].kind != RowKind::Same) {
            current = 0;
        } else if (auto first = NextDifference(result.rows, 0)) {
            current = *first;
            showRow(*first);
        }
    }

    const int numberWidth =
        static_cast<int>(std::to_wstring(std::max<size_t>({document.left.size(), document.right.size(), 1})).size());
    const bool ascii = console.UseBasicSymbols();
    const wchar_t divider = ascii ? L'|' : L'\x2502';

    auto render = [&]() {
        console.Clear(kAttrNormal);
        const int width = console.Width();
        // Header: both paths, each in its own half.
        int half = (width - 1) / 2;
        std::wstring header = PadToDisplayWidth(FitPath(document.leftPath.wstring(), half), static_cast<size_t>(half)) +
                              L" " + FitPath(document.rightPath.wstring(), width - half - 1);
        console.PutText(0, 0, PadToDisplayWidth(header, static_cast<size_t>(width)), kAttrHeader);

        for (int screen = 0; screen < rows(); ++screen) {
            size_t index = static_cast<size_t>(top + screen);
            SHORT y = static_cast<SHORT>(1 + screen);
            if (index >= lines.size()) break;
            const DisplayLine& line = lines[index];
            const DiffRow& row = result.rows[line.row];
            const WORD attr = KindAttr(row.kind);
            static const std::vector<std::pair<size_t, size_t>> kNone;
            const bool changed = row.kind == RowKind::Changed;
            if (unified) {
                // "  12 34 | text" with a '-' / '+' / ' ' marker.
                std::optional<size_t> leftNo = line.side > 0 ? std::nullopt : row.left;
                std::optional<size_t> rightNo = line.side < 0 ? std::nullopt : row.right;
                std::wstring gutter = Number(leftNo, numberWidth) + L" " + Number(rightNo, numberWidth) + L" ";
                wchar_t marker = line.side < 0 ? L'-' : line.side > 0 ? L'+' : L' ';
                WORD lineAttr = line.side < 0 ? (changed ? kAttrDiffChanged : kAttrDiffRemoved)
                                : line.side > 0 ? (changed ? kAttrDiffChanged : kAttrDiffAdded) : kAttrNormal;
                console.PutText(0, y, gutter, kAttrHeaderInactive);
                SHORT textX = static_cast<SHORT>(gutter.size());
                console.PutText(textX, y, std::wstring(1, marker), lineAttr);
                const std::wstring& text = line.side < 0 || (line.side == 0 && row.left) ? document.left[*row.left]
                                                                                           : document.right[*row.right];
                const auto& spans = changed ? (line.side < 0 ? spansFor(line.row).left : spansFor(line.row).right) : kNone;
                DrawCells(console, static_cast<SHORT>(textX + 2), y, width - textX - 2, text, spans, lineAttr,
                          kAttrDiffHighlight, offset);
            } else {
                // left half | marker | right half
                int leftWidth = (width - 1) / 2, rightWidth = width - leftWidth - 1;
                auto side = [&](SHORT x, int w, std::optional<size_t> no, const std::vector<std::wstring>& file,
                                const std::vector<std::pair<size_t, size_t>>& spans) {
                    std::wstring gutter = Number(no, numberWidth) + L" ";
                    console.PutText(x, y, gutter, kAttrHeaderInactive);
                    int textWidth = w - static_cast<int>(gutter.size());
                    if (no) DrawCells(console, static_cast<SHORT>(x + gutter.size()), y, textWidth, file[*no], spans,
                                      attr, kAttrDiffHighlight, offset);
                };
                side(0, leftWidth, row.left, document.left, changed ? spansFor(line.row).left : kNone);
                wchar_t marker = row.kind == RowKind::Same ? divider : KindMarker(row.kind);
                console.PutText(static_cast<SHORT>(leftWidth), y, std::wstring(1, marker),
                                row.kind == RowKind::Same ? kAttrBorder : attr);
                side(static_cast<SHORT>(leftWidth + 1), rightWidth, row.right, document.right,
                     changed ? spansFor(line.row).right : kNone);
            }
        }

        // Status line: counts, options, encodings, position, then the keys.
        std::wstring status = L" -" + std::to_wstring(result.removed) + L" +" + std::to_wstring(result.added) + L" ~" +
                              std::to_wstring(result.changed);
        if (result.Identical()) status = L" " + std::wstring(kStrDiffIdentical);
        status += L"  " + std::wstring(kStrDiffCaseLabel) + (options.ignoreCase ? kStrDiffIgnored : kStrDiffExact) + L"  " +
                  kStrDiffWhitespaceLabel + WhitespaceLabel(options.whitespace);
        if (document.leftEncoding != document.rightEncoding)
            status += L"  " + document.leftEncoding + L" | " + document.rightEncoding;
        if (!lines.empty())
            status += L"  " + std::to_wstring(std::min<size_t>(lines.size(), static_cast<size_t>(top) + 1)) + L"/" +
                      std::to_wstring(lines.size());
        if (!note.empty()) status += L"  " + note;
        console.PutText(0, static_cast<SHORT>(console.Height() - 1),
                        PadToDisplayWidth(status + L"  " + kStrDiffHint, static_cast<size_t>(width)), kAttrStatusBar);
    };

    auto changeOptions = [&](const DiffOptions& next) {
        size_t anchor = rowAtTop();
        size_t anchorLeft = anchor < result.rows.size() && result.rows[anchor].left ? *result.rows[anchor].left : 0;
        DiffResult fresh = recompute ? recompute(next) : ComputeLineDiff(document.left, document.right, next);
        if (fresh.status == DiffStatus::Cancelled) {
            note = kStrDiffRecomputeCancelled;
            return;
        }
        if (fresh.status == DiffStatus::TooLarge) {
            note = kStrDiffTooDifferent;
            return;
        }
        options = next;
        result = std::move(fresh);
        current.reset();
        rebuild();
        // Stay near the same left-file line.
        top = 0;
        for (size_t r = 0; r < result.rows.size(); ++r)
            if (result.rows[r].left && *result.rows[r].left >= anchorLeft) {
                top = static_cast<int>(firstLineOfRow[r]);
                break;
            }
        clampTop();
    };

    for (;;) {
        clampTop();
        render();
        console.Present();
        INPUT_RECORD record;
        DWORD read = 0;
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;
        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            console.UpdateSize();
            continue;
        }
        if (record.EventType == MOUSE_EVENT && record.Event.MouseEvent.dwEventFlags == MOUSE_WHEELED) {
            top += static_cast<short>(HIWORD(record.Event.MouseEvent.dwButtonState)) > 0 ? -3 : 3;
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;
        note.clear();
        const DWORD control = record.Event.KeyEvent.dwControlKeyState;
        const bool shift = (control & SHIFT_PRESSED) != 0;
        const wchar_t ch = record.Event.KeyEvent.uChar.UnicodeChar;
        const WORD key = record.Event.KeyEvent.wVirtualKeyCode;
        auto go = [&](std::optional<size_t> target) {
            if (target) {
                current = *target;
                showRow(*target);
            } else {
                note = kStrDiffNoMore;
            }
        };
        auto next = [&]() { go(NextDifference(result.rows, current ? *current : rowAtTop())); };
        auto previous = [&]() { go(PreviousDifference(result.rows, current ? *current : rowAtTop())); };
        auto scrolled = [&](int newTop) {
            top = newTop;
            current.reset();
        };
        switch (key) {
            case VK_UP: scrolled(top - 1); break;
            case VK_DOWN: scrolled(top + 1); break;
            case VK_PRIOR: scrolled(top - rows()); break;
            case VK_NEXT: scrolled(top + rows()); break;
            case VK_HOME: scrolled(0); break;
            case VK_END: scrolled(static_cast<int>(lines.size())); break;
            case VK_LEFT: offset = std::max(0, offset - 8); break;
            case VK_RIGHT: offset += 8; break;
            case VK_F7:
                if (shift) previous();
                else next();
                break;
            case VK_F3: {  // the left / right file in the normal viewer, at this line
                size_t r = rowAtTop();
                std::optional<size_t> no;
                if (r < result.rows.size()) no = shift ? result.rows[r].right : result.rows[r].left;
                ViewerStart start;
                start.line = no ? *no + 1 : 0;
                ShowTextFileViewer(console, shift ? document.rightPath : document.leftPath, start);
                break;
            }
            case VK_ESCAPE:
            case VK_F10:
                return;
            default:
                switch (ch) {
                    case L'n': next(); break;
                    case L'p':
                    case L'N': previous(); break;
                    case L'u':
                    case L'U':
                        unified = !unified;
                        {
                            size_t anchor = rowAtTop();
                            rebuild();
                            if (anchor < firstLineOfRow.size()) top = static_cast<int>(firstLineOfRow[anchor]);
                        }
                        break;
                    case L'i':
                    case L'I': {
                        DiffOptions o = options;
                        o.ignoreCase = !o.ignoreCase;
                        changeOptions(o);
                        break;
                    }
                    case L'w':
                    case L'W': {
                        DiffOptions o = options;
                        o.whitespace = o.whitespace == WhitespaceMode::Exact      ? WhitespaceMode::Trailing
                                       : o.whitespace == WhitespaceMode::Trailing ? WhitespaceMode::All
                                                                                  : WhitespaceMode::Exact;
                        changeOptions(o);
                        break;
                    }
                    case L'q':
                    case L'Q': return;
                    default: break;
                }
                break;
        }
    }
}

}  // namespace mc
