@echo off
REM ============================================================
REM build.cmd
REM Builds the Diablo: Unbound desktop game (DiabloUnbound.vcxproj)
REM in the configuration specified by build.config.ini
REM (defaults to Debug|x64 if config missing).
REM
REM Run this from the root of the extracted Diablo-Unbound project
REM (the folder containing DiabloUnbound.sln / DiabloUnbound.vcxproj).
REM ============================================================

cls

setlocal enabledelayedexpansion
cd /d "%~dp0"

set "PROJECT=DiabloUnbound.vcxproj"
set "PLATFORM=Win32"

REM --- Read build.config.ini if present --------------------------------------
set "CONFIG_FILE=build.config.ini"
set "ENVIRONMENT=Debug"   REM default
if exist "!CONFIG_FILE!" (
    echo [INFO] Reading configuration from !CONFIG_FILE!
    for /f "usebackq tokens=1,* delims==" %%a in ("!CONFIG_FILE!") do (
        set "key=%%a"
        set "value=%%b"
        REM Strip leading/trailing spaces
        for /f "tokens=*" %%i in ("!key!") do set "key=%%i"
        for /f "tokens=*" %%i in ("!value!") do set "value=%%i"
        if /i "!key!"=="ENVIRONMENT" (
            set "ENVIRONMENT=!value!"
        )
    )
) else (
    echo [INFO] No !CONFIG_FILE! found; using default ENVIRONMENT=Debug.
)

REM Validate ENVIRONMENT
if /i not "!ENVIRONMENT!"=="Debug" if /i not "!ENVIRONMENT!"=="Release" (
    echo [ERROR] ENVIRONMENT in !CONFIG_FILE! must be Debug or Release.
    goto :fail
)
echo [INFO] Build configuration: !ENVIRONMENT!

REM --- Verify project file ---------------------------------------------------
if not exist "!PROJECT!" (
    echo [ERROR] !PROJECT! not found in !cd!
    echo         Run this script from the project root.
    goto :fail
)

REM --- Locate vswhere --------------------------------------------------------
set "PF86=%ProgramFiles(x86)%"
set "PF64=%ProgramFiles%"
set "VSWHERE=!PF86!\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" set "VSWHERE=!PF64!\Microsoft Visual Studio\Installer\vswhere.exe"
if not exist "!VSWHERE!" (
    echo [ERROR] Could not find vswhere.exe. Is Visual Studio / Build Tools installed?
    goto :fail
)

REM --- Locate VS instance with MSBuild ---------------------------------------
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

REM --- Determine the toolset version from the VS installation ----------------
REM Get the product line version (e.g., 17 for VS 2022, 16 for VS 2019)
set "VS_VERSION="
for /f "usebackq tokens=*" %%v in (`"!VSWHERE!" -latest -products * -requires Microsoft.Component.MSBuild -property catalog_productLineVersion`) do (
    set "VS_VERSION=%%v"
)
if not defined VS_VERSION (
    echo [WARN] Could not determine Visual Studio version. Defaulting to v143.
    set "TOOLSET=v143"
) else (
    REM Map major version to toolset name
    if "!VS_VERSION!"=="17" set "TOOLSET=v143"
    if "!VS_VERSION!"=="16" set "TOOLSET=v142"
    if "!VS_VERSION!"=="15" set "TOOLSET=v141"
    if "!VS_VERSION!"=="14" set "TOOLSET=v140"
    REM If unknown, default to v143
    if not defined TOOLSET (
        echo [WARN] Unknown VS version !VS_VERSION!. Defaulting to v143.
        set "TOOLSET=v143"
    )
)
echo [INFO] Using toolset: !TOOLSET!

REM --- Find Windows SDK ------------------------------------------------------
set "SDKROOT="
for /f "tokens=2,*" %%a in ('reg query "HKLM\SOFTWARE\WOW6432Node\Microsoft\Windows Kits\Installed Roots" /v KitsRoot10 2^>nul ^| findstr /i KitsRoot10') do set "SDKROOT=%%b"
if not defined SDKROOT (
    for /f "tokens=2,*" %%a in ('reg query "HKLM\SOFTWARE\Microsoft\Windows Kits\Installed Roots" /v KitsRoot10 2^>nul ^| findstr /i KitsRoot10') do set "SDKROOT=%%b"
)
if defined SDKROOT if "!SDKROOT:~-1!"=="\" set "SDKROOT=!SDKROOT:~0,-1!"

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
echo [INFO] Building !PROJECT! (!ENVIRONMENT!^|!PLATFORM!) with toolset !TOOLSET!...
msbuild "!PROJECT!" /nologo /m /p:Configuration=!ENVIRONMENT! /p:Platform=!PLATFORM! /p:PlatformToolset=!TOOLSET! !SDKPROP! /verbosity:minimal
if errorlevel 1 goto :fail

echo.
echo [SUCCESS] Build complete.

REM --- Determine output directory and copy unbound.mpq -----------------------
set "OUTPUT_DIR=!cd!\Build\Win!ENVIRONMENT!"
if not exist "!OUTPUT_DIR!" (
    echo [WARN] Output directory !OUTPUT_DIR! does not exist. Skipping MPQ copy.
    goto :success_exit
)

set "MPQ_SOURCE=!cd!\UnboundMPQ\unbound.mpq"
if exist "!MPQ_SOURCE!" (
    echo [INFO] Copying unbound.mpq to !OUTPUT_DIR!
    copy /y "!MPQ_SOURCE!" "!OUTPUT_DIR!\" >nul
    if errorlevel 1 (
        echo [WARN] Failed to copy unbound.mpq.
    ) else (
        echo [INFO] unbound.mpq copied successfully.
    )
) else (
    echo [WARN] unbound.mpq not found in !cd!\UnboundMPQ\. Skipping copy.
)

:success_exit
echo.
echo           Output: !OUTPUT_DIR!\Diablo.exe
goto :eof

:fail
echo.
echo [FAILED] Build did not complete. See errors above.
exit /b 1