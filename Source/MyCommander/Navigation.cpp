#include "Navigation.h"

namespace mc {

NavIntent ClassifyNavigationKey(WORD virtualKeyCode, const NavKeyContext& ctx) {
    switch (virtualKeyCode) {
        case VK_TAB:
            return NavIntent::SwitchActivePanel;
        case VK_LEFT:
            return (ctx.altHeld && ctx.commandLineEmpty) ? NavIntent::PanelHistoryBack : NavIntent::None;
        case VK_RIGHT:
            return (ctx.altHeld && ctx.commandLineEmpty) ? NavIntent::PanelHistoryForward : NavIntent::None;
        case VK_UP:
            if (ctx.ctrlHeld) return ctx.hasCommandHistory ? NavIntent::CommandHistoryUp : NavIntent::None;
            return NavIntent::CursorUp;
        case VK_DOWN:
            if (ctx.ctrlHeld) return ctx.isBrowsingHistory ? NavIntent::CommandHistoryDown : NavIntent::None;
            return NavIntent::CursorDown;
        case VK_PRIOR:
            return NavIntent::CursorPageUp;
        case VK_NEXT:
            return NavIntent::CursorPageDown;
        case VK_HOME:
            return NavIntent::CursorToFirst;
        case VK_END:
            return NavIntent::CursorToLast;
        case VK_BACK:
            return ctx.commandLineEmpty ? NavIntent::GoToParentDirectory : NavIntent::ClearCommandLineChar;
        default:
            return NavIntent::None;
    }
}

std::optional<PanelHit> HitTestPanel(SHORT consoleWidth, int visibleRows, SHORT mouseX, SHORT mouseY) {
    SHORT leftWidth = static_cast<SHORT>(consoleWidth / 2);
    SHORT rightX = static_cast<SHORT>(leftWidth + 1);

    constexpr SHORT kEntriesStartRow = 3;  // border(0) + path line(1) + volume line(2)
    int rowOffset = mouseY - kEntriesStartRow;
    if (rowOffset < 0 || rowOffset >= visibleRows) return std::nullopt;

    if (mouseX >= 1 && mouseX < leftWidth) return PanelHit{true, rowOffset};
    if (mouseX >= rightX && mouseX < static_cast<SHORT>(consoleWidth - 1)) return PanelHit{false, rowOffset};
    return std::nullopt;
}

int MatchClickRegion(const std::vector<ClickableRegion>& regions, SHORT mouseX, SHORT mouseY) {
    for (size_t i = 0; i < regions.size(); ++i) {
        const ClickableRegion& r = regions[i];
        if (mouseY == r.row && mouseX >= r.startCol && mouseX < r.endCol) return static_cast<int>(i);
    }
    return -1;
}

}  // namespace mc
