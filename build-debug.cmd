@echo off
REM ============================================================
REM build-debug.cmd
REM Builds the Diablo: Unbound desktop game (DiabloUnbound.vcxproj)
REM in Debug|x64 configuration.
REM
REM Run this from the root of the extracted Diablo-Unbound project
REM (the folder containing DiabloUnbound.sln / DiabloUnbound.vcxproj).
REM ============================================================

setlocal enabledelayedexpansion
cd /d "%~dp0"

set "PROJECT=DiabloUnbound.vcxproj"
set "CONFIG=Debug"
set "PLATFORM=Win32"

if not exist "!PROJECT!" (
    echo [ERROR] !PROJECT! not found in !cd!
    echo         Run this script from the project root.
    goto :fail
)

REM --- Locate vswhere (ships with all modern VS/BuildTools installs) ---
set "PF86=%ProgramFiles(x86)%"
set "PF64=%ProgramFiles%"
set "VSWHERE=!PF86!\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=!PF64!\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [ERROR] Could not find vswhere.exe. Is Visual Studio / Build Tools installed?
    goto :fail
)

REM --- Locate an install that has the VC++ tools + MSBuild ---
set "VSINSTALL="
for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.Component.MSBuild -property installationPath`) do (
    set "VSINSTALL=%%i"
)
if not defined VSINSTALL (
    echo [ERROR] No Visual Studio installation with MSBuild was found.
    goto :fail
)

set "VSDEVCMD=!VSINSTALL!\Common7\Tools\VsDevCmd.bat"
if not exist "!VSDEVCMD!" (
    echo [ERROR] VsDevCmd.bat not found at "!VSDEVCMD!"
    goto :fail
)

echo [INFO] Using Visual Studio install: !VSINSTALL!
call "!VSDEVCMD!" -arch=x64 -host_arch=x64 >nul
if errorlevel 1 (
    echo [ERROR] Failed to initialize VS developer environment.
    goto :fail
)

REM --- Find the real SDK root via the registry (authoritative source) ---
set "SDKROOT="
for /f "tokens=2,*" %%a in ('reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots" /v KitsRoot10 2^>nul ^| findstr /i KitsRoot10') do set "SDKROOT=%%b"
if not defined SDKROOT (
    for /f "tokens=2,*" %%a in ('reg query "HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots" /v KitsRoot10 2^>nul ^| findstr /i KitsRoot10') do set "SDKROOT=%%b"
)
if defined SDKROOT if "!SDKROOT:~-1!"=="\" set "SDKROOT=!SDKROOT:~0,-1!"

REM --- Detect installed Windows SDK versions (must have matching Include AND Lib) and pick the highest ---
set "SDKVER="
if defined SDKROOT (
    for /f "tokens=*" %%v in ('dir "!SDKROOT!\Include" /b /ad /o-n 2^>nul ^| findstr /r "^10\."') do (
        if not defined SDKVER (
            if exist "!SDKROOT!\Lib\%%v\um\x64" set "SDKVER=%%v"
        )
    )
)

if defined SDKVER (
    echo [INFO] Found Windows SDK root: !SDKROOT!
    echo [INFO] Using installed Windows SDK version: !SDKVER!
    set "SDKPROP=/p:WindowsTargetPlatformVersion=!SDKVER!"
) else (
    echo.
    echo [ERROR] No usable Windows SDK is installed on this machine.
    echo         A native desktop build needs one regardless of version.
    echo         Install it via: Visual Studio Installer -^> Modify -^>
    echo         Individual Components -^> search "Windows SDK" -^> check
    echo         the newest "Windows 10/11 SDK" entry -^> Modify/Install.
    echo         ^(This is a small component, not a full VS re-download.^)
    goto :fail
)

echo.
echo [INFO] Building !PROJECT! (!CONFIG!^|!PLATFORM!) with the project's native toolset (v140)...
msbuild "!PROJECT!" /nologo /m /p:Configuration=!CONFIG! /p:Platform=!PLATFORM! !SDKPROP! /verbosity:minimal
if not errorlevel 1 goto :success

echo.
echo [WARN] Build failed - likely missing the v140 (VS2015) toolset.
echo [INFO] Retrying with the installed v143 toolset instead...
msbuild "!PROJECT!" /nologo /m /p:Configuration=!CONFIG! /p:Platform=!PLATFORM! /p:PlatformToolset=v143 !SDKPROP! /verbosity:minimal
if errorlevel 1 goto :fail

:success
echo.
echo [SUCCESS] Build complete.
echo           Output: !cd!\Build\WinDebug\Diablo.exe
goto :eof

:fail
echo.
echo [FAILED] Build did not complete. See errors above.
exit /b 1