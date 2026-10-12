#pragma once

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include <filesystem>
#include <string>

namespace mc {

// FEA-0003: true if `extension` (with or without a leading dot, matched
// case-insensitively) names a PE file type that can carry an embedded
// VERSIONINFO resource — exe/dll/ocx/sys/cpl/scr/drv, the same set Windows
// itself treats as "this kind of file might have version info" (Explorer's
// own Properties dialog shows a "Details" tab for exactly these). Pure —
// no filesystem access, so it's directly unit-testable.
bool IsRunnableExtension(const std::wstring& extension);

// FEA-0003: the StringFileInfo fields a runnable file's VERSIONINFO
// resource may carry — the same block Help.cpp's own About section
// (KEY-004) already reads for the running exe's own path, generalized
// here to an arbitrary file. `available` is false (every field left
// empty) if `path` has no VERSIONINFO resource at all, or any Win32 call
// along the way fails — never a crash, matching Help.cpp's own
// established graceful-degradation precedent for this exact API.
struct FileVersionFields {
    bool available = false;
    std::wstring description;
    std::wstring fileVersion;
    std::wstring productName;
    std::wstring productVersion;
    std::wstring copyright;
};

FileVersionFields ReadFileVersionFields(const std::filesystem::path& path);

// FEA-0003: a best-effort Authenticode check — presence/absence and a
// display name only, not a full trust verdict (no revocation check, no
// certificate-chain validation UI). `CheckFailed` means `path` itself
// couldn't be read (e.g. deleted between the panel listing and the
// keypress) and is distinguished from `NotSigned` so "not signed" is never
// misreported for a file this couldn't actually check. When `Signed`,
// `signerName` may still be empty if the certificate's subject name
// couldn't be extracted — the file is genuinely signed either way.
enum class SignatureStatus { NotSigned, Signed, CheckFailed };

struct SignatureInfo {
    SignatureStatus status = SignatureStatus::NotSigned;
    std::wstring signerName;  // meaningful only when status == Signed
};

SignatureInfo CheckDigitalSignature(const std::filesystem::path& path);

}  // namespace mc
