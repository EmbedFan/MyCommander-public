#pragma once

// FEA-0002, Phase 1: shared plain-data types used by McPluginHost.h's
// function pointers -- kept in their own header, separate from
// McPluginAbi.h's identity/lifecycle types, so a future addition here
// (e.g. Phase 2/3 types) never touches the identity/lifecycle ABI other
// code already depends on.

#include <stdint.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// §10: mirrors Log.h's LogLevel exactly, minus Off -- a plugin chooses
// which of its own lines to send, it can't silence the host's logging
// threshold (that stays the user's own Config::logLevel choice).
typedef enum McLogLevel {
    kMcLogError,
    kMcLogWarning,
    kMcLogInfo,
    kMcLogDebug,
} McLogLevel;

// §9: one labeled, hotkey-triggered choice in a ShowChoice dialog --
// mirrors Dialog.h's own DialogOption shape (hotkey + label) exactly, so
// the host's wrapper is a direct pass-through, no translation needed.
typedef struct McDialogOption {
    wchar_t hotkey;
    const wchar_t* label;
} McDialogOption;

// §11: a read-only snapshot of the live theme, in host-native RGB-ish
// slots (0-7 standard console colors, +8 as the bright/intense bit) rather
// than raw Win32 WORD attribute bitmasks -- a plugin author shouldn't need
// to know FOREGROUND_INTENSITY exists. One (fg, bg) pair per
// Theme.h::ThemePalette field, same field order. `structSize` follows the
// same forward-compatible-read convention as McPluginMetadata.
typedef struct McThemeSnapshot {
    uint32_t structSize;
    uint8_t normalFg, normalBg;
    uint8_t dirFg, dirBg;
    uint8_t selectedFg, selectedBg;
    uint8_t headerActiveFg, headerActiveBg;
    uint8_t headerInactiveFg, headerInactiveBg;
    uint8_t statusBarFg, statusBarBg;
    uint8_t commandLineFg, commandLineBg;
    uint8_t brokenFg, brokenBg;
    uint8_t brokenBannerFg, brokenBannerBg;
    uint8_t borderFg, borderBg;
    uint8_t titleFg, titleBg;
    uint8_t bodyFg, bodyBg;
    uint8_t sectionFg, sectionBg;
    uint8_t keyFg, keyBg;
} McThemeSnapshot;

// §12: one keyboard shortcut a plugin wants to own. `virtualKeyCode` is a
// standard Win32 VK_* constant (e.g. VK_F2, or a plain letter's uppercase
// ASCII code). Rejected by McPluginHost::RegisterShortcut (returns false,
// nothing registered) if it collides with a built-in shortcut or one
// another plugin already holds -- see DEVELOPER_GUIDE.md's Troubleshooting
// chapter for how to tell which happened.
typedef struct McShortcutSpec {
    uint16_t virtualKeyCode;
    bool ctrl;
    bool shift;
    bool alt;
    uint32_t commandId;  // handed back unchanged to the plugin's own OnShortcut
} McShortcutSpec;

// §6: where a plugin's new menu item is placed relative to an existing
// built-in item. `afterItemMnemonic` is looked up within the target top
// menu only (menu mnemonics/item mnemonics are two independent
// namespaces, matching MenuBar.h's own existing per-menu mnemonic
// scoping); if it doesn't match any current item in that menu, the item is
// appended instead -- the same graceful fallback as an outright
// kMcMenuPlacementAppend request, never a hard failure.
typedef enum McMenuPlacement {
    kMcMenuPlacementAppend,
    kMcMenuPlacementAfter,
} McMenuPlacement;

// Phase 3, §8: a small, named palette for McHost_PanelDrawTextFn -- deliberately
// not a raw Win32 WORD attribute (same reasoning as McThemeSnapshot's own
// small-integer color slots, §11) and deliberately small (four roles, not
// one entry per Theme.h attribute) since a panel-content plugin is drawing
// its own content, not reproducing the built-in file-listing's exact
// palette. The host maps each to whichever live theme attribute already
// serves that role (Normal -> kAttrNormal, Highlight -> the panel cursor's
// bright foreground and blue background,
// Section -> kAttrSection, Warning -> kAttrBroken), so a plugin's panel
// content stays visually consistent with a live theme change (§11)
// automatically, the same way the built-in panels already do.
typedef enum McPanelColor {
    kMcPanelColorNormal,
    kMcPanelColorHighlight,
    kMcPanelColorSection,
    kMcPanelColorWarning,
    kMcPanelColorSelected,
} McPanelColor;

// FEA-0007, §1: one field in a McHost_ShowFormFn dialog -- a generic,
// reusable multi-field options-dialog primitive (not ZIP-specific), since
// ShowChoice/ShowTextPrompt each handle exactly one concern and a packing
// dialog needs several at once. Every field is in/out: the plugin sets the
// initial value before calling ShowForm, and (if the user didn't press Esc)
// reads back the edited value afterward from the same struct.
typedef enum McFormFieldKind {
    kMcFormFieldCheckbox,
    kMcFormFieldNumber,
    kMcFormFieldChoice,  // cycles through `choices` on Left/Right, like a combo box
    kMcFormFieldText,
    // API 1.9 (FTP-010): a Text field whose characters are never drawn (each
    // shows as '*'); same textBuffer/textBufferSize in/out convention as Text.
    // A host older than 1.9 rejects a plugin declaring 1.9, so a plugin using
    // this never meets a host that would draw it in the clear.
    kMcFormFieldPassword,
} McFormFieldKind;

typedef struct McFormField {
    uint32_t structSize;
    McFormFieldKind kind;
    const wchar_t* label;

    bool boolValue;  // kMcFormFieldCheckbox

    int64_t numberValue;  // kMcFormFieldNumber
    int64_t numberMin;
    int64_t numberMax;

    int32_t choiceIndex;  // kMcFormFieldChoice
    const wchar_t* const* choices;
    int32_t choiceCount;

    wchar_t* textBuffer;  // kMcFormFieldText -- truncate-safe, same convention as ShowTextPrompt
    int32_t textBufferSize;
} McFormField;

#ifdef __cplusplus
}  // extern "C"
#endif
