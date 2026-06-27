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
echo Stopping gz-sim + ros_gz_bridge containers...
wsl --cd "%WSL_ROOT%" bash ./scripts/stop_gz_stack.sh
exit /b %ERRORLEVEL%
