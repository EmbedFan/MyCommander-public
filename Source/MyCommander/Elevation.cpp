#include "Elevation.h"

#include "PathUtil.h"
#include "Process.h"

#include <shellapi.h>

namespace mc {

namespace {

constexpr wchar_t kCopyFlag[] = L"--elevated-copy";
constexpr wchar_t kMoveFlag[] = L"--elevated-move";
constexpr wchar_t kDeleteFlag[] = L"--elevated-delete";
constexpr wchar_t kAttribFlag[] = L"--elevated-attrib";
constexpr wchar_t kOverwriteFlag[] = L"--overwrite";

const wchar_t* AttributeFlagName(FileAttributeFlag flag) {
    switch (flag) {
        case FileAttributeFlag::ReadOnly:
            return L"readonly";
        case FileAttributeFlag::Hidden:
            return L"hidden";
        case FileAttributeFlag::System:
            return L"system";
        case FileAttributeFlag::Archive:
            return L"archive";
    }
    return L"readonly";
}

bool ParseAttributeFlagName(const std::wstring& s, FileAttributeFlag& out) {
    if (s == L"readonly") {
        out = FileAttributeFlag::ReadOnly;
        return true;
    }
    if (s == L"hidden") {
        out = FileAttributeFlag::Hidden;
        return true;
    }
    if (s == L"system") {
        out = FileAttributeFlag::System;
        return true;
    }
    if (s == L"archive") {
        out = FileAttributeFlag::Archive;
        return true;
    }
    return false;
}

}  // namespace

std::wstring BuildElevatedHelperParameters(const ElevatedRequest& request) {
    std::wstring parameters;
    auto append = [&](const std::wstring& arg) {
        if (!parameters.empty()) parameters += L' ';
        parameters += QuoteCommandLineArgument(arg);
    };

    switch (request.action) {
        case ElevatedAction::Copy:
            append(kCopyFlag);
            append(request.source.wstring());
            append(request.destination.wstring());
            break;
        case ElevatedAction::Move:
            append(kMoveFlag);
            append(request.source.wstring());
            append(request.destination.wstring());
            if (request.overwrite) append(kOverwriteFlag);
            break;
        case ElevatedAction::DeletePermanent:
            append(kDeleteFlag);
            append(request.source.wstring());
            break;
        case ElevatedAction::ToggleAttribute:
            append(kAttribFlag);
            append(request.source.wstring());
            append(AttributeFlagName(request.attributeFlag));
            break;
    }
    return parameters;
}

DWORD RunElevated(const ElevatedRequest& request) {
    wchar_t modulePath[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, modulePath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return ERROR_MOD_NOT_FOUND;

    std::wstring parameters = BuildElevatedHelperParameters(request);

    SHELLEXECUTEINFOW sei{};
    sei.cbSize = sizeof(sei);
    sei.fMask = SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC;
    sei.lpVerb = L"runas";
    sei.lpFile = modulePath;
    sei.lpParameters = parameters.c_str();
    sei.nShow = SW_HIDE;

    if (!ShellExecuteExW(&sei) || sei.hProcess == nullptr) {
        DWORD err = GetLastError();
        // ShellExecuteExW reports a UAC decline as ERROR_CANCELLED itself in
        // most cases; if it somehow reports success (0) without a process
        // handle, treat that the same way rather than claiming ERROR_SUCCESS.
        return err == 0 ? static_cast<DWORD>(ERROR_CANCELLED) : err;
    }

    WaitForSingleObject(sei.hProcess, INFINITE);
    DWORD exitCode = ERROR_GEN_FAILURE;
    GetExitCodeProcess(sei.hProcess, &exitCode);
    CloseHandle(sei.hProcess);
    return exitCode;
}

std::optional<DWORD> RunAsElevatedHelperIfRequested(int argc, wchar_t** argv) {
    if (argc < 2) return std::nullopt;
    std::wstring flag = argv[1];

    std::wstring err;      // discarded — the parent side only ever sees the exit code
    DWORD win32Error = 0;  // reported back as this process's own exit code below
    bool ok = false;

    if (flag == kCopyFlag) {
        if (argc < 4) return static_cast<DWORD>(ERROR_INVALID_PARAMETER);
        ok = CopyPathElevated(argv[2], argv[3], err, win32Error);
    } else if (flag == kMoveFlag) {
        if (argc < 4) return static_cast<DWORD>(ERROR_INVALID_PARAMETER);
        bool overwrite = argc >= 5 && std::wstring(argv[4]) == kOverwriteFlag;
        ok = MovePathElevated(argv[2], argv[3], overwrite, err, win32Error);
    } else if (flag == kDeleteFlag) {
        if (argc < 3) return static_cast<DWORD>(ERROR_INVALID_PARAMETER);
        ok = DeletePathElevated(argv[2], err, win32Error);
    } else if (flag == kAttribFlag) {
        if (argc < 4) return static_cast<DWORD>(ERROR_INVALID_PARAMETER);
        FileAttributeFlag attrFlag;
        if (!ParseAttributeFlagName(argv[3], attrFlag)) return static_cast<DWORD>(ERROR_INVALID_PARAMETER);
        ok = ToggleAttributeElevated(argv[2], attrFlag, err, win32Error);
    } else {
        return std::nullopt;  // not an elevated-helper invocation at all
    }

    if (ok) return static_cast<DWORD>(ERROR_SUCCESS);
    return win32Error != 0 ? win32Error : static_cast<DWORD>(ERROR_GEN_FAILURE);
}

}  // namespace mc
