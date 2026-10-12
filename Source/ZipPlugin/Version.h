#pragma once

// IS-0011: single source of truth for ZipPlugin's own version number --
// independent of MyCommander.exe's own Version.h/MC_VERSION_* (this plugin
// versions itself separately, same as any third-party MC Plugin would).
// Both McPlugin_GetInfo's McPluginMetadata::version string (ZipPlugin.cpp)
// and ZipPlugin.rc's embedded Windows file-version resource derive from
// these same four macros, via the SDK's shared McPluginVersion.h -- so
// there is nothing else to update by hand when this plugin's version
// changes, mirroring Source/MyCommander/Version.h's own convention for the
// host application.
#define MCPLUGIN_VERSION_MAJOR 0
#define MCPLUGIN_VERSION_MINOR 12
#define MCPLUGIN_VERSION_PATCH 0
#define MCPLUGIN_VERSION_BUILD 0

#include "../MyCommanderPluginSdk/McPluginVersion.h"
