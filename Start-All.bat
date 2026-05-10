@echo off
setlocal
REM SPDX-License-Identifier: LGPL-2.1-or-later
REM
REM Start the full local stack in one step:
REM   - FreeCAD (this window returns once the app is launched)
REM   - Gazebo gz-sim helper (new console via WSL/Docker — see Start-gz-sim.bat)
REM   - ROS 2 helper (new console via WSL/Docker — see Start-ros2.bat)
REM
REM Usage:
REM   Start-All.bat          Start FreeCAD + both helpers.
REM   Start-All.bat e2e      Run unattended Docker E2E compose instead (blocks until exit).
REM

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
cd /d "%ROOT%" || exit /b 1

if /i "%~1"=="e2e" goto :e2e

echo [%DATE% %TIME%] Starting FreeCAD + Gazebo + ROS 2 helpers...
echo Repo: %ROOT%
echo.

call Start-FreeCAD.bat
if errorlevel 1 (
    echo Start-FreeCAD.bat failed.
    exit /b 1
)

REM Each helper blocks on Docker; run in separate consoles so they start together.
start "Gazebo gz-sim (WSL/Docker)" cmd /k cd /d "%ROOT%" ^& call Start-gz-sim.bat
start "ROS 2 (WSL/Docker)" cmd /k cd /d "%ROOT%" ^& call Start-ros2.bat

echo.
echo FreeCAD launch was requested. Two extra windows are running Start-gz-sim and Start-ros2.
echo Close those windows or press Ctrl+C inside them to stop the Docker sessions.
exit /b 0

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
