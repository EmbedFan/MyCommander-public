#pragma once

// IS-0011: a small, header-only helper any MC Plugin DLL can use to turn its
// own numeric version into both a narrow string (for a .rc file's
// FileVersion/ProductVersion VALUE strings) and a wide string (for
// McPluginMetadata::version, which is const wchar_t*) -- so a plugin's
// McPlugin_GetInfo and its embedded Windows file-version resource always
// agree, the same single-source-of-truth shape Source/MyCommander/Version.h
// already gives the host application itself.
//
// Usage: in your own plugin's Version.h, define the four macros below, then
// include this header:
//
//   #define MCPLUGIN_VERSION_MAJOR 1
//   #define MCPLUGIN_VERSION_MINOR 0
//   #define MCPLUGIN_VERSION_PATCH 0
//   #define MCPLUGIN_VERSION_BUILD 0
//   #include "../MyCommanderPluginSdk/McPluginVersion.h"
//
// Then use MCPLUGIN_VERSION_STR ("1.0.0.0", narrow) in your .rc file's
// FileVersion/ProductVersion values, and MCPLUGIN_VERSION_WSTR (L"1.0.0.0")
// as McPluginMetadata::version in your own McPlugin_GetInfo -- see
// Source/ZipPlugin/Version.h and Source/ZipPlugin/ZipPlugin.rc for a
// complete worked example, and DEVELOPER_GUIDE.md's "Giving your plugin DLL
// file-version info" section for the full walkthrough.
//
// There is nothing plugin-specific baked into this header -- a plugin's own
// MAJOR/MINOR/PATCH/BUILD numbers, and whether it even wants a .rc file at
// all, are entirely up to that plugin's own author. A plugin with no .rc
// file simply shows no file-version info on Alt+Enter (FileInfo.cpp degrades
// gracefully on any PE file with no VERSIONINFO resource), exactly as it did
// before this mechanism existed.

#ifndef MCPLUGIN_VERSION_MAJOR
#error "Define MCPLUGIN_VERSION_MAJOR/MINOR/PATCH/BUILD before including McPluginVersion.h"
#endif

#define MCPLUGIN_STRINGIZE_IMPL(x) #x
#define MCPLUGIN_STRINGIZE(x) MCPLUGIN_STRINGIZE_IMPL(x)

// "1.0.0.0" -- usable from both C++ (narrow string) and the .rc compiler.
#define MCPLUGIN_VERSION_STR                                                                               \
    MCPLUGIN_STRINGIZE(MCPLUGIN_VERSION_MAJOR) "." MCPLUGIN_STRINGIZE(MCPLUGIN_VERSION_MINOR) "." MCPLUGIN_STRINGIZE( \
        MCPLUGIN_VERSION_PATCH) "." MCPLUGIN_STRINGIZE(MCPLUGIN_VERSION_BUILD)

// Builds the wide form by pasting L onto only the first piece of the
// already-stringized narrow version above, then letting ordinary adjacent
// string-literal concatenation (translation phase 6, after preprocessing)
// join the rest -- L"1" "." "0" "." "0" "." "0" concatenates into
// L"1.0.0.0" because C++ allows concatenating an encoding-prefixed literal
// with plain ones. Simply writing `L MCPLUGIN_VERSION_STR` would NOT work:
// prefixing a multi-token macro expansion with L does not fuse into a
// single L"..." token (only ## on one token does that), which is exactly
// why Source/MyCommander/Version.h's own MC_VERSION_STR stays narrow-only
// and is printed with wprintf's %S instead -- this plugin-facing version
// needs an actual wide literal (McPluginMetadata::version's type), so it
// takes the extra WIDEN step rather than reusing that narrower approach.
#define MCPLUGIN_WIDEN_IMPL(x) L##x
#define MCPLUGIN_WIDEN(x) MCPLUGIN_WIDEN_IMPL(x)
#define MCPLUGIN_VERSION_WSTR MCPLUGIN_WIDEN(MCPLUGIN_VERSION_STR)
