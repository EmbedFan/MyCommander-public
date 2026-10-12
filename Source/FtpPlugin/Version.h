#pragma once

// EXT-005: the FTP plugin's own version (IS-0011 pattern, independent of the
// host's Source/MyCommander/Version.h). McPlugin_GetInfo's version string and
// FtpPlugin.rc's file-version resource both derive from these four macros.
#define MCPLUGIN_VERSION_MAJOR 0
#define MCPLUGIN_VERSION_MINOR 1
#define MCPLUGIN_VERSION_PATCH 0
#define MCPLUGIN_VERSION_BUILD 0

#include "../MyCommanderPluginSdk/McPluginVersion.h"
