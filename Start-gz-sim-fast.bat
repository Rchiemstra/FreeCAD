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
echo Fast gz-sim stack (OSRF packages, no source build). See docs/gazebo-lifecycle.md
wsl --cd "%WSL_ROOT%" bash ./scripts/run_gz_sim_fast.sh %*
exit /b %ERRORLEVEL%
