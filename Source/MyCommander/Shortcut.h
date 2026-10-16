#pragma once

// FOP-026: Windows shortcut (.lnk) files. This is the one home of the IShellLinkW/IPersistFile code; the
// installer's desktop shortcut (INST-005) calls it too. Deliberately no Console/Dialog dependency.

#include <filesystem>
#include <functional>
#include <string>

namespace mc {

// Writes a .lnk at `linkPath` pointing at `targetPath`. `workingDir` and `description` may be empty (then
// not set). The shortcut shows the shell's default icon for its target. Initializes COM for the call if the
// thread has not. Returns an empty string on success, otherwise a message (never throws). An existing file
// at `linkPath` is replaced -- callers choose a free name first (ShortcutNameFor).
std::wstring CreateShortcutFile(const std::filesystem::path& linkPath, const std::filesystem::path& targetPath,
                                const std::filesystem::path& workingDir, const std::wstring& description);

// "<name> - Shortcut.lnk", or "<name> - Shortcut (2).lnk", "(3)" ... while `exists(candidate)` says the name is
// taken (the caller's predicate, case-insensitive on disk). Empty when the result would exceed 255 characters
// or `name` is empty.
std::wstring ShortcutNameFor(const std::wstring& name, const std::function<bool(const std::wstring&)>& exists);

}  // namespace mc
