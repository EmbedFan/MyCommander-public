# Increments MC_VERSION_BUILD in Source/MyCommander/Version.h by 1.
#
# Wired as MyCommander's PreBuildEvent, so it runs on every real build of the
# app regardless of what triggers it - Visual Studio's Build/Rebuild, the
# MSBuild CLI, or an AI agent - with no reliance on anyone remembering to do
# it by hand. It does not run for a build MSBuild decides is already
# up to date (nothing to compile), which matches "a build happened" rather
# than "someone clicked Build."
$ErrorActionPreference = 'Stop'

$versionFile = Join-Path $PSScriptRoot '..\Source\MyCommander\Version.h'
$content = [System.IO.File]::ReadAllText($versionFile)
$pattern = '(#define MC_VERSION_BUILD )(\d+)'

$match = [regex]::Match($content, $pattern)
if (-not $match.Success) {
    Write-Error "MC_VERSION_BUILD not found in $versionFile"
    exit 1
}

$newValue = [int]$match.Groups[2].Value + 1
$newContent = [regex]::Replace($content, $pattern, "`${1}$newValue")
[System.IO.File]::WriteAllText($versionFile, $newContent)

Write-Host "MC_VERSION_BUILD -> $newValue"
