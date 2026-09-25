#pragma once

// Single source of truth for MyCommander's version number. Bump the four
// numeric components here — MyCommander.rc's VERSIONINFO resource (the
// "File Info" visible in Explorer's file Properties > Details tab) and the
// app's own `--version` output both derive from these same macros, so
// there is nothing else to update.
#define MC_VERSION_MAJOR 0
#define MC_VERSION_MINOR 1
#define MC_VERSION_PATCH 61
#define MC_VERSION_BUILD 253

#define MC_STRINGIZE_IMPL(x) #x
#define MC_STRINGIZE(x) MC_STRINGIZE_IMPL(x)

// "0.1.0.0" — usable from both C++ (narrow string) and the .rc compiler.
// To print it with wprintf, use %S (MSVC's "narrow string" specifier for
// wide-character printf functions) rather than trying to build a wide
// version of this macro: prefixing a multi-token macro expansion with `L`
// does not fuse into a single L"..." token (only `##` on one token does
// that), so `L MC_VERSION_STR` would not compile as a wide string literal.
#define MC_VERSION_STR                                                                          \
    MC_STRINGIZE(MC_VERSION_MAJOR) "." MC_STRINGIZE(MC_VERSION_MINOR) "." MC_STRINGIZE(          \
        MC_VERSION_PATCH) "." MC_STRINGIZE(MC_VERSION_BUILD)
