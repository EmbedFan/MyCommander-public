# FEA-0006 Sec 4 / INST-006: force-rebuilds MyCommander as Release|x64 and
# stages its payload for MyCommanderSetup to embed as RCDATA resources.
#
# Wired as MyCommanderSetup's PreBuildEvent, so it runs before every build of
# the installer - Debug or Release, Visual Studio or the MSBuild CLI - and
# guarantees the embedded MyCommander.exe is always a fresh, optimized
# Release build, never whatever happened to already sit in
# Build\x64\Release\ and never a Debug build, regardless of which
# configuration MyCommanderSetup itself is being built in. Deliberately a
# full Rebuild (clean, then build), not the incremental Build target, so a
# stale or partially-up-to-date Release folder can never be silently reused.
#
# This must go through MyCommander.sln, not MyCommander.vcxproj directly -
# see CLAUDE.md's own note that $(SolutionDir) resolves incorrectly when a
# .vcxproj is built outside its solution.
$ErrorActionPreference = 'Stop'

$repoRoot = Resolve-Path (Join-Path $PSScriptRoot '..')
$solutionPath = Join-Path $repoRoot 'MyCommander.sln'

function Find-MSBuild {
    $vswhere = Join-Path ${env:ProgramFiles(x86)} 'Microsoft Visual Studio\Installer\vswhere.exe'
    if (Test-Path $vswhere) {
        $installPath = & $vswhere -latest -products * -requires Microsoft.Component.MSBuild -property installationPath
        if ($installPath) {
            $candidate = Join-Path $installPath 'MSBuild\Current\Bin\MSBuild.exe'
            if (Test-Path $candidate) { return $candidate }
        }
    }
    $onPath = Get-Command msbuild.exe -ErrorAction SilentlyContinue
    if ($onPath) { return $onPath.Source }
    throw 'Could not locate MSBuild.exe (checked vswhere.exe and PATH).'
}

$msbuild = Find-MSBuild
Write-Host "Rebuilding MyCommander (Release|x64) for the installer payload..."

& $msbuild $solutionPath /t:MyCommander:Rebuild /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
if ($LASTEXITCODE -ne 0) {
    Write-Error "MyCommander Release rebuild failed (exit $LASTEXITCODE) - refusing to package a broken build into the installer."
    exit 1
}

$payloadDir = Join-Path $PSScriptRoot '..\Source\MyCommanderSetup\payload'
New-Item -ItemType Directory -Force -Path $payloadDir | Out-Null

$releaseExe = Join-Path $repoRoot 'Build\x64\Release\MyCommander.exe'
if (-not (Test-Path $releaseExe)) {
    Write-Error "Expected rebuilt exe not found at $releaseExe"
    exit 1
}

Copy-Item -Path $releaseExe -Destination (Join-Path $payloadDir 'MyCommander.exe') -Force
Copy-Item -Path (Join-Path $repoRoot 'USERGUIDE.md') -Destination (Join-Path $payloadDir 'USERGUIDE.md') -Force
Copy-Item -Path (Join-Path $repoRoot 'LICENSE') -Destination (Join-Path $payloadDir 'LICENSE') -Force

Write-Host "Installer payload staged in $payloadDir"

# IS-0013: also stage the ZIP plugin, the one real, user-facing ("implemented")
# plugin today -- ExamplePlugin is deliberately excluded, it's a developer-
# facing template, not a feature meant to ship to end users. Same freshness
# discipline as MyCommander.exe above (always rebuilt, never whatever already
# happens to sit in Build\), but a failed plugin rebuild is a warning, not a
# script-terminating error -- the core app is fully usable with zero plugins
# installed (true today for every user), so this must never block packaging
# or installing it.
$zipPluginProject = Join-Path $repoRoot 'Source\ZipPlugin\ZipPlugin.vcxproj'
try {
    & $msbuild $zipPluginProject /t:Rebuild /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "ZipPlugin rebuild failed (exit $LASTEXITCODE)" }

    $zipPluginDll = Join-Path $repoRoot 'Source\ZipPlugin\Build\x64\Release\ZipPlugin.dll'
    if (-not (Test-Path $zipPluginDll)) { throw "Expected rebuilt plugin not found at $zipPluginDll" }

    Copy-Item -Path $zipPluginDll -Destination (Join-Path $payloadDir 'ZipPlugin.dll') -Force
    Write-Host "ZIP plugin staged in $payloadDir"
} catch {
    Write-Warning "Could not stage the ZIP plugin for the installer: $_"
    Write-Warning "Continuing without it -- the installer will still ship, just without the ZIP plugin."
}

# EXT-005 / FTP-012: the FTP plugin, staged exactly like the ZIP plugin above.
$ftpPluginProject = Join-Path $repoRoot 'Source\FtpPlugin\FtpPlugin.vcxproj'
try {
    & $msbuild $ftpPluginProject /t:Rebuild /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "FtpPlugin rebuild failed (exit $LASTEXITCODE)" }

    $ftpPluginDll = Join-Path $repoRoot 'Source\FtpPlugin\Build\x64\Release\FtpPlugin.dll'
    if (-not (Test-Path $ftpPluginDll)) { throw "Expected rebuilt plugin not found at $ftpPluginDll" }

    Copy-Item -Path $ftpPluginDll -Destination (Join-Path $payloadDir 'FtpPlugin.dll') -Force
    Write-Host "FTP plugin staged in $payloadDir"
} catch {
    Write-Warning "Could not stage the FTP plugin for the installer: $_"
    Write-Warning "Continuing without it -- the installer will still ship, just without the FTP plugin."
}

# EXT-004 / SFTP-010: the SFTP plugin (libssh2 compiled in), staged the same way.
$sftpPluginProject = Join-Path $repoRoot 'Source\SftpPlugin\SftpPlugin.vcxproj'
try {
    & $msbuild $sftpPluginProject /t:Rebuild /p:Configuration=Release /p:Platform=x64 /nologo /v:minimal
    if ($LASTEXITCODE -ne 0) { throw "SftpPlugin rebuild failed (exit $LASTEXITCODE)" }

    $sftpPluginDll = Join-Path $repoRoot 'Source\SftpPlugin\Build\x64\Release\SftpPlugin.dll'
    if (-not (Test-Path $sftpPluginDll)) { throw "Expected rebuilt plugin not found at $sftpPluginDll" }

    Copy-Item -Path $sftpPluginDll -Destination (Join-Path $payloadDir 'SftpPlugin.dll') -Force
    Write-Host "SFTP plugin staged in $payloadDir"
} catch {
    Write-Warning "Could not stage the SFTP plugin for the installer: $_"
    Write-Warning "Continuing without it -- the installer will still ship, just without the SFTP plugin."
}
