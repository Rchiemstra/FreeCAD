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

Write-Host ""
Write-Host "[OK] RobotCAD/CROSS addon path: $target"
Write-Host "Next: restart FreeCAD, enable the workbench, then run the workbench demo / export smoke test."
