#pragma once

#include <filesystem>
#include <optional>

namespace mc {

// NAV-008/ERR-006: this deliberately contains only cosmetic panel locations.
// It must never grow operation, selection, command, or other actionable state.
struct SessionState {
    std::optional<std::filesystem::path> leftPath;
    std::optional<std::filesystem::path> rightPath;
};

// Separate from Config: the session file is read only after a previous clean
// exit and is written only when this process exits through its normal loop.
std::filesystem::path SessionFilePath();
SessionState LoadSessionFrom(const std::filesystem::path& path);
SessionState LoadSession();

// Writes both locations atomically where possible. Failure is intentionally
// silent to callers: session restoration is a convenience, never a reason to
// prevent a normal exit.
bool SaveSessionTo(const std::filesystem::path& path, const SessionState& state);
bool SaveSession(const SessionState& state);

}  // namespace mc
