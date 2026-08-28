@echo off
rem ==========================================================================
rem  setup-windows-rsync.bat -- run setup-windows-rsync.ps1 from this folder.
rem
rem  Double-click it, or run it from any prompt.  It only supplies the command
rem  line the script needs (PowerShell will not run an unsigned script without
rem  -ExecutionPolicy Bypass, which applies to this one invocation and nothing
rem  else); everything the script does, and every option it takes, is in the
rem  script's own header and in `Get-Help .\setup-windows-rsync.ps1 -Full`.
rem  Arguments given here are passed straight through, e.g.
rem
rem      setup-windows-rsync.bat -SkipServer
rem
rem  Inside an unpacked release zip the script finds the rsync.exe beside it
rem  and installs those files; on its own it downloads the latest release.
rem  Either way it asks for elevation and continues in a new window, which
rem  stays open so the result can be read.
rem
rem  Copyright (C) 2026 Max Vilimpoc, rsync CMake/Windows port.
rem  Distributed under the same GPL-3.0-or-later terms as the rest of rsync.
rem ==========================================================================

if not exist "%~dp0setup-windows-rsync.ps1" (
    echo ERROR: setup-windows-rsync.ps1 is not beside this file in "%~dp0".
    pause
    exit /b 1
)

powershell -NoProfile -ExecutionPolicy Bypass -File "%~dp0setup-windows-rsync.ps1" %*
if errorlevel 1 (
    echo.
    echo setup-windows-rsync.ps1 exited with error %errorlevel%.
    pause
    exit /b %errorlevel%
)
