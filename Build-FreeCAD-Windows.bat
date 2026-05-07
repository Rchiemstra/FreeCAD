@echo off
setlocal

set "ROOT=%~dp0"
pushd "%ROOT%" >nul

where pixi >nul 2>nul
if errorlevel 1 (
    echo pixi was not found on PATH.
    popd >nul
    exit /b 1
)

if exist "build\release\CMakeCache.txt" (
    findstr /c:"/workspace/.pixi/envs/default" "build\release\CMakeCache.txt" >nul 2>nul
    if not errorlevel 1 (
        echo build\release is configured from a Linux Docker build.
        echo Run Clean-FreeCAD-Pixi.bat first, then run this script again.
        popd >nul
        exit /b 1
    )
)

pixi run initialize
if errorlevel 1 goto :failed

pixi run configure-release
if errorlevel 1 goto :failed

pixi run build-release
if errorlevel 1 goto :failed

pixi run freecad-release
if errorlevel 1 goto :failed

popd >nul
exit /b 0

:failed
echo Build or launch failed.
popd >nul
exit /b 1
