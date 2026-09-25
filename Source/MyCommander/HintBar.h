#pragma once

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include "TextWidth.h"

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

namespace mc {

// UI-008: the main-screen hint bar advertises only the actions that make
// sense for the active input/listing state. Keeping this pure makes every
// state independently testable without a console handle.
enum class HintContext {
    CommandLine,
    InaccessibleLocation,
    FilteredListing,
    Selection,
    Directory,
    File,
    EmptyListing,
};

// IS-0003: the keyboard action a hint segment triggers when clicked,
// expressed in the same virtual-key/modifier shape a real KEY_EVENT
// carries -- so a click can be replayed as a synthetic key press through
// wmain's own key-dispatch switch (see main.cpp's SimulateKeyPress)
// without that switch needing to know anything happened.
struct HintAction {
    WORD virtualKeyCode = 0;
    bool ctrlHeld = false;
    bool shiftHeld = false;
    bool altHeld = false;
};

// `action` is nullopt for a hint that names two different keys as a
// slash-separated pair (e.g. "Alt+Left/Right History", "Ctrl+Up/Down
// History", "Alt+F1/2 Drive") -- a click there can't unambiguously pick
// which of the two was meant, so those stay informational-only rather than
// guessing. Every single-key hint gets a real action.
struct HintSegment {
    std::wstring label;
    std::optional<HintAction> action;
};

inline std::vector<HintSegment> BuildHintSegments(HintContext context) {
    auto seg = [](const wchar_t* label, WORD vk, bool ctrl = false, bool shift = false, bool alt = false) {
        return HintSegment{label, HintAction{vk, ctrl, shift, alt}};
    };
    auto info = [](const wchar_t* label) { return HintSegment{label, std::nullopt}; };

    switch (context) {
        case HintContext::CommandLine:
            return {
                seg(L"Enter Run/Go", VK_RETURN),
                seg(L"Backspace Edit", VK_BACK),
                seg(L"Esc Clear", VK_ESCAPE),
                info(L"Ctrl+Up/Down History"),
                seg(L"Ctrl+H Hidden", L'H', /*ctrl=*/true),
                seg(L"F9 Shell", VK_F9),
                seg(L"F1 Help", VK_F1),
                seg(L"F10 Quit", VK_F10),
            };
        case HintContext::InaccessibleLocation:
            return {
                info(L"Alt+Left/Right History"),
                info(L"Alt+F1/2 Drive"),
                seg(L"Backspace Parent", VK_BACK),
                seg(L"Ctrl+F Filter", L'F', /*ctrl=*/true),
                seg(L"Ctrl+H Hidden", L'H', /*ctrl=*/true),
                seg(L"F9 Shell", VK_F9),
                seg(L"F1 Help", VK_F1),
                seg(L"F10 Quit", VK_F10),
            };
        case HintContext::FilteredListing:
            return {
                seg(L"Enter Open", VK_RETURN),
                seg(L"Ctrl+F Clear filter", L'F', /*ctrl=*/true),
                seg(L"Ctrl+H Hidden", L'H', /*ctrl=*/true),
                seg(L"F3 View", VK_F3),
                seg(L"F5 Copy", VK_F5),
                seg(L"F6 Move", VK_F6),
                seg(L"F8 Delete", VK_F8),
                seg(L"F1 Help", VK_F1),
                seg(L"F10 Quit", VK_F10),
            };
        case HintContext::Selection:
            return {
                seg(L"F2 Rename", VK_F2),
                seg(L"F3 View", VK_F3),
                seg(L"F4 Edit", VK_F4),
                seg(L"F5 Copy selection", VK_F5),
                seg(L"F6 Move selection", VK_F6),
                seg(L"F8 Delete", VK_F8),
                seg(L"Ctrl+A Attrib", L'A', /*ctrl=*/true),
                seg(L"Ctrl+H Hidden", L'H', /*ctrl=*/true),
                seg(L"F1 Help", VK_F1),
                seg(L"F10 Quit", VK_F10),
            };
        case HintContext::Directory:
            return {
                seg(L"Enter Open", VK_RETURN),
                info(L"Alt+Left/Right History"),
                seg(L"Backspace Parent", VK_BACK),
                seg(L"Ctrl+H Hidden", L'H', /*ctrl=*/true),
                seg(L"F2 Rename", VK_F2),
                seg(L"F5 Copy", VK_F5),
                seg(L"F6 Move", VK_F6),
                seg(L"F7 MkDir", VK_F7),
                seg(L"Shift+F7 NewFile", VK_F7, /*ctrl=*/false, /*shift=*/true),
                seg(L"F8 Delete", VK_F8),
                seg(L"F1 Help", VK_F1),
                seg(L"F10 Quit", VK_F10),
            };
        case HintContext::File:
            return {
                seg(L"Enter Action", VK_RETURN),
                info(L"Alt+Left/Right History"),
                seg(L"Ctrl+H Hidden", L'H', /*ctrl=*/true),
                seg(L"F2 Rename", VK_F2),
                seg(L"F3 View", VK_F3),
                seg(L"F4 Edit", VK_F4),
                seg(L"F5 Copy", VK_F5),
                seg(L"F6 Move", VK_F6),
                seg(L"F8 Delete", VK_F8),
                seg(L"Ctrl+A Attrib", L'A', /*ctrl=*/true),
                seg(L"F1 Help", VK_F1),
                seg(L"F10 Quit", VK_F10),
            };
        case HintContext::EmptyListing:
            return {
                info(L"Alt+Left/Right History"),
                seg(L"Backspace Parent", VK_BACK),
                seg(L"F7 MkDir", VK_F7),
                seg(L"Shift+F7 NewFile", VK_F7, /*ctrl=*/false, /*shift=*/true),
                info(L"Alt+F1/2 Drive"),
                seg(L"Ctrl+F Filter", L'F', /*ctrl=*/true),
                seg(L"Ctrl+H Hidden", L'H', /*ctrl=*/true),
                seg(L"Alt+F7 Find", VK_F7, /*ctrl=*/false, /*shift=*/false, /*alt=*/true),
                seg(L"F9 Shell", VK_F9),
                seg(L"F1 Help", VK_F1),
                seg(L"F10 Quit", VK_F10),
            };
    }
    return {seg(L"F1 Help", VK_F1), seg(L"F10 Quit", VK_F10)};
}

// Re-expressed in terms of BuildHintSegments (rather than its own
// hand-maintained switch) so the two can never drift apart -- same leading
// space and "  " inter-segment separator as before IS-0003, so this stays
// byte-identical to what every existing HintBarTests.cpp assertion expects.
inline std::wstring BuildHintBar(HintContext context) {
    std::wstring result = L" ";
    bool first = true;
    for (const auto& segment : BuildHintSegments(context)) {
        if (!first) result += L"  ";
        result += segment.label;
        first = false;
    }
    return result;
}

// IS-0007: a hint segment placed on a specific row/column — the output of
// laying BuildHintSegments' flat list out across as many rows as it takes
// to show every segment at `width` display columns. Never splits a
// segment's label across two rows; each row is filled greedily
// left-to-right, wrapping to a new row only when the next segment (plus
// its "  " separator) wouldn't fit.
struct PlacedHintSegment {
    HintSegment segment;
    int row = 0;    // 0-based, relative to the hint bar's own first row
    SHORT col = 0;  // display column within that row
};

// Lays BuildHintSegments(context) out across however many rows are needed
// at `width` display columns, never more than `maxRows` — segments beyond
// that cap are simply omitted from the result, the same "some commands
// aren't reachable in an extreme case" fallback a single truncated row
// already had before this, just for a much narrower slice of cases.
// Returns an empty vector for `width <= 0`.
inline std::vector<PlacedHintSegment> LayOutHintBar(HintContext context, SHORT width, int maxRows = 3) {
    std::vector<PlacedHintSegment> placed;
    if (width <= 0 || maxRows <= 0) return placed;

    int row = 0;
    SHORT col = 1;  // BuildHintBar's leading " "
    bool firstOnRow = true;
    for (const auto& segment : BuildHintSegments(context)) {
        SHORT segWidth = static_cast<SHORT>(StringDisplayWidth(segment.label));
        SHORT advance = static_cast<SHORT>(firstOnRow ? 0 : 2);  // "  " separator, none before the first segment on a row
        if (!firstOnRow && static_cast<int>(col) + advance + segWidth > width) {
            if (row + 1 >= maxRows) break;  // capped — remaining segments are simply not placed
            ++row;
            col = 1;
            firstOnRow = true;
            advance = 0;
        }
        col = static_cast<SHORT>(col + advance);
        placed.push_back(PlacedHintSegment{segment, row, col});
        col = static_cast<SHORT>(col + segWidth);
        firstOnRow = false;
    }
    return placed;
}

// How many rows LayOutHintBar(context, width, maxRows) actually used — the
// value callers that reserve screen rows (DrawFrame/ComputeVisibleRows)
// need, without materializing the full placement list just to count rows.
inline int HintBarRowCount(HintContext context, SHORT width, int maxRows = 3) {
    auto placed = LayOutHintBar(context, width, maxRows);
    int rows = 0;
    for (const auto& p : placed) rows = std::max(rows, p.row);
    return placed.empty() ? 1 : rows + 1;
}

}  // namespace mc
