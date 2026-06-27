@echo off
setlocal
set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"

where wsl >nul 2>nul
if errorlevel 1 (
    echo WSL was not found.
    exit /b 1
)

for /f "delims=" %%I in ('wsl wslpath -a "%ROOT%"') do set "WSL_ROOT=%%I"
echo Starting ros_gz_bridge sidecar (requires gz-sim from Start-gz-sim.bat or Start-gz-sim-fast.bat)...
echo World/env: GAZEBO_WORLD_NAME=empty_world — see docs/gazebo-lifecycle.md
wsl --cd "%WSL_ROOT%" bash ./scripts/ensure_ros_gz_bridge.sh
exit /b %ERRORLEVEL%
