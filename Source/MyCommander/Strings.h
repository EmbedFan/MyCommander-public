#pragma once

// ACC-003: the single place every piece of user-facing prose the app shows
// (dialog titles/prompts/labels, status/error messages, viewer/help chrome)
// is defined, kept apart from the control-flow logic that decides *when* to
// show it — so a future localizer can find and replace copy here without
// reading or touching program logic. Matches the same "one header owns the
// display copy" shape this codebase already used successfully for
// `HintBar.h` (a context -> hint-line table) and `Theme.h` (color
// constants) — this header does the equivalent job for everything else.
//
// What's deliberately NOT here (and why), so this isn't read as an
// incomplete sweep:
//  - `HintBar.h`'s per-context hint lines and `Help.cpp`'s `Sections()`
//    keyboard-shortcut table: both are already self-contained data tables,
//    fully separate from logic, in their own right — moving their entries
//    here would fragment two already-correct, easy-to-audit tables instead
//    of consolidating anything.
//  - Config file keys/values (`Config.cpp`, e.g. "confirmrecyclebindelete",
//    "true"/"false", "view"/"edit"/"execute"), the session file's own
//    format (`Session.cpp`), and the elevated-helper's `--elevated-*`
//    command-line flags (`Elevation.cpp`): these are data-format/protocol
//    identifiers a user (or this app's own re-invocation of itself) reads
//    and writes verbatim, not prose a reader interprets in their language —
//    same category as a JSON key name.
//  - Diagnostic log content (`Log.cpp`'s `LogLevelToString`, every
//    `logger.Log(...)` call site): the log file is a technical/support
//    artifact, not interactive UI a user reads to operate the app;
//    conventionally kept in English/technical form regardless of UI
//    language, the same way a stack trace or error code is.
//  - Win32 API parameters that happen to be string literals (ShellExecute's
//    `"open"`/`"runas"` verbs, `%EDITOR%`/`%COMSPEC%` environment variable
//    names, `notepad.exe`/`cmd.exe` fallback executable names): these are
//    technical identifiers the OS interprets, not language-dependent text.
//
// Runtime locale switching (actually loading a non-English catalog) is
// explicitly out of scope here — that's ACC-005 (LATER), which this
// separation exists to make possible, not something this change needs to
// build yet.

#include <cstdint>
#include <string>

// --- Dialog.cpp -----------------------------------------------------------

constexpr const wchar_t* kStrPressAnyKeyToContinue = L"Press any key to continue";

// --- Viewer.cpp -------------------------------------------------------------

constexpr const wchar_t* kStrViewerEmptyFile = L"(empty file)";
constexpr const wchar_t* kStrViewerLinePrefix = L"Line ";
constexpr const wchar_t* kStrViewerHintLine = L"  Esc/F3 Close  /: Search  n: Next";
constexpr const wchar_t* kStrSearchTitle = L"Search";

inline std::wstring FormatSearchTermNotFound(const std::wstring& term) {
    return L"\"" + term + L"\" not found";
}

// --- Help.cpp: the fixed header/status chrome around the Sections() table
// (the table itself is its own already-separated data, see the note above).

constexpr const wchar_t* kStrShortcutReferenceTitle = L"MyCommander \x2014 Keyboard Shortcuts (KEY-004)";
constexpr const wchar_t* kStrShortcutReferenceHintSuffix = L"  Esc/F1/F10 Close";

// --- main.cpp: formatting/labels ------------------------------------------

constexpr const wchar_t* kStrUnknown = L"Unknown";
constexpr const wchar_t* kStrDriveTypeRemovable = L"Removable";
constexpr const wchar_t* kStrDriveTypeNetwork = L"Network";
constexpr const wchar_t* kStrDriveTypeOptical = L"Optical";
constexpr const wchar_t* kStrDriveTypeRamDisk = L"RAM Disk";
constexpr const wchar_t* kStrDriveTypeFixed = L"Fixed";
constexpr const wchar_t* kStrDriveTypeLocalDisk = L"Local Disk";
constexpr const wchar_t* kStrDriveTypeCdDvd = L"CD/DVD";

constexpr const wchar_t* kStrSortName = L"Name";
constexpr const wchar_t* kStrSortExtension = L"Ext";
constexpr const wchar_t* kStrSortSize = L"Size";
constexpr const wchar_t* kStrSortDate = L"Date";

constexpr const wchar_t* kStrVolumeInfoUnavailable = L"Volume information unavailable";
constexpr const wchar_t* kStrVolumeSpaceUnavailable = L"  space unavailable";
constexpr const wchar_t* kStrVolumeFreeOf = L" free of ";
constexpr const wchar_t* kStrVolumeFreeSlash = L" free / ";

constexpr const wchar_t* kStrHiddenAndSystemTag = L"  [Hidden+system]";
constexpr const wchar_t* kStrLoadingTag = L"  [Loading...]";

// UI-004/NAV-009/LIST-006 type/size-column tags. Bare (unpadded) — UI-005's
// configurable column widths mean main.cpp right-aligns these to the
// currently-configured size-column width itself (RightAlignAscii), rather
// than baking a fixed width into the literal the way these used to.
constexpr const wchar_t* kStrTypeLink = L"LINK";
constexpr const wchar_t* kStrSizeColBroken = L"<BROKEN>";
constexpr const wchar_t* kStrSizeColReparse = L"<REPARSE>";
constexpr const wchar_t* kStrSizeColDir = L"<DIR>";

constexpr const wchar_t* kStrTerminalTooSmall = L"Terminal window too small";

inline std::wstring FormatMinimumSize(int minWidth, int minHeight, int currentWidth, int currentHeight) {
    wchar_t buf[80];
    swprintf_s(buf, L"Minimum size: %dx%d  (current: %dx%d)", minWidth, minHeight, currentWidth, currentHeight);
    return buf;
}

// --- main.cpp: progress dialog (ProgressUi) --------------------------------

constexpr const wchar_t* kStrProgressLocationLabel = L"Location: ";
constexpr const wchar_t* kStrProgressSourceLabel = L"Source: ";
constexpr const wchar_t* kStrProgressDestinationLabel = L"Destination: ";
constexpr const wchar_t* kStrEscToCancel = L"Esc to cancel";
constexpr const wchar_t* kStrVerbMove = L"Move";
constexpr const wchar_t* kStrVerbCopy = L"Copy";
constexpr const wchar_t* kStrVerbMoving = L"Moving";
constexpr const wchar_t* kStrVerbCopying = L"Copying";
constexpr const wchar_t* kStrVerbDeleting = L"Deleting";
constexpr const wchar_t* kStrVerbSettingAttributes = L"Setting attributes";
constexpr const wchar_t* kStrVerbDelete = L"Delete";
constexpr const wchar_t* kStrVerbToggleAttribute = L"Toggle attribute";

inline std::wstring FormatProgressItemLine(int doneCount1Based, int total, const std::wstring& currentName) {
    return L"Item " + std::to_wstring(doneCount1Based) + L" of " + std::to_wstring(total) + L": " + currentName;
}

// --- main.cpp: conflict/error dialogs --------------------------------------

constexpr const wchar_t* kStrFileExistsTitle = L"File exists";
constexpr const wchar_t* kStrAlreadyExistsAt = L"\" already exists at:";
constexpr const wchar_t* kStrOverwriteLabel = L"verwrite";
constexpr const wchar_t* kStrOverwriteAllLabel = L"ll (overwrite)";
constexpr const wchar_t* kStrSkipLabel = L"kip";
constexpr const wchar_t* kStrSkipAllLabel = L"ip all";
constexpr const wchar_t* kStrRenameLabel = L"ename";
constexpr const wchar_t* kStrCancelLabel = L"ancel";
constexpr const wchar_t* kStrRenameToTitle = L"Rename to";

constexpr const wchar_t* kStrOperationFailedTitle = L"Operation failed";
constexpr const wchar_t* kStrRetryLabel = L"etry";
constexpr const wchar_t* kStrElevateAndRetryLabel = L"levate & Retry";

inline std::wstring FormatOperationResultTitle(const std::wstring& verb) { return verb + L" result"; }
inline std::wstring FormatOperationResultHeader(int succeeded, int skipped, size_t failedCount, bool cancelled) {
    std::wstring header = std::to_wstring(succeeded) + L" succeeded, " + std::to_wstring(skipped) + L" skipped, " +
                          std::to_wstring(failedCount) + L" failed";
    if (cancelled) header += L" (cancelled)";
    return header;
}
inline std::wstring FormatMoreFailures(size_t remaining) { return L"... and " + std::to_wstring(remaining) + L" more"; }

// --- main.cpp: filter/select/search prompts ---------------------------------

constexpr const wchar_t* kStrFilterPrompt = L"Filter (wildcard, empty = show all)";
constexpr const wchar_t* kStrSelectByMaskPrompt = L"Select by mask (wildcard)";
constexpr const wchar_t* kStrDeselectByMaskPrompt = L"Deselect by mask (wildcard)";
constexpr const wchar_t* kStrFindFilesPrompt = L"Find files (name/wildcard)";
constexpr const wchar_t* kStrFindFilesTitle = L"Find files";
constexpr const wchar_t* kStrSearchCancelled = L"Search cancelled.";
constexpr const wchar_t* kStrNoMatchesFound = L"No matches found.";
constexpr const wchar_t* kStrSearchResultsHint = L" Enter: go to  F3: view  Esc/F10: close";

inline std::wstring FormatSearchingHeader(const std::wstring& path, const std::wstring& pattern) {
    return L"Searching \"" + path + L"\" for \"" + pattern + L"\"... Esc to cancel";
}
inline std::wstring FormatSearchResultsHeader(size_t matchCount, bool truncated, const std::wstring& pattern) {
    return std::to_wstring(matchCount) + L" match(es)" + (truncated ? L" (truncated)" : L"") + L" for \"" + pattern +
          L"\"";
}

// --- main.cpp: drive selection ---------------------------------------------

constexpr const wchar_t* kStrSelectDriveTitle = L"Select drive";
constexpr const wchar_t* kStrNoDrivesFound = L"No drives found.";
constexpr const wchar_t* kStrSelectDriveHint = L" Enter: select  Esc/F10: cancel";

inline std::wstring FormatSelectDriveForPanelHeader(bool leftPanel) {
    return std::wstring(L"Select drive for ") + (leftPanel ? L"left" : L"right") + L" panel";
}

// --- main.cpp: rename / create directory / create file ---------------------

constexpr const wchar_t* kStrRenameTitle = L"Rename";
constexpr const wchar_t* kStrRenameFailedTitle = L"Rename failed";
constexpr const wchar_t* kStrNewFolderNamePrompt = L"New folder name";
constexpr const wchar_t* kStrCreateFolderFailedTitle = L"Create folder failed";
constexpr const wchar_t* kStrNewFileNamePrompt = L"New file name";
constexpr const wchar_t* kStrCreateFileFailedTitle = L"Create file failed";

// --- main.cpp: view / edit / execute / shell --------------------------------

constexpr const wchar_t* kStrCannotStartEditorTitle = L"Cannot start editor";
constexpr const wchar_t* kStrEditorLabel = L"Editor: ";
constexpr const wchar_t* kStrCannotOpenFileTitle = L"Cannot open file";
constexpr const wchar_t* kStrCannotStartShellTitle = L"Cannot start shell";
constexpr const wchar_t* kStrCannotRunCommandTitle = L"Cannot run command";
constexpr const wchar_t* kStrShellLabel = L"Shell: ";

// --- main.cpp: copy/move destination prompt ---------------------------------

constexpr const wchar_t* kStrCreateDestinationTitle = L"Create destination?";
constexpr const wchar_t* kStrDoesNotExistSuffix = L"\" does not exist.";
constexpr const wchar_t* kStrYesCreateItLabel = L"es, create it";
constexpr const wchar_t* kStrNoCancelLabel = L"o, cancel";
constexpr const wchar_t* kStrCannotCreateDestinationTitle = L"Cannot create destination";
constexpr const wchar_t* kStrIsNotADirectory = L"is not a directory.";

inline std::wstring FormatItemsToDestinationHeader(const std::wstring& verb, size_t itemCount) {
    return verb + L" " + std::to_wstring(itemCount) + L" item(s) to:";
}
inline std::wstring FormatCannotVerbTitle(const std::wstring& verb) { return L"Cannot " + verb; }

// --- main.cpp: delete confirmation ------------------------------------------

constexpr const wchar_t* kStrPermanentlyDeleteTitle = L"Permanently delete?";
constexpr const wchar_t* kStrDeleteTitle = L"Delete?";
constexpr const wchar_t* kStrWillBePermanentlyDeleted = L"will be PERMANENTLY deleted. This cannot be undone.";
constexpr const wchar_t* kStrWillBeSentToRecycleBin = L"will be sent to the Recycle Bin.";
constexpr const wchar_t* kStrYesLabel = L"es";
constexpr const wchar_t* kStrNoLabel = L"o";

inline std::wstring FormatItemCount(size_t count) { return std::to_wstring(count) + L" items"; }

// --- main.cpp: attribute toggle ----------------------------------------------

constexpr const wchar_t* kStrEachItemsCurrentStateFlipped = L"Each item's current state is flipped.";
constexpr const wchar_t* kStrReadonlyLabel = L"eadonly";
constexpr const wchar_t* kStrHiddenLabel = L"idden";
constexpr const wchar_t* kStrSystemLabel = L"ystem";
constexpr const wchar_t* kStrArchiveLabel = L"rchive";

inline std::wstring FormatToggleAttributeOnHeader(const std::wstring& itemLabel) {
    return L"Toggle attribute on " + itemLabel;
}

// --- main.cpp: startup configuration warning --------------------------------

constexpr const wchar_t* kStrConfigurationWarningTitle = L"Configuration warning";
constexpr const wchar_t* kStrUsingDefaultForSettings = L"Using the default for the following setting(s):";

// --- main.cpp: drag-and-drop (IS-0003) ---------------------------------------

inline std::wstring FormatDragHint(size_t itemCount) {
    return L" Dragging " + std::to_wstring(itemCount) + L" item(s) \x2014 release to copy (hold Shift to move)";
}

// --- Panel.cpp: status-bar messages (LIST-006) ------------------------------

constexpr const wchar_t* kStrCannotListDirectory = L"Cannot list directory: ";
constexpr const wchar_t* kStrCannotEnterReparsePoint = L"Cannot enter reparse point: ";
constexpr const wchar_t* kStrCannotEnter = L"Cannot enter: ";

// --- Config.cpp: config-file parse warnings (shown via the dialog above) ---
// The `expected`/key/value tokens interpolated into these (e.g. "true/false",
// "confirmrecyclebindelete") are config-file syntax, not prose — same
// exclusion as the rest of Config.cpp's literals (see the top-of-file note).

inline std::wstring FormatConfigLineSyntaxWarning(int lineNumber, const std::wstring& rawLine) {
    return L"Line " + std::to_wstring(lineNumber) + L": expected \"key = value\", got: " + rawLine;
}
inline std::wstring FormatConfigInvalidValueWarning(int lineNumber, const std::wstring& key, const std::wstring& value,
                                                    const std::wstring& expected) {
    return L"Line " + std::to_wstring(lineNumber) + L": invalid value for \"" + key + L"\": \"" + value +
          L"\" (expected " + expected + L") \x2014 using the default instead";
}

// --- FileOps.cpp: validation and error messages -----------------------------
// The path/system-message portions interpolated into these stay as
// parameters — only the fixed English wording lives here.

constexpr const wchar_t* kStrInvalidName = L"invalid name";
constexpr const wchar_t* kStrNameReservedCharacter = L"name contains a character reserved by Windows (\\ / : * ? \" < > |)";
constexpr const wchar_t* kStrRefusingReparseCopy = L"refusing to copy the contents of a reparse point/junction";
constexpr const wchar_t* kStrCannotPlaceDirectoryInsideItself = L"cannot place a directory inside itself";
constexpr const wchar_t* kStrCannotOverwriteFileWithDirectory = L"cannot overwrite an existing file with a directory";
constexpr const wchar_t* kStrCannotOverwriteDirectoryWithFile = L"cannot overwrite an existing directory with a file";
constexpr const wchar_t* kStrSourceAndDestinationSame = L"source and destination are the same directory";
constexpr const wchar_t* kStrCannotOverwriteDirectoryByMovingIntoIt = L"cannot overwrite an existing directory by moving into it";

inline std::wstring FormatCannotCreateDirectory(const std::wstring& path, const std::wstring& sysMessage) {
    return L"cannot create directory \"" + path + L"\": " + sysMessage;
}
inline std::wstring FormatCannotListDirectory(const std::wstring& path, const std::wstring& sysMessage) {
    return L"cannot list directory \"" + path + L"\": " + sysMessage;
}
inline std::wstring FormatItemAlreadyExists(const std::wstring& name) {
    return L"an item named \"" + name + L"\" already exists";
}
inline std::wstring FormatCannotRename(const std::wstring& oldName, const std::wstring& newName,
                                       const std::wstring& sysMessage) {
    return L"cannot rename \"" + oldName + L"\" to \"" + newName + L"\": " + sysMessage;
}
inline std::wstring FormatCannotCreate(const std::wstring& path, const std::wstring& sysMessage) {
    return L"cannot create \"" + path + L"\": " + sysMessage;
}
inline std::wstring FormatCannotDelete(const std::wstring& path, const std::wstring& sysMessage) {
    return L"cannot delete \"" + path + L"\": " + sysMessage;
}
inline std::wstring FormatRecycleBinDeleteFailed(int resultCode) {
    return L"Recycle Bin delete failed (code " + std::to_wstring(resultCode) + L")";
}
inline std::wstring FormatCopiedButCouldNotRemoveSource(const std::wstring& finalDst, const std::wstring& src,
                                                        const std::wstring& sysMessage) {
    return L"copied to \"" + finalDst + L"\", but could not remove source \"" + src + L"\": " + sysMessage;
}
