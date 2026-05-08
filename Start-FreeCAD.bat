@echo off
setlocal

set "ROOT=%~dp0"
set "FREECAD_ARGS=%*"
pushd "%ROOT%" >nul

call :try_start "%ROOT%.pixi\envs\default\Library\bin\FreeCAD.exe"
call :try_start "%ROOT%build\release\bin\FreeCAD.exe"
call :try_start "%ROOT%build\debug\bin\FreeCAD.exe"

for /d %%D in ("%ProgramFiles%\FreeCAD*") do (
    call :try_start "%%~fD\bin\FreeCAD.exe"
    call :try_start "%%~fD\FreeCAD.exe"
)

if defined ProgramFiles(x86) (
    for /d %%D in ("%ProgramFiles(x86)%\FreeCAD*") do (
        call :try_start "%%~fD\bin\FreeCAD.exe"
        call :try_start "%%~fD\FreeCAD.exe"
    )
)

for /d %%D in ("%LOCALAPPDATA%\Programs\FreeCAD*") do (
    call :try_start "%%~fD\bin\FreeCAD.exe"
    call :try_start "%%~fD\FreeCAD.exe"
)

where FreeCAD.exe >nul 2>nul
if not errorlevel 1 (
    start "" FreeCAD.exe %FREECAD_ARGS%
    goto :done
)

where pixi >nul 2>nul
if not errorlevel 1 (
    echo FreeCAD.exe was not found.
    echo Refreshing CMake files ^(configure-release^), then building/installing and starting via freecad-release...
    pixi run configure-release
    if errorlevel 1 (
        echo configure-release failed.
        pause
        exit /b 1
    )
    pixi run freecad-release %FREECAD_ARGS%
    goto :done
)

echo FreeCAD.exe was not found.
echo Install FreeCAD, or build this checkout with pixi ^(configure-release then freecad-release^).
pause
exit /b 1

:try_start
if exist "%~1" (
    start "" "%~1" %FREECAD_ARGS%
    goto :done
)
exit /b 0

:done
popd >nul
exit /b 0
