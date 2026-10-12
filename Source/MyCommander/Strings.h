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
constexpr const wchar_t* kStrFormDialogHint =
    L"Up/Down field, Left/Right change, [Enter] OK, [Esc] Cancel";
constexpr const wchar_t* kStrListDialogHint = L"Up/Down choose, [Enter] OK, [Esc] Cancel";

// --- Viewer.cpp -------------------------------------------------------------

constexpr const wchar_t* kStrViewerEmptyFile = L"(empty file)";
constexpr const wchar_t* kStrViewerLinePrefix = L"Line ";
constexpr const wchar_t* kStrViewerHintLine = L"  Esc/F3 Close  /: Search  n: Next  H: Hex";
constexpr const wchar_t* kStrViewerHexHintLine =
    L"  Esc/F3 Close  H: Text  W: Items/row  Arrows: Move cursor  /: Search  n: Next";
constexpr const wchar_t* kStrSearchTitle = L"Search";
constexpr const wchar_t* kStrSearchPromptTitle = L"Find: text, or #hex bytes (e.g. #4D 5A)";
constexpr const wchar_t* kStrViewerInvalidHex = L"Invalid hex pattern. Use pairs of hex digits, e.g. #4D 5A";

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

// --- main.cpp: background job queue (FOP-014, Phase 3) ----------------------

constexpr const wchar_t* kStrJobsTitle = L"Job queue";
constexpr const wchar_t* kStrJobsEmpty = L"No background jobs.";
constexpr const wchar_t* kStrJobsCancelQuestion = L"Cancel this job?";
constexpr const wchar_t* kStrJobFailedGeneric = L"The background job failed.";
constexpr const wchar_t* kStrQueuePluginRefused = L"Background copy/move is not available for plugin panels.";
constexpr const wchar_t* kStrQueueBadDestination = L"The destination folder does not exist.";
constexpr const wchar_t* kStrQueueDeletePluginRefused = L"Background delete is not available for plugin panels.";
constexpr const wchar_t* kStrInTheBackgroundSuffix = L" (in the background)";
constexpr const wchar_t* kStrJobBusyTitle = L"Files in use by a background job";
constexpr const wchar_t* kStrJobBusyAdvice = L"Wait for it to finish, or cancel it with Alt+Q.";
constexpr const wchar_t* kStrJobBusyIntro = L"A background job is using these files:";
constexpr const wchar_t* kStrQuitJobsTitle = L"Background jobs";
constexpr const wchar_t* kStrQuitWait = L"Wait until they finish, then quit";
constexpr const wchar_t* kStrQuitCancel = L"Cancel them and quit";

inline std::wstring FormatJobsPending(size_t count) {
    return std::to_wstring(count) + L" background job(s) still running or queued.";
}

// --- main.cpp: DirectoryTreeContent (UI-013, Part B) -------------------------

constexpr const wchar_t* kStrTreeContentTitle = L"Directory Tree";

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
// SRC-004/005: content search.
constexpr const wchar_t* kStrNoFilesContain = L"No files contain the text.";
inline std::wstring FormatContentSearchProgress(const std::wstring& path, const std::wstring& text, size_t files,
                                                const std::wstring& current) {
    return L"Searching \"" + path + L"\" for \"" + text + L"\"... " + std::to_wstring(files) +
           L" files searched, Esc to cancel \x2014 " + current;
}
inline std::wstring FormatContentResultsHeader(size_t matchCount, bool truncated, const std::wstring& text,
                                               size_t unreadable, size_t binarySkipped) {
    std::wstring h = std::to_wstring(matchCount) + L" file(s) containing \"" + text + L"\"" +
                     (truncated ? L" (truncated)" : L"");
    if (unreadable > 0) h += L", " + std::to_wstring(unreadable) + L" unreadable skipped";
    if (binarySkipped > 0) h += L", " + std::to_wstring(binarySkipped) + L" binary skipped";
    return h;
}

// --- main.cpp: compare folders (SRC-006) ------------------------------------

constexpr const wchar_t* kStrCompareTitle = L"Compare folders";
constexpr const wchar_t* kStrCompareSameFolder = L"Both panels show the same folder.";
constexpr const wchar_t* kStrCompareNeedsFolders =
    L"Both panels must show a folder (not an archive or the tree view).";
constexpr const wchar_t* kStrCompareCannotList = L"This folder can't be listed:";
constexpr const wchar_t* kStrCompareCancelledNothing = L"Compare cancelled.";
constexpr const wchar_t* kStrCompareResultsHint =
    L" Tab: all/differences/left/right  Enter: go to  F3/Shift+F3: view left/right  Alt+F3: differences  "
    L"F5: synchronize  Esc: close";
inline std::wstring FormatCompareProgress(size_t items, const std::wstring& bytes, const std::wstring& current) {
    return L"Comparing... " + std::to_wstring(items) + L" items, " + bytes + L" read, Esc to cancel \x2014 " + current;
}
// --- main.cpp: key bindings (KEY-005) ---------------------------------------

constexpr const wchar_t* kStrKeyBindingsWarningTitle = L"Key bindings";
constexpr const wchar_t* kStrKeyBindingsWarningIntro =
    L"Some lines in keys.conf were not applied (everything else was):";
constexpr const wchar_t* kStrCannotCreateKeysFile = L"Could not create the key bindings file:";

// --- main.cpp: synchronize folders (SRC-007) --------------------------------

constexpr const wchar_t* kStrSyncTitle = L"Synchronize folders";
constexpr const wchar_t* kStrSyncConfirmTitle = L"Synchronize folders?";
constexpr const wchar_t* kStrSyncNothing = L"Nothing to synchronize \x2014 the folders match.";
constexpr const wchar_t* kStrSyncTruncated =
    L"Too many items to synchronize safely. Narrow it with a filter (Ctrl+F) or a subfolder.";
constexpr const wchar_t* kStrSyncInsideOther = L"One folder is inside the other, so a sync would copy into itself.";
constexpr const wchar_t* kStrSyncCancelled = L"Cancelled; nothing was changed.";
constexpr const wchar_t* kStrSyncNothingSelected = L"No actions are selected.";
constexpr const wchar_t* kStrSyncPreviewHint =
    L" Space: on/off  Left/Right: copy direction  Del: delete  Tab: filter  F3/Shift+F3: view  F5: synchronize  "
    L"Esc: cancel";
constexpr const wchar_t* kStrSyncBackLabel = L"Back to preview";
constexpr const wchar_t* kStrSyncGoLabel = L"Synchronize";
constexpr const wchar_t* kStrVerbSync = L"Synchronize";
constexpr const wchar_t* kStrVerbSyncing = L"Synchronizing";
constexpr const wchar_t* kStrSyncChangedSincePreview = L"changed since the preview, skipped";
inline std::wstring FormatSyncTotals(size_t copyRight, const std::wstring& bytesRight, size_t copyLeft,
                                     const std::wstring& bytesLeft, size_t overwrite, size_t deleteRight,
                                     size_t deleteLeft, size_t conflicts) {
    std::wstring s = L"Copy " + std::to_wstring(copyRight) + L" -> (" + bytesRight + L")";
    if (copyLeft > 0) s += L", " + std::to_wstring(copyLeft) + L" <- (" + bytesLeft + L")";
    if (overwrite > 0) s += L", " + std::to_wstring(overwrite) + L" overwrite";
    if (deleteRight > 0) s += L", delete " + std::to_wstring(deleteRight) + L" on the right";
    if (deleteLeft > 0) s += L", delete " + std::to_wstring(deleteLeft) + L" on the left";
    if (conflicts > 0) s += L", " + std::to_wstring(conflicts) + L" unresolved conflict(s) left alone";
    return s;
}

// "12 different, 5 left only, 3 right only, 140 identical"
inline std::wstring FormatCompareSummary(size_t different, size_t leftOnly, size_t rightOnly, size_t identical,
                                         size_t unreadable, bool cancelled, bool truncated) {
    std::wstring s = std::to_wstring(different) + L" different, " + std::to_wstring(leftOnly) + L" left only, " +
                     std::to_wstring(rightOnly) + L" right only, " + std::to_wstring(identical) + L" identical";
    if (unreadable > 0) s += L", " + std::to_wstring(unreadable) + L" unreadable";
    if (cancelled) s += L" (cancelled)";
    if (truncated) s += L" (truncated)";
    return s;
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

// --- main.cpp: FOP-017 group rename --------------------------------------------

constexpr const wchar_t* kStrGroupRenameTitle = L"Group rename";
constexpr const wchar_t* kStrGroupRenameHint = L"[Enter] rename  [F2] change options  [Esc] cancel";
constexpr const wchar_t* kStrGroupRenamePluginRefused = L"Group rename is not available for plugin panels.";
constexpr const wchar_t* kStrGroupRenameNothing = L"Nothing to rename: every new name equals the old one.";
constexpr const wchar_t* kStrGroupRenameFixFirst = L"Fix these problems first (F2 changes the options):";
constexpr const wchar_t* kStrGroupRenameUnchanged = L"(unchanged)";
constexpr const wchar_t* kStrVerbRenaming = L"Renaming";
constexpr const wchar_t* kStrVerbGroupRename = L"Group rename";
inline std::wstring FormatGroupRenameTitle(size_t total, size_t toRename, size_t unchanged) {
    return L"Group rename: " + std::to_wstring(total) + L" items, " + std::to_wstring(toRename) + L" to rename, " +
           std::to_wstring(unchanged) + L" unchanged";
}
inline std::wstring FormatGroupRenameProblemsTitle(size_t problems) {
    return L"Group rename: " + std::to_wstring(problems) + L" problem(s) \x2014 press F2 to fix the options";
}

// --- main.cpp: NAV-007 directory hotlist ---------------------------------------

constexpr const wchar_t* kStrHotlistTitle = L"Directory hotlist";
constexpr const wchar_t* kStrHotlistHint = L"[Enter] go  [Del] remove  [F2] rename  [Esc] close";
constexpr const wchar_t* kStrHotlistAddPrompt = L"Name for this favorite:";
constexpr const wchar_t* kStrHotlistRenamePrompt = L"Rename favorite:";
constexpr const wchar_t* kStrHotlistRemoveTitle = L"Remove favorite?";
constexpr const wchar_t* kStrHotlistInvalidName = L"A name can't be empty or contain '='.";
constexpr const wchar_t* kStrHotlistCannotSave = L"Could not save favorites.conf.";
constexpr const wchar_t* kStrHotlistWarningIntro = L"Some lines in favorites.conf were skipped:";
inline std::wstring FormatHotlistAddRow(const std::wstring& folder) { return L"+ Add current folder (" + folder + L")"; }
inline std::wstring FormatHotlistAlreadyThere(const std::wstring& name) {
    return L"This folder is already in the hotlist as \"" + name + L"\".";
}

// --- main.cpp: FEA-0009 file actions -----------------------------------------

constexpr const wchar_t* kStrFileActionsWarningTitle = L"File actions";
constexpr const wchar_t* kStrFileActionsWarningIntro = L"Some lines in file-actions.conf were skipped:";
constexpr const wchar_t* kStrCannotRunFileActionTitle = L"Cannot run file action";
constexpr const wchar_t* kStrProgramLabel = L"Program: ";
constexpr const wchar_t* kStrAssignFileToAppTitle = L"Assign File to App";
constexpr const wchar_t* kStrAssignNeedsFile = L"Put the cursor on a file first.";
constexpr const wchar_t* kStrAddFileAction = L"+ Add a new action...";
// EXT-003: viewer plugins.
constexpr const wchar_t* kStrAddViewerPlugin = L"+ Use a viewer plugin as the default view...";
constexpr const wchar_t* kStrViewerPluginTitle = L"Viewer plugin";
constexpr const wchar_t* kStrPluginSuffix = L" (plugin)";
inline std::wstring FormatViewerPluginFailed(const std::wstring& viewer) {
    return L"\"" + viewer + L"\" could not show this file (not loaded, declined, or failed) -- opening the built-in viewer.";
}
constexpr const wchar_t* kStrEditFileActionsFile = L"  Edit file-actions.conf in the external editor";
constexpr const wchar_t* kStrCannotSaveFileActions = L"Could not save file-actions.conf.";
constexpr const wchar_t* kStrProgramRequired = L"A program is required.";
constexpr const wchar_t* kStrRemoveFileActionQuestion = L"Remove this action?";
constexpr const wchar_t* kStrBuiltinViewerLabel = L"Built-in viewer";
constexpr const wchar_t* kStrWindowsDefaultLabel = L"Windows default";
inline std::wstring FormatChooseActionTitle(const std::wstring& name) { return L"Actions for \"" + name + L"\""; }
inline std::wstring FormatFileActionsForTitle(const std::wstring& pattern) { return L"File actions for " + pattern; }

// FOP-019: Shift+F5 "copy as" prompt and its errors.
inline std::wstring FormatCopyAsHeader(const std::wstring& name) { return L"Copy \"" + name + L"\" as:"; }
constexpr const wchar_t* kStrCopyAsSameItem = L"The new name is the same as the original.";

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

inline std::wstring FormatConfigFileActionLineWarning(int lineNumber, const std::wstring& rawLine) {
    return L"Line " + std::to_wstring(lineNumber) + L" looks like a file action (" + rawLine +
           L") -- those only work in file-actions.conf, not here. Open it via File > Assign File to App.";
}
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
constexpr const wchar_t* kStrCannotCopyOntoItself = L"source and destination are the same item";
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
// FOP-017: an item parked under a temporary name couldn't be put back (ERR-001: names both).
inline std::wstring FormatRenameRollbackFailed(const std::wstring& tempName, const std::wstring& originalName,
                                               const std::wstring& reason) {
    return L"left as \"" + tempName + L"\" -- could not rename it back to \"" + originalName + L"\": " + reason;
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

// --- main.cpp: top menu bar (FEA-0005) --------------------------------------

constexpr const wchar_t* kStrMenuNotImplementedTitle = L"Not yet implemented";
constexpr const wchar_t* kStrMenuNotImplementedBody =
   L"This menu item doesn't have a feature behind it yet.";

// --- main.cpp: file properties (FEA-0003) -----------------------------------

constexpr const wchar_t* kStrFilePropertiesTitle = L"File properties";
constexpr const wchar_t* kStrPropNotSigned = L"Not signed.";
constexpr const wchar_t* kStrPropSignedUnknownSigner = L"Signed.";
constexpr const wchar_t* kStrPropSignatureCheckFailed = L"Could not check the file's signature.";

inline std::wstring FormatPropLine(const wchar_t* label, const std::wstring& value) {
   return std::wstring(label) + L": " + value;
}
inline std::wstring FormatPropSignedBy(const std::wstring& signerName) {
   return L"Signed by: " + signerName;
}

// --- DiffViewer.cpp / main.cpp: text difference viewer (SRC-008) ------------

constexpr const wchar_t* kStrDiffTitle = L"Compare contents";
constexpr const wchar_t* kStrDiffHint =
    L"F7/Shift+F7: next/prev difference  U: unified  I: case  W: whitespace  F3/Shift+F3: view  Esc: close";
constexpr const wchar_t* kStrDiffIdentical = L"Identical";
constexpr const wchar_t* kStrDiffCaseLabel = L"case: ";
constexpr const wchar_t* kStrDiffWhitespaceLabel = L"whitespace: ";
constexpr const wchar_t* kStrDiffIgnored = L"ignored";
constexpr const wchar_t* kStrDiffExact = L"exact";
constexpr const wchar_t* kStrDiffWhitespaceExact = L"exact";
constexpr const wchar_t* kStrDiffWhitespaceTrailing = L"trailing ignored";
constexpr const wchar_t* kStrDiffWhitespaceAll = L"all ignored";
constexpr const wchar_t* kStrDiffNoMore = L"(no more differences)";
constexpr const wchar_t* kStrDiffRecomputeCancelled = L"(cancelled -- previous comparison kept)";
constexpr const wchar_t* kStrDiffTooDifferent = L"(too different to align -- previous comparison kept)";
constexpr const wchar_t* kStrDiffFilesIdentical = L"The files are identical.";
constexpr const wchar_t* kStrDiffTooDifferentLong =
    L"The files are too different to align line by line (more than 20,000 changes).";
inline std::wstring FormatDiffProgress(size_t edits, size_t limit) {
    return L"Comparing files... " + std::to_wstring(edits) + L" / " + std::to_wstring(limit) + L" steps, Esc to cancel";
}
