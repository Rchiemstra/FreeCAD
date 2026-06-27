@echo off
setlocal

set "ROOT=%~dp0"
set "FREECAD_ARGS=%*"
set "PIXI_ENV=%ROOT%.pixi\envs\default"
set "PIXI_LIBRARY=%PIXI_ENV%\Library"
pushd "%ROOT%" >nul

call :install_sim_workbench

call :try_start_local "%PIXI_LIBRARY%\bin\FreeCAD.exe"
if defined FREECAD_STARTED goto :done
call :try_start_local "%ROOT%build\release\bin\FreeCAD.exe"
if defined FREECAD_STARTED goto :done
call :try_start_local "%ROOT%build\debug\bin\FreeCAD.exe"
if defined FREECAD_STARTED goto :done

for /d %%D in ("%ProgramFiles%\FreeCAD*") do (
    call :try_start "%%~fD\bin\FreeCAD.exe"
    if defined FREECAD_STARTED goto :done
    call :try_start "%%~fD\FreeCAD.exe"
    if defined FREECAD_STARTED goto :done
)

if defined ProgramFiles(x86) (
    for /d %%D in ("%ProgramFiles(x86)%\FreeCAD*") do (
        call :try_start "%%~fD\bin\FreeCAD.exe"
        if defined FREECAD_STARTED goto :done
        call :try_start "%%~fD\FreeCAD.exe"
        if defined FREECAD_STARTED goto :done
    )
)

for /d %%D in ("%LOCALAPPDATA%\Programs\FreeCAD*") do (
    call :try_start "%%~fD\bin\FreeCAD.exe"
    if defined FREECAD_STARTED goto :done
    call :try_start "%%~fD\FreeCAD.exe"
    if defined FREECAD_STARTED goto :done
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
    set "FREECAD_STARTED=1"
)
exit /b 0

:try_start_local
if exist "%~1" (
    call :setup_pixi_runtime
    start "" "%~1" %FREECAD_ARGS%
    set "FREECAD_STARTED=1"
)
exit /b 0

:setup_pixi_runtime
if exist "%PIXI_LIBRARY%\bin" set "PATH=%PIXI_LIBRARY%\bin;%PATH%"
if exist "%PIXI_LIBRARY%\lib\qt6\bin" set "PATH=%PIXI_LIBRARY%\lib\qt6\bin;%PATH%"
if exist "%PIXI_LIBRARY%\mingw-w64\bin" set "PATH=%PIXI_LIBRARY%\mingw-w64\bin;%PATH%"
if exist "%PIXI_LIBRARY%\usr\bin" set "PATH=%PIXI_LIBRARY%\usr\bin;%PATH%"
if exist "%PIXI_ENV%\DLLs" set "PATH=%PIXI_ENV%\DLLs;%PATH%"
if exist "%PIXI_ENV%" set "PATH=%PIXI_ENV%;%PATH%"
if exist "%PIXI_LIBRARY%\lib\qt6\plugins" set "QT_PLUGIN_PATH=%PIXI_LIBRARY%\lib\qt6\plugins"
if exist "%PIXI_LIBRARY%\lib\qt6\plugins\platforms" set "QT_QPA_PLATFORM_PLUGIN_PATH=%PIXI_LIBRARY%\lib\qt6\plugins\platforms"
if exist "%PIXI_LIBRARY%\lib\qt6\qml" set "QML2_IMPORT_PATH=%PIXI_LIBRARY%\lib\qt6\qml"
exit /b 0

:install_sim_workbench
set "SIMWB_INSTALLER=%ROOT%addons\SimWorkbench\install_addon.py"
if not exist "%SIMWB_INSTALLER%" exit /b 0

set "SIMWB_PYTHON="
if exist "%PIXI_ENV%\python.exe" set "SIMWB_PYTHON=%PIXI_ENV%\python.exe"
if not defined SIMWB_PYTHON (
    where python >nul 2>nul
    if not errorlevel 1 set "SIMWB_PYTHON=python"
)
if not defined SIMWB_PYTHON (
    echo [WARN] Could not install SimWorkbench: python was not found.
    exit /b 0
)

set "PYTHONIOENCODING=utf-8"
"%SIMWB_PYTHON%" "%SIMWB_INSTALLER%"
if errorlevel 1 (
    echo [WARN] SimWorkbench install failed. FreeCAD will still start.
)
exit /b 0

:done
popd >nul
exit /b 0
