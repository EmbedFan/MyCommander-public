#include "FolderWatch.h"

#include "PathUtil.h"

namespace mc {

bool FolderWatch::Arm(const std::wstring& folder) {
    Close();
    folder_ = folder;
    if (folder.empty()) {
        state_ = State::Unavailable;
        return false;
    }
    handle_ = FindFirstChangeNotificationW(
        ToLongPath(std::filesystem::path(folder)).c_str(), FALSE,
        FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_ATTRIBUTES |
            FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE);
    if (handle_ == INVALID_HANDLE_VALUE) {
        state_ = State::Unavailable;
        return false;
    }
    state_ = State::Active;
    return true;
}

void FolderWatch::Close() {
    if (handle_ != INVALID_HANDLE_VALUE) {
        FindCloseChangeNotification(handle_);
        handle_ = INVALID_HANDLE_VALUE;
    }
    state_ = State::Closed;
    folder_.clear();
}

bool FolderWatch::Poll() {
    if (state_ != State::Active) return false;
    DWORD wait = WaitForSingleObject(handle_, 0);
    if (wait == WAIT_TIMEOUT) return false;
    if (wait != WAIT_OBJECT_0 || !FindNextChangeNotification(handle_)) {
        // The folder or its volume went away: stop watching, but report once.
        FindCloseChangeNotification(handle_);
        handle_ = INVALID_HANDLE_VALUE;
        state_ = State::Unavailable;
        return true;
    }
    return true;
}

void FolderWatch::Drain() {
    while (Poll() && state_ == State::Active) {
    }
}

}  // namespace mc
