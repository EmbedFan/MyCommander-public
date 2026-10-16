@echo off
rem EXT-004: builds and runs the SFTP plugin's standalone harnesses (not MSBuild,
rem like Source\FtpPlugin's): SftpLogicTests (pure logic), SftpPanelTests (the
rem plugin UI + shared remote panel on an in-memory server) and
rem SftpIntegrationTests (a real SSH server, skipped unless MC_SFTP_TEST_HOST is
rem set -- see that file's header).
rem   No server needed:           SftpLogicTests, SftpPanelTests
rem   NEEDS A REAL SFTP SERVER:   SftpIntegrationTests (read-only checks: any
rem                               server; W1-W15: a writable MC_SFTP_TEST_DIR)
rem libssh2 is compiled once into a static
rem library under Build\. Exit code 0 = every harness passed.
rem Usage: Source\SftpPlugin\BuildTests.cmd [outputFolder]   (default: Build\SftpTests)

setlocal
call "C:\Program Files\Microsoft Visual Studio\2022\Community\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
where cl >nul 2>nul || for /f "usebackq delims=" %%i in (`"%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe" -latest -property installationPath`) do call "%%i\VC\Auxiliary\Build\vcvars64.bat" >nul 2>nul
set ROOT=%~dp0..\..
set S=%ROOT%\Source\SftpPlugin
set C=%ROOT%\Source\PluginCommon
set M=%ROOT%\Source\MyCommander
set L=%S%\third_party\libssh2
set OUT=%~1
if "%OUT%"=="" set OUT=%ROOT%\Build\SftpTests
if not exist "%OUT%\libssh2" mkdir "%OUT%\libssh2"
set DEFS=/DLIBSSH2_WINCNG /DLIBSSH2_ECDSA_WINCNG /DLIBSSH2_API= /D_CRT_SECURE_NO_WARNINGS
set CXX=cl /nologo /utf-8 /std:c++20 /EHsc /MTd /W4 %DEFS% /I"%L%\include" /Fo"%OUT%\\"

if exist "%OUT%\libssh2.lib" goto haveLib
set LIBSRC=
for %%f in (agent bcrypt_pbkdf chacha channel cipher-chachapoly comp crypt crypto global hostkey keepalive kex knownhost mac misc packet pem poly1305 publickey scp session sftp transport userauth userauth_kbd_packet version) do call set LIBSRC=%%LIBSRC%% "%L%\src\%%f.c"
cl /nologo /c /MTd /W3 /wd4996 %DEFS% /I"%L%\include" /Fo"%OUT%\libssh2\\" %LIBSRC% > "%OUT%\libssh2.log" 2>&1 || (type "%OUT%\libssh2.log" & exit /b 1)
lib /nologo /OUT:"%OUT%\libssh2.lib" "%OUT%\libssh2\*.obj" || exit /b 1
:haveLib

%CXX% /Fe"%OUT%\SftpLogicTests.exe" "%S%\SftpLogicTests.cpp" "%S%\SftpLogic.cpp" "%C%\RemoteConnections.cpp" || exit /b 1
%CXX% /Fe"%OUT%\SftpPanelTests.exe" "%S%\SftpPanelTests.cpp" "%S%\SftpClient.cpp" "%S%\SftpLogic.cpp" "%C%\RemotePanel.cpp" "%C%\RemoteConnections.cpp" "%C%\RemoteUi.cpp" "%M%\PluginProviderAdapter.cpp" "%M%\ProviderCopy.cpp" "%M%\LocalProvider.cpp" "%M%\FileProvider.cpp" "%OUT%\libssh2.lib" || exit /b 1
%CXX% /Fe"%OUT%\SftpIntegrationTests.exe" "%S%\SftpIntegrationTests.cpp" "%S%\SftpClient.cpp" "%S%\SftpLogic.cpp" "%C%\RemoteUi.cpp" "%OUT%\libssh2.lib" || exit /b 1

set FAILED=0
"%OUT%\SftpLogicTests.exe" || set FAILED=1
"%OUT%\SftpPanelTests.exe" || set FAILED=1
"%OUT%\SftpIntegrationTests.exe" || set FAILED=1
exit /b %FAILED%
