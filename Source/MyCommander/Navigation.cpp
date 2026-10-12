#include "Navigation.h"

#include "TextWidth.h"

#include <algorithm>

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

PanelLayout ComputePanelLayout(SHORT consoleWidth, bool leftActive, bool singlePanel) {
    PanelLayout layout;
    if (singlePanel) {
        layout.showLeft = leftActive;
        layout.showRight = !leftActive;
        SHORT inner = static_cast<SHORT>(std::max(0, consoleWidth - 2));
        layout.leftX = layout.rightX = 1;
        layout.leftWidth = leftActive ? inner : 0;
        layout.rightWidth = leftActive ? 0 : inner;
        layout.dividerX = -1;
        return layout;
    }
    SHORT divider = static_cast<SHORT>(consoleWidth / 2);
    layout.leftX = 1;
    layout.leftWidth = static_cast<SHORT>(std::max(0, divider - 1));
    layout.dividerX = divider;
    layout.rightX = static_cast<SHORT>(divider + 1);
    layout.rightWidth = static_cast<SHORT>(std::max(0, consoleWidth - divider - 2));
    return layout;
}

std::wstring FormatOtherPanelLabel(const std::wstring& text, int maxWidth) {
    const std::wstring prefix = L" Other panel: ";
    const int fixed = StringDisplayWidth(prefix) + 1;  // + the closing space
    const int room = maxWidth - fixed;
    if (room < 6) return L"";
    if (StringDisplayWidth(text) <= room) return prefix + text + L" ";
    std::wstring tail = text;
    while (!tail.empty() && StringDisplayWidth(tail) + 3 > room) tail.erase(0, 1);
    return prefix + L"..." + tail + L" ";
}

std::optional<PanelHit> HitTestPanel(SHORT consoleWidth, int visibleRows, SHORT mouseX, SHORT mouseY,
                                     SHORT topOffset) {
    return HitTestPanel(ComputePanelLayout(consoleWidth, true, false), visibleRows, mouseX, mouseY, topOffset);
}

std::optional<PanelHit> HitTestPanel(const PanelLayout& layout, int visibleRows, SHORT mouseX, SHORT mouseY,
                                     SHORT topOffset) {
    // border(+0) + path line(+1) + volume line(+2), all shifted down by
    // topOffset (FEA-0005's menu-bar row, if any).
    SHORT entriesStartRow = static_cast<SHORT>(topOffset + 3);
    int rowOffset = mouseY - entriesStartRow;
    if (rowOffset < 0 || rowOffset >= visibleRows) return std::nullopt;

    if (layout.showLeft && mouseX >= layout.leftX && mouseX < layout.leftX + layout.leftWidth)
        return PanelHit{true, rowOffset};
    if (layout.showRight && mouseX >= layout.rightX && mouseX < layout.rightX + layout.rightWidth)
        return PanelHit{false, rowOffset};
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
