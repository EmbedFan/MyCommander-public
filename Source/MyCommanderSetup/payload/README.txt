This folder is populated automatically, every build, by
Tools\RebuildReleaseForInstaller.ps1 (MyCommanderSetup's own PreBuildEvent):
a freshly Release-rebuilt MyCommander.exe, plus USERGUIDE.md and LICENSE
copied from the repo root. MyCommanderSetup.rc embeds all three as RCDATA
resources (FEA-0006 Sec 4).

Nothing else in this folder is source-controlled (see .gitignore) - it is
entirely build output, regenerated on every MyCommanderSetup build. This
placeholder file exists only so the folder itself is present in a fresh
checkout, before the first build has run.
