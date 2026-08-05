@echo off
rem ==========================================================================
rem  windows-build-and-test.bat -- build rsync on Windows and run its tests.
rem
rem  Usage:
rem      windows-build-and-test.bat [options]
rem
rem  Options:
rem      --clean            delete the build directory first
rem      --config CFG       Release (default), Debug or RelWithDebInfo
rem      --build-dir DIR    build directory (default: build)
rem      --host USER@HOST   also run the ssh transfer tests against HOST
rem      --tests PATTERN    run only matching tests (may be repeated)
rem      --no-tests         build only
rem      -h, --help         show this text
rem
rem  Requires Visual Studio 2022 with the C++ tools, and Python 3.6+.
rem  CMake and Ninja ship with Visual Studio, and the MSVC environment is set
rem  up automatically via vswhere, so this does not have to be run from a
rem  Developer Command Prompt.
rem
rem  Note on style: variables are expanded as !VAR! rather than %VAR% inside
rem  every parenthesised block.  %VAR% is substituted while the block is being
rem  parsed, so a value containing parentheses -- "C:\Program Files (x86)\..."
rem  being the obvious one -- would close the block early.
rem
rem  Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
rem  Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
rem ==========================================================================

setlocal EnableExtensions EnableDelayedExpansion

set "SRC_DIR=%~dp0"
if "%SRC_DIR:~-1%"=="\" set "SRC_DIR=%SRC_DIR:~0,-1%"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

set "BUILD_DIR=build"
set "CONFIG=Release"
set "RUN_TESTS=1"
set "DO_CLEAN=0"
set "TEST_ARGS="

rem ---------------------------------------------------------------- options

:parse_args
if "%~1"=="" goto args_done
if /i "%~1"=="-h"          goto usage
if /i "%~1"=="--help"      goto usage
if /i "%~1"=="--clean" (
    set "DO_CLEAN=1"
    shift
    goto parse_args
)
if /i "%~1"=="--no-tests" (
    set "RUN_TESTS=0"
    shift
    goto parse_args
)
if /i "%~1"=="--config" (
    if "%~2"=="" goto missing_value
    set "CONFIG=%~2"
    shift
    shift
    goto parse_args
)
if /i "%~1"=="--build-dir" (
    if "%~2"=="" goto missing_value
    set "BUILD_DIR=%~2"
    shift
    shift
    goto parse_args
)
if /i "%~1"=="--host" (
    if "%~2"=="" goto missing_value
    set "RSYNC_WIN_TEST_HOST=%~2"
    shift
    shift
    goto parse_args
)
if /i "%~1"=="--tests" (
    if "%~2"=="" goto missing_value
    set "TEST_ARGS=!TEST_ARGS! %~2"
    shift
    shift
    goto parse_args
)
echo ERROR: unknown option "%~1"
echo.
goto usage

:missing_value
echo ERROR: option "%~1" needs a value
echo.
goto usage

:args_done

rem Make the build directory absolute, so a step that needs a different
rem working directory still finds it.
for %%d in ("!BUILD_DIR!") do set "BUILD_DIR=%%~fd"

echo ==========================================================================
echo  rsync for Windows
echo    source     : !SRC_DIR!
echo    build      : !BUILD_DIR!
echo    config     : !CONFIG!
echo ==========================================================================
echo.

rem ------------------------------------------------------- MSVC environment

if defined INCLUDE (
    echo [1/5] MSVC environment already set, using it.
    goto have_msvc
)

echo [1/5] Setting up the MSVC environment...
if not exist "!VSWHERE!" (
    echo ERROR: cannot find vswhere.exe at:
    echo        !VSWHERE!
    echo        Install Visual Studio 2022 with the "Desktop development with
    echo        C++" workload, or run this from a Developer Command Prompt.
    goto fail
)

set "VS_PATH="
for /f "usebackq tokens=*" %%i in (`"!VSWHERE!" -latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -property installationPath 2^>nul`) do set "VS_PATH=%%i"

if not defined VS_PATH (
    echo ERROR: no Visual Studio installation with the C++ tools was found.
    echo        Install the "Desktop development with C++" workload.
    goto fail
)

if not exist "!VS_PATH!\VC\Auxiliary\Build\vcvars64.bat" (
    echo ERROR: found Visual Studio at "!VS_PATH!"
    echo        but not VC\Auxiliary\Build\vcvars64.bat under it.
    goto fail
)

call "!VS_PATH!\VC\Auxiliary\Build\vcvars64.bat" >nul
if errorlevel 1 (
    echo ERROR: vcvars64.bat failed.
    goto fail
)
echo       using !VS_PATH!

:have_msvc

rem ------------------------------------------------------------------ tools

echo [2/5] Locating the build tools...

where cmake >nul 2>&1
if errorlevel 1 (
    echo ERROR: cmake is not on PATH. It ships with Visual Studio under
    echo        Common7\IDE\CommonExtensions\Microsoft\CMake, or install it
    echo        separately.
    goto fail
)

where ninja >nul 2>&1
if errorlevel 1 (
    echo ERROR: ninja is not on PATH. It ships with Visual Studio under
    echo        Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja.
    goto fail
)

set "PYTHON="
where python >nul 2>&1
if not errorlevel 1 set "PYTHON=python"
if not defined PYTHON (
    where py >nul 2>&1
    if not errorlevel 1 set "PYTHON=py -3"
)
if not defined PYTHON (
    echo ERROR: no Python interpreter found. Python 3.6+ is required: CMake
    echo        uses it to generate proto.h and friends, in place of awk.
    goto fail
)
echo       cmake, ninja and !PYTHON! found.

rem --------------------------------------------------------------- clean up

if "!DO_CLEAN!"=="1" (
    echo [3/5] Removing "!BUILD_DIR!"...
    if exist "!BUILD_DIR!" rmdir /s /q "!BUILD_DIR!"
    if exist "!BUILD_DIR!" (
        echo ERROR: could not remove "!BUILD_DIR!".
        goto fail
    )
) else (
    echo [3/5] Keeping any existing build directory ^(use --clean to wipe^).
)

rem --------------------------------------------------------------- configure

echo [4/5] Configuring...
cmake -S "!SRC_DIR!" -B "!BUILD_DIR!" -G Ninja -DCMAKE_BUILD_TYPE=!CONFIG!
if errorlevel 1 (
    echo.
    echo ERROR: cmake configure failed.
    echo        If it complained about a stale config.h or proto.h in the
    echo        source tree, those are left over from an autoconf build and
    echo        would shadow the generated ones; delete them and retry.
    goto fail
)

rem ------------------------------------------------------------------- build

echo.
echo [5/5] Building...
cmake --build "!BUILD_DIR!"
if errorlevel 1 (
    echo.
    echo ERROR: the build failed.
    goto fail
)

if not exist "!BUILD_DIR!\rsync.exe" (
    echo ERROR: the build reported success but rsync.exe is missing from
    echo        "!BUILD_DIR!".
    goto fail
)

echo.
"!BUILD_DIR!\rsync.exe" --version
if errorlevel 1 (
    echo ERROR: the freshly built rsync.exe would not run.
    goto fail
)

rem ------------------------------------------------------------------- tests

if "!RUN_TESTS!"=="0" (
    echo.
    echo Build complete ^(tests skipped^): !BUILD_DIR!\rsync.exe
    goto success
)

echo.
if defined RSYNC_WIN_TEST_HOST (
    echo Running the test suite, including ssh transfers to !RSYNC_WIN_TEST_HOST!...
) else (
    echo Running the test suite ^(pass --host USER@HOST to add the ssh tests^)...
)
echo.

!PYTHON! "!SRC_DIR!\win32\tests\run.py" --rsync-bin "!BUILD_DIR!\rsync.exe"!TEST_ARGS!
if errorlevel 1 (
    echo.
    echo ERROR: one or more tests failed.
    goto fail
)

echo.
echo Build and tests passed: !BUILD_DIR!\rsync.exe
goto success

rem ----------------------------------------------------------------- usage

:usage
echo Build rsync on Windows and run its test suite.
echo.
echo   windows-build-and-test.bat [options]
echo.
echo   --clean            delete the build directory first
echo   --config CFG       Release ^(default^), Debug or RelWithDebInfo
echo   --build-dir DIR    build directory ^(default: build^)
echo   --host USER@HOST   also run the ssh transfer tests against HOST
echo   --tests PATTERN    run only matching tests ^(may be repeated^)
echo   --no-tests         build only
echo   -h, --help         show this text
echo.
echo Requires Visual Studio 2022 with the C++ tools, and Python 3.6+.
echo CMake and Ninja ship with Visual Studio; the MSVC environment is set up
echo automatically, so a Developer Command Prompt is not needed.
endlocal
exit /b 2

:fail
echo.
endlocal
exit /b 1

:success
endlocal
exit /b 0
