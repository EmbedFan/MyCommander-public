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
