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

set "PIXI_ENV=%ROOT%\.pixi\envs\default"
set "PIXI_LIBRARY=%PIXI_ENV%\Library"
if exist "%PIXI_LIBRARY%\bin" set "PATH=%PIXI_LIBRARY%\bin;%PATH%"
if exist "%PIXI_LIBRARY%\lib\qt6\bin" set "PATH=%PIXI_LIBRARY%\lib\qt6\bin;%PATH%"
if exist "%PIXI_LIBRARY%\mingw-w64\bin" set "PATH=%PIXI_LIBRARY%\mingw-w64\bin;%PATH%"
if exist "%PIXI_LIBRARY%\usr\bin" set "PATH=%PIXI_LIBRARY%\usr\bin;%PATH%"
if exist "%PIXI_ENV%\DLLs" set "PATH=%PIXI_ENV%\DLLs;%PATH%"
if exist "%PIXI_ENV%" set "PATH=%PIXI_ENV%;%PATH%"
if exist "%PIXI_LIBRARY%\lib\qt6\plugins" set "QT_PLUGIN_PATH=%PIXI_LIBRARY%\lib\qt6\plugins"
if exist "%PIXI_LIBRARY%\lib\qt6\plugins\platforms" set "QT_QPA_PLATFORM_PLUGIN_PATH=%PIXI_LIBRARY%\lib\qt6\plugins\platforms"
if exist "%PIXI_LIBRARY%\lib\qt6\qml" set "QML2_IMPORT_PATH=%PIXI_LIBRARY%\lib\qt6\qml"

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
