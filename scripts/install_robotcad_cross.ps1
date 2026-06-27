#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Clone or update RobotCAD (CROSS / OVERCROSS) into the FreeCAD user Mod directory.

.DESCRIPTION
    Phase 0 helper: reproducible install without relying on Addon Manager UI.
    After install, restart FreeCAD and enable the RobotCAD / CROSS workbench from
    Edit → Preferences → Workbenches.

.PARAMETER ModRoot
    FreeCAD user Mod folder. Default: %APPDATA%\FreeCAD\v1-2\Mod

.PARAMETER RepoUrl
    Git remote for RobotCAD/CROSS (OVERCROSS fork maintained for recent FreeCAD).

.PARAMETER CloneDirName
    Directory name under ModRoot (must match how the addon expects to be loaded).

.EXAMPLE
    .\scripts\install_robotcad_cross.ps1
.EXAMPLE
    .\scripts\install_robotcad_cross.ps1 -ModRoot "D:\FreeCADMods"
#>
param(
    [string]$ModRoot = "",
    [string]$RepoUrl = "https://github.com/drfenixion/freecad.overcross.git",
    [string]$CloneDirName = "freecad.overcross"
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

if (-not $ModRoot) {
    $ModRoot = Join-Path $env:APPDATA "FreeCAD\v1-2\Mod"
}

if (-not (Test-Path -LiteralPath $ModRoot)) {
    Write-Host "Creating Mod directory: $ModRoot"
    New-Item -ItemType Directory -Path $ModRoot -Force | Out-Null
}

$target = Join-Path $ModRoot $CloneDirName

if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Write-Error "git was not found on PATH. Install Git for Windows and retry."
    exit 1
}

Push-Location $ModRoot
try {
    if (Test-Path -LiteralPath $target) {
        Write-Host "Updating existing clone: $target"
        Push-Location $target
        try {
            git fetch --prune origin
            git pull --ff-only origin HEAD
        }
        finally {
            Pop-Location
        }
    }
    else {
        Write-Host "Cloning $RepoUrl -> $target"
        git clone --depth 1 $RepoUrl $CloneDirName
    }
}
finally {
    Pop-Location
}

# Python deps for URDF import/export (FreeCAD AdditionalPythonPackages, matches embedded py311)
$pyPkgs = Join-Path $env:USERPROFILE ".local\share\FreeCAD\AdditionalPythonPackages\py311"
New-Item -ItemType Directory -Force -Path $pyPkgs | Out-Null

$pipPython = $null
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$pixiPy = Join-Path $repoRoot ".pixi\envs\default\python.exe"
if (Test-Path -LiteralPath $pixiPy) {
    $pipPython = $pixiPy
}
elseif (Get-Command python -ErrorAction SilentlyContinue) {
    $pipPython = (Get-Command python).Source
}

if ($pipPython) {
    Write-Host "Installing RobotCAD Python deps into $pyPkgs ..."
    $deps = @(
        "urdf-parser-py",
        "xacro",
        "xmltodict",
        "pycollada",
        "lxml"
    )
    & $pipPython -m pip install @deps --target $pyPkgs --disable-pip-version-check -q
    Write-Host "[OK] Python dependencies installed for FreeCAD py311"
}
else {
    Write-Host "[WARN] No Python found for pip; install deps manually into $pyPkgs"
}

# OVERCROSS still resolves MOD_PATH as Mod/freecad.robotcad — junction for pixi FreeCAD.
$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$pixiDataMod = Join-Path $repoRoot ".pixi\envs\default\Library\data\Mod"
$robotcadLink = Join-Path $pixiDataMod "freecad.robotcad"
if ((Test-Path -LiteralPath $pixiDataMod) -and -not (Test-Path -LiteralPath $robotcadLink)) {
    New-Item -ItemType Directory -Force -Path $pixiDataMod | Out-Null
    cmd /c mklink /J "$robotcadLink" "$target" | Out-Null
    Write-Host "[OK] Junction: $robotcadLink -> $target"
}

Push-Location $target
try {
    if (Test-Path -LiteralPath ".gitmodules") {
        Write-Host "Initializing git submodules (ros2_controllers, sdformat, ...)..."
        git submodule update --init --depth 1 2>&1 | Out-Host
    }
}
finally {
    Pop-Location
}

Write-Host ""
Write-Host "[OK] RobotCAD/CROSS addon path: $target"
Write-Host "Verify (FreeCADCmd): .\.pixi\envs\default\Library\bin\FreeCADCmd.exe scripts\verify_robotcad_cross.py"
Write-Host "Build FCStd (GUI + RPC :9875): python scripts\build_arm_2dof_fcstd_rpc.py"
Write-Host "Export URDF (GUI + RPC :9875): python scripts\export_arm_2dof_rpc.py"
Write-Host "Next: restart FreeCAD, enable the CROSS workbench, then open robots\arm_2dof.FCStd"
