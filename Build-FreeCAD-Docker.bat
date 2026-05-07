@echo off
setlocal

set "ROOT=%~dp0"
set "ROOT=%ROOT:~0,-1%"
set "DOCKER_IMAGE=ghcr.io/prefix-dev/pixi:0.59.0"
set "PIXI_VOLUME=freecad-linux-pixi"
set "BUILD_VOLUME=freecad-linux-build-release"

where docker >nul 2>nul
if errorlevel 1 (
    echo Docker was not found on PATH.
    exit /b 1
)

docker volume create %PIXI_VOLUME% >nul
if errorlevel 1 exit /b 1

docker volume create %BUILD_VOLUME% >nul
if errorlevel 1 exit /b 1

echo Building FreeCAD in Docker.
echo Linux Pixi/build files are stored in Docker volumes, not in the Windows checkout.
docker run --rm -it ^
    --workdir /workspace ^
    --mount "type=bind,source=%ROOT%,target=/workspace" ^
    --mount "type=volume,source=%PIXI_VOLUME%,target=/workspace/.pixi" ^
    --mount "type=volume,source=%BUILD_VOLUME%,target=/workspace/build" ^
    %DOCKER_IMAGE% ^
    bash -lc "pixi run initialize && pixi run configure-release && pixi run build-release"
