#include "TestFramework.h"

#include "HintBar.h"
#include "TextWidth.h"

#include <algorithm>
#include <windows.h>

using mc::BuildHintBar;
using mc::BuildHintSegments;
using mc::HintAction;
using mc::HintBarRowCount;
using mc::HintContext;
using mc::HintSegment;
using mc::LayOutHintBar;
using mc::PlacedHintSegment;
using mc::StringDisplayWidth;

namespace {

// IS-0003: finds the segment with this exact label, or nullptr if absent —
// callers CHECK the pointer before dereferencing, since FAIL/CHECK need a
// TestContext& only a TEST_CASE body has.
const HintSegment* FindSegment(const std::vector<HintSegment>& segments, const std::wstring& label) {
    for (const auto& segment : segments) {
        if (segment.label == label) return &segment;
    }
    return nullptr;
}

}  // namespace

TEST_CASE(HintBar_CommandLinePrioritizesCommandEditing_UI008) {
    std::wstring hint = BuildHintBar(HintContext::CommandLine);
    CHECK(hint.find(L"Enter Run/Go") != std::wstring::npos);
    CHECK(hint.find(L"Esc Clear") != std::wstring::npos);
    CHECK(hint.find(L"F5 Copy") == std::wstring::npos);
}

TEST_CASE(HintBar_FilteredListingAdvertisesClearingFilter_UI008) {
    std::wstring hint = BuildHintBar(HintContext::FilteredListing);
    CHECK(hint.find(L"Ctrl+F Clear filter") != std::wstring::npos);
    CHECK(hint.find(L"F5 Copy") != std::wstring::npos);
}

TEST_CASE(HintBar_SelectionAdvertisesSelectionOperations_UI008) {
    std::wstring hint = BuildHintBar(HintContext::Selection);
    CHECK(hint.find(L"F5 Copy selection") != std::wstring::npos);
    CHECK(hint.find(L"F6 Move selection") != std::wstring::npos);
    CHECK(hint.find(L"Ctrl+A Attrib") != std::wstring::npos);
}

TEST_CASE(HintBar_DirectoryAndFileAdvertiseTheirDifferentEnterActions_UI008) {
    CHECK(BuildHintBar(HintContext::Directory).find(L"Enter Open") != std::wstring::npos);
    CHECK(BuildHintBar(HintContext::File).find(L"Enter Action") != std::wstring::npos);
    CHECK(BuildHintBar(HintContext::Directory).find(L"Alt+Left/Right History") != std::wstring::npos);
    CHECK(BuildHintBar(HintContext::Directory).find(L"Ctrl+H Hidden") != std::wstring::npos);
}

TEST_CASE(HintBar_InaccessibleLocationOffersRecoveryActions_UI008) {
    std::wstring hint = BuildHintBar(HintContext::InaccessibleLocation);
    CHECK(hint.find(L"Alt+F1/2 Drive") != std::wstring::npos);
    CHECK(hint.find(L"Backspace Parent") != std::wstring::npos);
}

// KEY-004: the shortcut reference is reachable from every context, not just
// some of them, so its hint should be advertised everywhere F10 Quit is.
TEST_CASE(HintBar_EveryContextAdvertisesTheShortcutReference_KEY004) {
    CHECK(BuildHintBar(HintContext::CommandLine).find(L"F1 Help") != std::wstring::npos);
    CHECK(BuildHintBar(HintContext::InaccessibleLocation).find(L"F1 Help") != std::wstring::npos);
    CHECK(BuildHintBar(HintContext::FilteredListing).find(L"F1 Help") != std::wstring::npos);
    CHECK(BuildHintBar(HintContext::Selection).find(L"F1 Help") != std::wstring::npos);
    CHECK(BuildHintBar(HintContext::Directory).find(L"F1 Help") != std::wstring::npos);
    CHECK(BuildHintBar(HintContext::File).find(L"F1 Help") != std::wstring::npos);
    CHECK(BuildHintBar(HintContext::EmptyListing).find(L"F1 Help") != std::wstring::npos);
}

// --- BuildHintSegments (IS-0003) ---------------------------------------------

TEST_CASE(BuildHintSegments_SingleKeyHint_HasAClickableAction) {
    auto segments = BuildHintSegments(HintContext::Directory);
    const HintSegment* seg = FindSegment(segments, L"F5 Copy");
    CHECK(seg != nullptr);
    if (seg) {
        CHECK(seg->action.has_value());
        if (seg->action) CHECK(seg->action->virtualKeyCode == VK_F5);
    }
}

TEST_CASE(BuildHintSegments_CtrlModifiedHint_CarriesCtrlHeld) {
    auto segments = BuildHintSegments(HintContext::Directory);
    const HintSegment* seg = FindSegment(segments, L"Ctrl+H Hidden");
    CHECK(seg != nullptr);
    if (seg && seg->action) {
        CHECK(seg->action->virtualKeyCode == L'H');
        CHECK(seg->action->ctrlHeld);
        CHECK(!seg->action->shiftHeld);
        CHECK(!seg->action->altHeld);
    }
}

TEST_CASE(BuildHintSegments_ShiftF7NewFile_CarriesShiftHeldNotPlainF7) {
    auto segments = BuildHintSegments(HintContext::Directory);
    const HintSegment* mkdir = FindSegment(segments, L"F7 MkDir");
    const HintSegment* newFile = FindSegment(segments, L"Shift+F7 NewFile");
    CHECK(mkdir != nullptr);
    CHECK(newFile != nullptr);
    if (mkdir && mkdir->action) {
        CHECK(mkdir->action->virtualKeyCode == VK_F7);
        CHECK(!mkdir->action->shiftHeld);
    }
    if (newFile && newFile->action) {
        CHECK(newFile->action->virtualKeyCode == VK_F7);
        CHECK(newFile->action->shiftHeld);
    }
}

TEST_CASE(BuildHintSegments_AltF7Find_CarriesAltHeld) {
    auto segments = BuildHintSegments(HintContext::EmptyListing);
    const HintSegment* seg = FindSegment(segments, L"Alt+F7 Find");
    CHECK(seg != nullptr);
    if (seg && seg->action) {
        CHECK(seg->action->virtualKeyCode == VK_F7);
        CHECK(seg->action->altHeld);
        CHECK(!seg->action->shiftHeld);
    }
}

// A hint naming two different keys as a slash pair can't unambiguously be
// resolved to one on a click, so it must stay non-clickable.
TEST_CASE(BuildHintSegments_AmbiguousSlashPairHints_AreNotClickable) {
    auto commandLine = BuildHintSegments(HintContext::CommandLine);
    const HintSegment* ctrlUpDown = FindSegment(commandLine, L"Ctrl+Up/Down History");
    CHECK(ctrlUpDown != nullptr);
    if (ctrlUpDown) CHECK(!ctrlUpDown->action.has_value());

    auto directory = BuildHintSegments(HintContext::Directory);
    const HintSegment* altLeftRight = FindSegment(directory, L"Alt+Left/Right History");
    CHECK(altLeftRight != nullptr);
    if (altLeftRight) CHECK(!altLeftRight->action.has_value());

    auto emptyListing = BuildHintSegments(HintContext::EmptyListing);
    const HintSegment* altF1F2 = FindSegment(emptyListing, L"Alt+F1/2 Drive");
    CHECK(altF1F2 != nullptr);
    if (altF1F2) CHECK(!altF1F2->action.has_value());
}

TEST_CASE(BuildHintSegments_EveryOtherSegment_IsClickable) {
    // The three hints naming two different keys as a slash pair -- the only
    // ones deliberately left non-clickable. "Enter Run/Go" also contains a
    // '/' but names one key (Enter) describing two possible outcomes, not
    // two keys, so it does get a real action -- can't tell the two cases
    // apart from the label text alone, hence this explicit list rather than
    // a "contains '/'" heuristic.
    const std::vector<std::wstring> kAmbiguousLabels = {L"Ctrl+Up/Down History", L"Alt+Left/Right History",
                                                         L"Alt+F1/2 Drive"};
    for (HintContext context : {HintContext::CommandLine, HintContext::InaccessibleLocation,
                                HintContext::FilteredListing, HintContext::Selection, HintContext::Directory,
                                HintContext::File, HintContext::EmptyListing}) {
        for (const auto& segment : BuildHintSegments(context)) {
            bool isAmbiguousPair =
                std::find(kAmbiguousLabels.begin(), kAmbiguousLabels.end(), segment.label) != kAmbiguousLabels.end();
            if (isAmbiguousPair) {
                CHECK(!segment.action.has_value());
            } else {
                CHECK(segment.action.has_value());
            }
        }
    }
}

// IS-0003: BuildHintBar is now derived from BuildHintSegments -- confirm the
// join stays byte-identical to what it was before (leading space, "  "
// separator) by reconstructing it by hand for one context and comparing.
TEST_CASE(BuildHintSegments_JoinedWithSeparators_MatchesBuildHintBar) {
    std::wstring rebuilt = L" ";
    bool first = true;
    for (const auto& segment : BuildHintSegments(HintContext::Directory)) {
        if (!first) rebuilt += L"  ";
        rebuilt += segment.label;
        first = false;
    }
    CHECK(rebuilt == BuildHintBar(HintContext::Directory));
}

// --- LayOutHintBar / HintBarRowCount (IS-0007) ------------------------------

TEST_CASE(LayOutHintBar_FitsOnOneRowAtAWideWidth_IS0007) {
    auto placed = LayOutHintBar(HintContext::CommandLine, 200);
    CHECK(HintBarRowCount(HintContext::CommandLine, 200) == 1);
    CHECK(!placed.empty());
    for (const auto& p : placed) CHECK(p.row == 0);
}

// The core regression guard: at the documented minimum terminal width,
// every segment BuildHintSegments produces must still appear somewhere in
// the wrapped layout -- nothing silently dropped, which is what IS-0007 is
// actually about.
TEST_CASE(LayOutHintBar_Directory_AtMinimumWidth_PlacesEverySegmentAndUsesMultipleRows_IS0007) {
    constexpr SHORT kMinWidth = 60;  // main.cpp's own UI-006 floor
    auto segments = BuildHintSegments(HintContext::Directory);
    auto placed = LayOutHintBar(HintContext::Directory, kMinWidth);

    CHECK(placed.size() == segments.size());
    for (const auto& segment : segments) {
        auto it = std::find_if(placed.begin(), placed.end(),
                               [&](const PlacedHintSegment& p) { return p.segment.label == segment.label; });
        CHECK(it != placed.end());
    }
    CHECK(HintBarRowCount(HintContext::Directory, kMinWidth) > 1);
}

TEST_CASE(LayOutHintBar_EmptyListing_AtMinimumWidth_PlacesEverySegment_IS0007) {
    constexpr SHORT kMinWidth = 60;
    auto segments = BuildHintSegments(HintContext::EmptyListing);
    auto placed = LayOutHintBar(HintContext::EmptyListing, kMinWidth);
    CHECK(placed.size() == segments.size());
}

// No placed segment's label is ever allowed to run past the right edge --
// wrapping must never let that happen (a single over-wide segment on an
// otherwise-empty row is the one exception, since there's nowhere further
// to wrap it to; not exercised here since no real hint label is anywhere
// near 60 columns wide).
TEST_CASE(LayOutHintBar_NoSegmentOverflowsTheRightEdge_IS0007) {
    for (HintContext context : {HintContext::CommandLine, HintContext::InaccessibleLocation,
                                HintContext::FilteredListing, HintContext::Selection, HintContext::Directory,
                                HintContext::File, HintContext::EmptyListing}) {
        for (int rawWidth : {60, 61, 79, 80, 120}) {
            SHORT width = static_cast<SHORT>(rawWidth);
            for (const auto& p : LayOutHintBar(context, width)) {
                int endCol = static_cast<int>(p.col) + StringDisplayWidth(p.segment.label);
                CHECK(endCol <= width);
            }
        }
    }
}

// The walk only ever breaks lines between segments -- it must never reorder
// them, so a segment's row index can never decrease relative to the
// original BuildHintSegments order.
TEST_CASE(LayOutHintBar_RowsAreMonotonicallyNonDecreasing_IS0007) {
    auto placed = LayOutHintBar(HintContext::Directory, 60);
    for (size_t i = 1; i < placed.size(); ++i) {
        CHECK(placed[i].row >= placed[i - 1].row);
    }
}

TEST_CASE(LayOutHintBar_MaxRowsCap_TruncatesCleanlyWithoutCrashing_IS0007) {
    auto placed = LayOutHintBar(HintContext::Directory, 60, /*maxRows=*/1);
    CHECK(HintBarRowCount(HintContext::Directory, 60, /*maxRows=*/1) == 1);
    for (const auto& p : placed) CHECK(p.row == 0);
    CHECK(placed.size() < BuildHintSegments(HintContext::Directory).size());  // some segments didn't fit and were dropped
}

TEST_CASE(LayOutHintBar_NonPositiveWidthOrMaxRows_ReturnsEmptyWithoutCrashing_IS0007) {
    CHECK(LayOutHintBar(HintContext::Directory, 0).empty());
    CHECK(LayOutHintBar(HintContext::Directory, -10).empty());
    CHECK(LayOutHintBar(HintContext::Directory, 60, /*maxRows=*/0).empty());
    CHECK(HintBarRowCount(HintContext::Directory, 0) >= 1);  // still a safe, non-zero row count to reserve
}
