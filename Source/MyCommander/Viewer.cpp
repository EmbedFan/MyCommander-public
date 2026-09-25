#include "Viewer.h"

#include "Dialog.h"
#include "PathUtil.h"
#include "Strings.h"
#include "TextFile.h"
#include "TextWidth.h"
#include "Theme.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>

namespace mc {

namespace {

// UI-011: display-width-aware, so a viewed line containing full-width
// (CJK) characters still pads/truncates to the right on-screen column.
std::wstring PadOrTrim(const std::wstring& text, size_t width) {
    return PadToDisplayWidth(text, width);
}

bool ContainsCaseInsensitive(const std::wstring& haystack, const std::wstring& needle) {
    if (needle.empty()) return false;
    auto toLower = [](std::wstring s) {
        std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
        return s;
    };
    return toLower(haystack).find(toLower(needle)) != std::wstring::npos;
}

} // namespace

void ShowTextFileViewer(Console& console, const std::filesystem::path& file) {
    // VEE-004: indexed once (a bounded-memory scan for line boundaries only)
    // rather than reading the whole file into memory, so a file of any size
    // can be paged through below. `in` stays open for the viewer's whole
    // lifetime and is reused by every ReadIndexedLine call — scrolling or
    // searching never reopens the file per line.
    TextFileIndex fileIndex = IndexTextFileForViewing(file);
    std::ifstream in(ToLongPath(file), std::ios::binary);
    size_t lineCount = LineCount(fileIndex);

    auto lineAt = [&](size_t i) { return ReadIndexedLine(in, fileIndex, i); };

    int topLine = 0;
    std::wstring searchTerm;

    auto contentHeight = [&]() { return std::max<int>(1, console.Height() - 2); };

    auto clampTop = [&]() {
        int maxTop = std::max<int>(0, static_cast<int>(lineCount) - contentHeight());
        topLine = std::clamp(topLine, 0, maxTop);
    };

    // Draws the viewer into the back buffer without presenting it — the
    // shape dialogs expect for their "redraw what's behind me" callback.
    auto renderFrame = [&]() {
        console.Clear(kAttrNormal);
        SHORT width = console.Width();

        console.PutText(0, 0, PadOrTrim(file.wstring(), width), kAttrHeader);

        int rows = contentHeight();
        for (int row = 0; row < rows; ++row) {
            size_t idx = static_cast<size_t>(topLine + row);
            std::wstring line = idx < lineCount ? lineAt(idx) : L"";
            console.PutText(0, static_cast<SHORT>(1 + row), PadOrTrim(line, width), kAttrNormal);
        }

        std::wstring status;
        if (!fileIndex.ok) {
            status = fileIndex.error;
        } else if (lineCount == 0) {
            status = kStrViewerEmptyFile;
        } else {
            int lastVisible = std::min<int>(static_cast<int>(lineCount), topLine + rows);
            status = kStrViewerLinePrefix + std::to_wstring(topLine + 1) + L"-" + std::to_wstring(lastVisible) + L"/" +
                    std::to_wstring(lineCount);
        }
        std::wstring line = status + kStrViewerHintLine;
        console.PutText(0, static_cast<SHORT>(console.Height() - 1), PadOrTrim(line, width), kAttrStatusBar);
    };

    auto paint = [&]() {
        renderFrame();
        console.Present();
    };

    auto findNext = [&]() {
        if (searchTerm.empty() || lineCount == 0) return;
        for (size_t offset = 1; offset <= lineCount; ++offset) {
            size_t idx = (static_cast<size_t>(topLine) + offset) % lineCount;
            if (ContainsCaseInsensitive(lineAt(idx), searchTerm)) {
                topLine = static_cast<int>(idx);
                clampTop();
                return;
            }
        }
        ShowMessage(console, kStrSearchTitle, {FormatSearchTermNotFound(searchTerm)}, renderFrame);
    };

    clampTop();
    paint();

    bool running = true;
    while (running) {
        INPUT_RECORD record;
        DWORD read = 0;
        if (!ReadConsoleInputW(console.InputHandle(), &record, 1, &read) || read == 0) continue;

        if (record.EventType == WINDOW_BUFFER_SIZE_EVENT) {
            console.UpdateSize();
            clampTop();
            paint();
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

        WORD key = record.Event.KeyEvent.wVirtualKeyCode;
        wchar_t ch = record.Event.KeyEvent.uChar.UnicodeChar;
        switch (key) {
            case VK_UP:
                --topLine;
                clampTop();
                paint();
                break;
            case VK_DOWN:
                ++topLine;
                clampTop();
                paint();
                break;
            case VK_PRIOR:
                topLine -= contentHeight();
                clampTop();
                paint();
                break;
            case VK_NEXT:
                topLine += contentHeight();
                clampTop();
                paint();
                break;
            case VK_HOME:
                topLine = 0;
                paint();
                break;
            case VK_END:
                topLine = std::max<int>(0, static_cast<int>(lineCount) - contentHeight());
                paint();
                break;
            case VK_ESCAPE:
            case VK_F3:
            case VK_F10:
                running = false;
                break;
            default:
                if (ch == L'/') {
                    auto term = ShowTextPrompt(console, kStrSearchTitle, searchTerm, renderFrame);
                    if (term && !term->empty()) {
                        searchTerm = *term;
                        findNext();
                    }
                    paint();
                } else if (ch == L'n' || ch == L'N') {
                    findNext();
                    paint();
                }
                break;
        }
    }
}

} // namespace mc
