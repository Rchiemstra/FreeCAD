@echo off
setlocal

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "DOCKER_IMAGE=ghcr.io/prefix-dev/pixi:0.59.0"

where docker >nul 2>nul
if errorlevel 1 (
    echo Docker was not found on PATH.
    echo Cannot safely remove the Linux Pixi environment from Windows.
    exit /b 1
)

echo Removing Docker-created Linux Pixi/build folders from this checkout...
docker run --rm ^
    --workdir /workspace ^
    --mount "type=bind,source=%ROOT%,target=/workspace" ^
    %DOCKER_IMAGE% ^
    bash -lc "rm -rf /workspace/.pixi/envs/default /workspace/.pixi/task-cache-v0 /workspace/build/release"

if errorlevel 1 (
    echo Cleanup failed.
    exit /b 1
)

echo Cleanup complete.
echo You can now rebuild for Windows with:
echo   pixi run initialize
echo   pixi run configure-release
echo   pixi run build-release
echo   pixi run freecad-release
