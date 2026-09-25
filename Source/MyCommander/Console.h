#pragma once

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>
#include <string>
#include <vector>

namespace mc {

// Double-buffered console screen writer built on WriteConsoleOutputW.
// Owns a private screen buffer so drawing never tears the visible frame.
class Console {
public:
    Console();
    ~Console();

    Console(const Console&) = delete;
    Console& operator=(const Console&) = delete;

    // Re-reads the OS console buffer size. Call after a resize event.
    void UpdateSize();

    SHORT Width() const { return width_; }
    SHORT Height() const { return height_; }

    // UI-014/ACC-006: box-drawing frames are the normal presentation. The
    // user can choose plain 7-bit ASCII for a terminal without those glyphs.
    void SetUseBasicSymbols(bool useBasicSymbols) { useBasicSymbols_ = useBasicSymbols; }
    bool UseBasicSymbols() const { return useBasicSymbols_; }

    void Clear(WORD attributes);
    void PutText(SHORT x, SHORT y, const std::wstring& text, WORD attributes);
    void FillRow(SHORT y, SHORT x, SHORT count, WCHAR ch, WORD attributes);

    // Blits the back buffer to the real console in one call.
    void Present();

    HANDLE InputHandle() const { return input_; }

    // Hands the console back to its normal state so a spawned child process
    // (an external editor, a future shell launch) can use it exactly like
    // any other console app: a well-behaved full-screen console program
    // (vim, nano, ...) creates and activates its own screen buffer, so our
    // buffer must stop being the active one first, and raw input mode must
    // give way to whatever mode the child expects. Call
    // ResumeAfterChildProcess() once the child exits to restore both.
    void SuspendForChildProcess();
    void ResumeAfterChildProcess();

private:
    HANDLE output_ = INVALID_HANDLE_VALUE;
    HANDLE input_ = INVALID_HANDLE_VALUE;
    HANDLE originalOutput_ = INVALID_HANDLE_VALUE;
    DWORD originalOutputMode_ = 0;
    DWORD originalInputMode_ = 0;
    SHORT width_ = 0;
    SHORT height_ = 0;
    bool useBasicSymbols_ = false;
    std::vector<CHAR_INFO> buffer_;
};

} // namespace mc
