This folder is populated automatically, every build, by
Tools\RebuildReleaseForInstaller.ps1 (MyCommanderSetup's own PreBuildEvent):
a freshly Release-rebuilt MyCommander.exe, plus USERGUIDE.md and LICENSE
copied from the repo root. MyCommanderSetup.rc embeds all three as RCDATA
resources (FEA-0006 Sec 4).

IS-0013: ZipPlugin.dll, freshly Release-rebuilt the same way, is staged here
too and embedded as a fourth RCDATA resource -- the one real, user-facing
plugin today. Unlike the three files above, a failed plugin rebuild is a
warning, not a script-terminating error (see the script's own comment).

Nothing else in this folder is source-controlled (see .gitignore) - it is
entirely build output, regenerated on every MyCommanderSetup build. This
placeholder file exists only so the folder itself is present in a fresh
checkout, before the first build has run.
