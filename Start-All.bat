@echo off
setlocal
REM SPDX-License-Identifier: LGPL-2.1-or-later
REM
REM Start the full local stack in one step:
REM   - FreeCAD
REM   - Gazebo gz-sim helper via WSL/Docker
REM   - ROS 2 helper via WSL/Docker
REM   - FreeCAD, Gazebo, and ROS MCP server processes
REM
REM Usage:
REM   Start-All.bat          Start FreeCAD + both helpers.
REM   Start-All.bat e2e      Run unattended Docker E2E compose instead (blocks until exit).
REM

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
cd /d "%ROOT%" || exit /b 1

if /i "%~1"=="e2e" goto :e2e

if not exist "%ROOT%\.log" mkdir "%ROOT%\.log" >nul 2>nul
set "START_ALL_STATUS_FILE=%ROOT%\.log\start-all-console.txt"
del "%START_ALL_STATUS_FILE%" >nul 2>nul

powershell -NoProfile -ExecutionPolicy Bypass -File "%ROOT%\scripts\start_all.ps1" -StatusFile "%START_ALL_STATUS_FILE%" %*
set "EXITCODE=%ERRORLEVEL%"
if exist "%START_ALL_STATUS_FILE%" type "%START_ALL_STATUS_FILE%"
exit /b %EXITCODE%

:e2e
where docker >nul 2>nul
if errorlevel 1 (
    echo Docker was not found on PATH. Install Docker Desktop ^(Linux containers^) or add docker.exe to PATH.
    exit /b 1
)

echo [%DATE% %TIME%] Running Docker E2E compose ^(see docker/compose.e2e.yml^)...
docker compose -f docker/compose.e2e.yml up --build --abort-on-container-exit --exit-code-from e2e
set "EXITCODE=%ERRORLEVEL%"
exit /b %EXITCODE%
