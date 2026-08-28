@echo off
rem ==========================================================================
rem  windows-build-and-test.bat -- build rsync on Windows and run its tests.
rem
rem  Usage:
rem      windows-build-and-test.bat [options]
rem
rem  Options:
rem      --arch ARCH        x64, x86, or both (default)
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
rem  Two architectures, and each gets its own everything:
rem
rem      x64   build\rsync.exe          the build people want
rem      x86   build-x86\rsync-x86.exe  for 32-bit Windows, and for the
rem                                     occasional 64-bit machine running
rem                                     something that will only load a
rem                                     32-bit image
rem
rem  Ninja generates for one compiler, and which compiler that is comes from
rem  the environment vcvars set up -- so "both" cannot be one configure.  This
rem  script calls itself once per architecture instead, each call with its own
rem  vcvars, its own build directory and its own run of the test suite.
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

rem Kept verbatim so an "--arch both" run can hand them to its two children.
rem The children get "--arch <one>" appended, and the last --arch wins.
rem
rem SELF has to be taken before the argument loop: `shift` renumbers %0 along
rem with everything else, so by the time parsing is done %~f0 names an option
rem rather than this script.
set "SELF=%~f0"
set "ORIG_ARGS=%*"

set "VSWHERE=%ProgramFiles(x86)%\Microsoft Visual Studio\Installer\vswhere.exe"

set "BUILD_DIR=build"
set "CONFIG=Release"
set "ARCH=both"
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
if /i "%~1"=="--arch" (
    if "%~2"=="" goto missing_value
    set "ARCH=%~2"
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

if /i "!ARCH!"=="both"  goto build_both
if /i "!ARCH!"=="x64"   goto arch_ok
if /i "!ARCH!"=="x86"   goto arch_ok
echo ERROR: --arch must be x64, x86 or both, not "!ARCH!"
echo.
goto usage

rem ------------------------------------------------------------ both, in turn
rem
rem Once per architecture, by calling this script again with the architecture
rem pinned.  Each call gets its own setlocal, so the vcvars environment it sets
rem up is discarded at its endlocal and the next call starts from a clean one --
rem which matters, because vcvars is what decides which compiler a configure
rem finds.  x64 goes first, so the build people actually want fails fast.

:build_both
echo ##########################################################################
echo #  1 of 2: x64
echo ##########################################################################
echo.
call "!SELF!" !ORIG_ARGS! --arch x64
if errorlevel 1 goto fail
echo.
echo ##########################################################################
echo #  2 of 2: x86
echo ##########################################################################
echo.
call "!SELF!" !ORIG_ARGS! --arch x86
if errorlevel 1 goto fail
echo.
echo Both architectures built and tested.
goto success

:arch_ok

rem Each architecture needs its own build directory -- Ninja caches the
rem compiler it configured with -- and its own name for the result.
if /i "!ARCH!"=="x86" (
    set "BUILD_DIR=!BUILD_DIR!-x86"
    set "EXE_NAME=rsync-x86.exe"
    set "VCVARS=vcvarsamd64_x86.bat"
) else (
    set "EXE_NAME=rsync.exe"
    set "VCVARS=vcvars64.bat"
)

rem Make the build directory absolute, so a step that needs a different
rem working directory still finds it.
for %%d in ("!BUILD_DIR!") do set "BUILD_DIR=%%~fd"

echo ==========================================================================
echo  rsync for Windows
echo    source     : !SRC_DIR!
echo    build      : !BUILD_DIR!
echo    config     : !CONFIG!
echo    target     : !ARCH! ^(!EXE_NAME!^)
echo ==========================================================================
echo.

rem ------------------------------------------------------- MSVC environment

rem An environment that is already set up is only usable if it targets the
rem architecture being built -- otherwise a Developer Command Prompt, or the
rem parent of an "--arch both" run, would silently hand x64 tools to the x86
rem build.  vcvars records its target in VSCMD_ARG_TGT_ARCH; re-running it for
rem a different target in the same process is supported and does the right
rem thing, so a mismatch is not an error, just a reason to run it again.
if defined INCLUDE (
    if /i "!VSCMD_ARG_TGT_ARCH!"=="!ARCH!" (
        echo [1/5] MSVC environment already targets !ARCH!, using it.
        goto have_msvc
    )
    echo [1/5] MSVC environment targets "!VSCMD_ARG_TGT_ARCH!", re-targeting to !ARCH!...
    goto setup_msvc
)

echo [1/5] Setting up the MSVC environment for !ARCH!...

:setup_msvc
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

rem The x86 build is cross-compiled from the 64-bit toolchain (vcvarsamd64_x86)
rem rather than built with the 32-bit one: it is the same compiler with more
rem address space to work in, and the component this script already requires,
rem VC.Tools.x86.x64, provides it.  vcvars32 is the fallback for a 32-bit host.
if not exist "!VS_PATH!\VC\Auxiliary\Build\!VCVARS!" (
    if /i "!ARCH!"=="x86" (
        if exist "!VS_PATH!\VC\Auxiliary\Build\vcvars32.bat" set "VCVARS=vcvars32.bat"
    )
)
if not exist "!VS_PATH!\VC\Auxiliary\Build\!VCVARS!" (
    echo ERROR: found Visual Studio at "!VS_PATH!"
    echo        but not VC\Auxiliary\Build\!VCVARS! under it.
    goto fail
)

call "!VS_PATH!\VC\Auxiliary\Build\!VCVARS!" >nul
if errorlevel 1 (
    echo ERROR: !VCVARS! failed.
    goto fail
)
echo       using !VS_PATH! ^(!VCVARS!^)

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
rem The two SIMD options are passed explicitly rather than left to their
rem defaults: a CMake option keeps whatever value a build directory's cache
rem already holds, so a directory configured before the defaults became ON
rem would silently keep building the scalar checksums.  This script builds
rem what the release ships, and the release has them on.
cmake -S "!SRC_DIR!" -B "!BUILD_DIR!" -G Ninja -DCMAKE_BUILD_TYPE=!CONFIG! ^
    -DRSYNC_ENABLE_SIMD=ON -DRSYNC_XXH_DISPATCH=ON
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

if not exist "!BUILD_DIR!\!EXE_NAME!" (
    echo ERROR: the build reported success but !EXE_NAME! is missing from
    echo        "!BUILD_DIR!".
    goto fail
)

echo.
"!BUILD_DIR!\!EXE_NAME!" --version
if errorlevel 1 (
    echo ERROR: the freshly built !EXE_NAME! would not run.
    goto fail
)

rem ------------------------------------------------------------------- tests

if "!RUN_TESTS!"=="0" (
    echo.
    echo Build complete ^(tests skipped^): !BUILD_DIR!\!EXE_NAME!
    goto success
)

echo.
if defined RSYNC_WIN_TEST_HOST (
    echo Running the !ARCH! test suite, including ssh transfers to !RSYNC_WIN_TEST_HOST!...
) else (
    echo Running the !ARCH! test suite ^(pass --host USER@HOST to add the ssh tests^)...
)
echo.

!PYTHON! "!SRC_DIR!\win32\tests\run.py" --rsync-bin "!BUILD_DIR!\!EXE_NAME!"!TEST_ARGS!
if errorlevel 1 (
    echo.
    echo ERROR: one or more !ARCH! tests failed.
    goto fail
)

echo.
echo Build and tests passed: !BUILD_DIR!\!EXE_NAME!
goto success

rem ----------------------------------------------------------------- usage

:usage
echo Build rsync on Windows and run its test suite.
echo.
echo   windows-build-and-test.bat [options]
echo.
echo   --arch ARCH        x64, x86, or both ^(default^)
echo   --clean            delete the build directory first
echo   --config CFG       Release ^(default^), Debug or RelWithDebInfo
echo   --build-dir DIR    build directory ^(default: build^)
echo   --host USER@HOST   also run the ssh transfer tests against HOST
echo   --tests PATTERN    run only matching tests ^(may be repeated^)
echo   --no-tests         build only
echo   -h, --help         show this text
echo.
echo x64 builds build\rsync.exe; x86 builds build-x86\rsync-x86.exe.  Both
echo builds and tests each in turn.
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
