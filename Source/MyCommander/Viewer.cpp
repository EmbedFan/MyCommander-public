#include "Viewer.h"

#include "Dialog.h"
#include "HexDump.h"
#include "PathUtil.h"
#include "Strings.h"
#include "TextFile.h"
#include "TextWidth.h"
#include "Theme.h"

#include <windows.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <optional>
#include <system_error>
#include <utility>
#include <vector>

namespace mc {

namespace {

// Byte column of a hex row's first byte: the 16-digit offset plus two spaces.
constexpr size_t kHexFirstByteColumn = 18;

size_t HexAsciiColumn(size_t bytesPerRow) { return kHexFirstByteColumn + bytesPerRow * 3 + 1; }

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

std::wstring ToHex(uint64_t value, int digits) {
    std::wstring out(static_cast<size_t>(digits), L'0');
    for (int i = digits - 1; i >= 0; --i) {
        out[static_cast<size_t>(i)] = L"0123456789ABCDEF"[value & 0xF];
        value >>= 4;
    }
    return out;
}

// An ASCII search in hex mode matches raw bytes, so the query is searched as
// its UTF-8 encoding, which is what the file bytes of a UTF-8 text file hold.
std::vector<uint8_t> Utf8Bytes(const std::wstring& text) {
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::vector<uint8_t> bytes(size > 0 ? static_cast<size_t>(size) : 0);
    if (size > 0) {
        WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), reinterpret_cast<char*>(bytes.data()),
                            size, nullptr, nullptr);
    }
    return bytes;
}

// VEE-005 (cursor): the status line's offset is always the cursor's own
// position, not the view's scroll anchor -- the anchor is purely a rendering
// detail of which rows happen to be on screen.
std::wstring FormatHexStatus(uint64_t cursor, uint64_t fileSize, size_t itemsPerRow, bool fit, size_t pan,
                             size_t visibleColumns) {
    size_t total = HexRowColumns(itemsPerRow);
    size_t last = std::min(total, pan + visibleColumns);
    std::wstring items = (fit ? L"fit: " : L"") + std::to_wstring(itemsPerRow) + L" items/row";
    return L"Offset 0x" + ToHex(cursor, 16) + L" / 0x" + ToHex(fileSize, 16) + L"  " + items + L"  cols " +
           std::to_wstring(pan + 1) + L"-" + std::to_wstring(last) + L" of " + std::to_wstring(total);
}

} // namespace

void ShowTextFileViewer(Console& console, const std::filesystem::path& file) {
    ShowTextFileViewer(console, file, ViewerStart{});
}

void ShowTextFileViewer(Console& console, const std::filesystem::path& file, const ViewerStart& start) {
    // VEE-004: indexed once (a bounded-memory scan for line boundaries only)
    // rather than reading the whole file into memory, so a file of any size
    // can be paged through below. `in` stays open for the viewer's whole
    // lifetime and is reused by every ReadIndexedLine call — scrolling or
    // searching never reopens the file per line.
    TextFileIndex fileIndex = IndexTextFileForViewing(file);
    std::ifstream in(ToLongPath(file), std::ios::binary);
    size_t lineCount = LineCount(fileIndex);

    std::error_code sizeError;
    uint64_t fileSize = std::filesystem::file_size(ToLongPath(file), sizeError);
    if (sizeError) fileSize = 0;
    uint64_t lastByte = fileSize > 0 ? fileSize - 1 : 0;

    auto lineAt = [&](size_t i) { return ReadIndexedLine(in, fileIndex, i); };

    bool hexMode = false;
    // SRC-004: Find files' F3 opens with the matching line at the top and the
    // term ready, so 'n' goes on to the *next* match (findNext starts below topLine).
    int topLine = start.line > 0 ? static_cast<int>(start.line) - 1 : 0;
    std::wstring searchTerm = start.searchTerm;

    // VEE-005: hex mode's own state. `hexAnchor` is the first visible byte
    // (always row-aligned to the items per row) -- purely where the view is
    // scrolled to. `hexCursor` is the single byte the cursor is on, shown
    // highlighted in both the hex and ASCII columns and reported as the
    // status line's offset. The two are kept in sync by ensureCursorVisible,
    // which scrolls (and pans) the view only as far as needed to keep the
    // cursor on screen, the same way a text editor's cursor and scrollbar
    // interact.
    size_t hexSetting = 8;  // kHexFitSetting or a fixed count, see ResolveHexItems
    uint64_t hexAnchor = 0;
    uint64_t hexCursor = 0;
    size_t hexPan = 0;
    std::optional<std::pair<uint64_t, size_t>> hexHit;  // start and length of the last hex-mode match

    auto contentHeight = [&]() { return std::max<int>(1, console.Height() - 2); };
    auto visibleColumns = [&]() { return static_cast<size_t>(console.Width()); };
    auto hexItems = [&]() { return ResolveHexItems(hexSetting, visibleColumns()); };

    auto clampTop = [&]() {
        int maxTop = std::max<int>(0, static_cast<int>(lineCount) - contentHeight());
        topLine = std::clamp(topLine, 0, maxTop);
    };
    clampTop();  // a start line near the end still fills the screen

    auto clampHex = [&]() {
        hexAnchor = std::min(HexRowStart(hexAnchor, hexItems()),
                             HexMaxAnchor(fileSize, hexItems(), static_cast<size_t>(contentHeight())));
        hexPan = std::min(hexPan, HexMaxPan(hexItems(), visibleColumns()));
    };

    // Scrolls (and pans) the view by the minimum needed to bring hexCursor
    // back on screen, vertically and horizontally. Called after every cursor
    // move, so the cursor is always visible without the user ever having to
    // scroll manually.
    auto ensureCursorVisible = [&]() {
        size_t items = hexItems();
        uint64_t cursorRow = HexRowStart(hexCursor, items);
        int rows = contentHeight();
        if (cursorRow < hexAnchor) {
            hexAnchor = cursorRow;
        } else {
            uint64_t bottomRow = hexAnchor + static_cast<uint64_t>(rows - 1) * items;
            if (cursorRow > bottomRow) hexAnchor = cursorRow - static_cast<uint64_t>(rows - 1) * items;
        }
        clampHex();

        size_t slot = static_cast<size_t>(hexCursor - cursorRow);
        size_t hexColumn = kHexFirstByteColumn + slot * 3;
        size_t asciiColumn = HexAsciiColumn(items) + slot;
        size_t visible = visibleColumns();
        if (hexColumn < hexPan) {
            hexPan = hexColumn;
        } else if (asciiColumn >= hexPan + visible) {
            hexPan = asciiColumn + 1 - visible;
        }
        hexPan = std::min(hexPan, HexMaxPan(items, visible));
    };

    auto enterHexFromTopLine = [&]() {
        hexCursor = lineCount == 0 ? 0 : fileIndex.lineStarts[static_cast<size_t>(topLine)];
        hexAnchor = hexCursor;
        hexPan = 0;
        ensureCursorVisible();
    };

    auto topLineFromCursor = [&]() {
        topLine = static_cast<int>(LineIndexForByte(fileIndex.lineStarts, hexCursor));
        clampTop();
    };

    auto putHexGlyph = [&](int row, size_t fullColumn, wchar_t glyph, WORD attr) {
        if (fullColumn < hexPan) return;
        size_t screenColumn = fullColumn - hexPan;
        if (screenColumn >= visibleColumns()) return;
        console.PutText(static_cast<SHORT>(screenColumn), static_cast<SHORT>(1 + row), std::wstring(1, glyph), attr);
    };

    auto drawHexRows = [&]() {
        SHORT width = console.Width();
        int rows = contentHeight();
        uint64_t remaining = fileSize > hexAnchor ? fileSize - hexAnchor : 0;
        size_t wanted = static_cast<size_t>(std::min<uint64_t>(remaining, static_cast<uint64_t>(rows) * hexItems()));
        std::vector<uint8_t> bytes(wanted);
        if (wanted > 0) {
            in.clear();
            in.seekg(static_cast<std::streamoff>(hexAnchor));
            in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(wanted));
            bytes.resize(static_cast<size_t>(in.gcount()));
        }

        for (int row = 0; row < rows; ++row) {
            size_t begin = static_cast<size_t>(row) * hexItems();
            std::wstring text;
            if (begin < bytes.size()) {
                size_t count = std::min(hexItems(), bytes.size() - begin);
                text = FormatHexRow(hexAnchor + begin, bytes.data() + begin, count, hexItems(), hexPan,
                                    static_cast<size_t>(width));
            }
            console.PutText(0, static_cast<SHORT>(1 + row), PadOrTrim(text, width), kAttrNormal);
        }

        auto drawByteHighlight = [&](uint64_t b, WORD attr) {
            if (b < hexAnchor || b >= hexAnchor + bytes.size()) return;
            size_t rel = static_cast<size_t>(b - hexAnchor);
            int row = static_cast<int>(rel / hexItems());
            size_t slot = rel % hexItems();
            uint8_t value = bytes[rel];
            std::wstring hex = ToHex(value, 2);
            putHexGlyph(row, kHexFirstByteColumn + slot * 3, hex[0], attr);
            putHexGlyph(row, kHexFirstByteColumn + slot * 3 + 1, hex[1], attr);
            putHexGlyph(row, HexAsciiColumn(hexItems()) + slot,
                       (value >= 0x20 && value <= 0x7E) ? static_cast<wchar_t>(value) : L'.', attr);
        };

        if (hexHit) {
            auto [hitStart, hitLength] = *hexHit;
            for (uint64_t b = hitStart; b < hitStart + hitLength; ++b) drawByteHighlight(b, kAttrHeaderActive);
        }
        // Drawn last so the cursor stays visible even where it coincides with a search hit.
        drawByteHighlight(hexCursor, kAttrCursor);
    };

    // Draws the viewer into the back buffer without presenting it — the
    // shape dialogs expect for their "redraw what's behind me" callback.
    auto renderFrame = [&]() {
        console.Clear(kAttrNormal);
        SHORT width = console.Width();

        console.PutText(0, 0, PadOrTrim(start.title.empty() ? file.wstring() : start.title, width), kAttrHeader);

        std::wstring status;
        if (hexMode) {
            drawHexRows();
            status =
                FormatHexStatus(hexCursor, fileSize, hexItems(), hexSetting == kHexFitSetting, hexPan, visibleColumns());
        } else {
            int rows = contentHeight();
            for (int row = 0; row < rows; ++row) {
                size_t idx = static_cast<size_t>(topLine + row);
                std::wstring line = idx < lineCount ? lineAt(idx) : L"";
                console.PutText(0, static_cast<SHORT>(1 + row), PadOrTrim(line, width), kAttrNormal);
            }
            if (!fileIndex.ok) {
                status = fileIndex.error;
            } else if (lineCount == 0) {
                status = kStrViewerEmptyFile;
            } else {
                int lastVisible = std::min<int>(static_cast<int>(lineCount), topLine + rows);
                status = kStrViewerLinePrefix + std::to_wstring(topLine + 1) + L"-" + std::to_wstring(lastVisible) +
                         L"/" + std::to_wstring(lineCount);
            }
        }
        std::wstring line = status + (hexMode ? kStrViewerHexHintLine : kStrViewerHintLine);
        console.PutText(0, static_cast<SHORT>(console.Height() - 1), PadOrTrim(line, width), kAttrStatusBar);
    };

    auto paint = [&]() {
        renderFrame();
        console.Present();
    };

    auto findNext = [&]() {
        if (searchTerm.empty()) return;
        bool byteQuery = searchTerm[0] == L'#';

        if (!hexMode && !byteQuery) {
            if (lineCount == 0) return;
            for (size_t offset = 1; offset <= lineCount; ++offset) {
                size_t idx = (static_cast<size_t>(topLine) + offset) % lineCount;
                if (ContainsCaseInsensitive(lineAt(idx), searchTerm)) {
                    topLine = static_cast<int>(idx);
                    clampTop();
                    return;
                }
            }
            ShowMessage(console, kStrSearchTitle, {FormatSearchTermNotFound(searchTerm)}, renderFrame);
            return;
        }

        std::vector<uint8_t> pattern;
        if (byteQuery) {
            auto parsed = ParseHexPattern(searchTerm.substr(1));
            if (!parsed) {
                ShowMessage(console, kStrSearchTitle, {kStrViewerInvalidHex}, renderFrame);
                return;
            }
            pattern = *parsed;
        } else {
            pattern = Utf8Bytes(searchTerm);
        }

        uint64_t from = hexMode ? hexCursor + 1 : (lineCount == 0 ? 0 : fileIndex.lineStarts[static_cast<size_t>(topLine)] + 1);
        in.clear();
        auto hit = FindBytePattern(in, fileSize, pattern, from, !byteQuery);
        if (!hit) {
            ShowMessage(console, kStrSearchTitle, {FormatSearchTermNotFound(searchTerm)}, renderFrame);
            return;
        }

        if (hexMode) {
            hexCursor = *hit;
            hexAnchor = HexRowStart(*hit, hexItems());
            ensureCursorVisible();
            hexHit = std::make_pair(*hit, pattern.size());
        } else {
            topLine = static_cast<int>(LineIndexForByte(fileIndex.lineStarts, *hit));
            clampTop();
        }
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
            if (hexMode) ensureCursorVisible();
            clampTop();
            paint();
            continue;
        }
        if (record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) continue;

        hexHit.reset();
        WORD key = record.Event.KeyEvent.wVirtualKeyCode;
        wchar_t ch = record.Event.KeyEvent.uChar.UnicodeChar;
        switch (key) {
            case VK_UP:
                if (hexMode) {
                    if (hexCursor >= hexItems()) hexCursor -= hexItems();
                    ensureCursorVisible();
                } else {
                    --topLine;
                    clampTop();
                }
                paint();
                break;
            case VK_DOWN:
                if (hexMode) {
                    hexCursor = std::min(hexCursor + hexItems(), lastByte);
                    ensureCursorVisible();
                } else {
                    ++topLine;
                    clampTop();
                }
                paint();
                break;
            case VK_PRIOR: {
                if (hexMode) {
                    uint64_t step = static_cast<uint64_t>(contentHeight()) * hexItems();
                    hexCursor = hexCursor >= step ? hexCursor - step : hexCursor % hexItems();
                    ensureCursorVisible();
                } else {
                    topLine -= contentHeight();
                    clampTop();
                }
                paint();
                break;
            }
            case VK_NEXT: {
                if (hexMode) {
                    uint64_t step = static_cast<uint64_t>(contentHeight()) * hexItems();
                    hexCursor = std::min(hexCursor + step, lastByte);
                    ensureCursorVisible();
                } else {
                    topLine += contentHeight();
                    clampTop();
                }
                paint();
                break;
            }
            case VK_HOME:
                if (hexMode) {
                    hexCursor = 0;
                    ensureCursorVisible();
                } else {
                    topLine = 0;
                }
                paint();
                break;
            case VK_END:
                if (hexMode) {
                    hexCursor = lastByte;
                    ensureCursorVisible();
                } else {
                    topLine = std::max<int>(0, static_cast<int>(lineCount) - contentHeight());
                }
                paint();
                break;
            case VK_LEFT:
                if (hexMode) {
                    if (hexCursor > 0) --hexCursor;
                    ensureCursorVisible();
                    paint();
                }
                break;
            case VK_RIGHT:
                if (hexMode) {
                    if (hexCursor < lastByte) ++hexCursor;
                    ensureCursorVisible();
                    paint();
                }
                break;
            case VK_ESCAPE:
            case VK_F3:
            case VK_F10:
                running = false;
                break;
            default:
                if (ch == L'/') {
                    auto term = ShowTextPrompt(console, kStrSearchPromptTitle, searchTerm, renderFrame);
                    if (term && !term->empty()) {
                        searchTerm = *term;
                        findNext();
                    }
                    paint();
                } else if (ch == L'n' || ch == L'N') {
                    findNext();
                    paint();
                } else if (ch == L'h' || ch == L'H') {
                    if (hexMode) {
                        topLineFromCursor();
                        hexMode = false;
                    } else {
                        enterHexFromTopLine();
                        hexMode = true;
                    }
                    paint();
                } else if ((ch == L'w' || ch == L'W') && hexMode) {
                    hexSetting = NextHexItemSetting(hexSetting);
                    hexPan = 0;
                    hexAnchor = HexRowStart(hexCursor, hexItems());
                    ensureCursorVisible();
                    paint();
                }
                break;
        }
    }
}

} // namespace mc
