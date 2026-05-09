@echo off
setlocal

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"

where wsl >nul 2>nul
if errorlevel 1 (
    echo WSL was not found. Install WSL or ensure `wsl.exe` is on PATH.
    exit /b 1
)

for /f "delims=" %%I in ('wsl wslpath -a "%ROOT%"') do set "WSL_ROOT=%%I"
if not defined WSL_ROOT (
    echo Failed to convert path to WSL: "%ROOT%"
    exit /b 1
)

echo Running ROS 2 helper via WSL ^(see Start-ros2.sh^).
echo Repo ^(WSL^): %WSL_ROOT%
echo.

wsl --cd "%WSL_ROOT%" bash ./Start-ros2.sh %*

if errorlevel 1 (
    echo ROS 2 helper exited with an error.
    exit /b 1
)
exit /b 0
