# Launch the branch-built FreeCAD release binary with pixi runtime DLLs on PATH.
#
# Dev builds on Windows do not bundle Qt, Python, OCCT, or MSVC runtime DLLs next to
# FreeCAD.exe. Starting the executable directly fails with exit 0xC0000135
# (STATUS_DLL_NOT_FOUND). This script runs the build binary under pixi so those
# dependencies resolve.
#
# Examples:
#   .\run_freecad_release.ps1
#   .\run_freecad_release.ps1 chess_pawn.FCStd
#   .\run_freecad_release.ps1 -Cmd -c "import FreeCAD; print(FreeCAD.Version())"
#   .\run_freecad_release.ps1 -Preset debug

[CmdletBinding()]
param(
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]] $FreeCADArgs,

    [string] $Preset = "release",
    [switch] $Cmd
)

$ErrorActionPreference = "Stop"

function Find-RepoRoot {
    param([string] $StartDir)
    $dir = (Resolve-Path $StartDir).Path
    while ($dir) {
        if (Test-Path (Join-Path $dir "src\Main") -PathType Container) {
            return $dir
        }
        $parent = Split-Path $dir -Parent
        if (-not $parent -or $parent -eq $dir) {
            break
        }
        $dir = $parent
    }
    throw "Could not find FreeCAD repo root (expected src\Main directory)."
}

$repoRoot = Find-RepoRoot $PSScriptRoot
$buildDir = Join-Path $repoRoot "build"
$exeName = if ($Cmd) { "FreeCADCmd.exe" } else { "FreeCAD.exe" }
$freecadExe = Join-Path (Join-Path (Join-Path $buildDir $Preset) "bin") $exeName

if (-not (Test-Path $freecadExe -PathType Leaf)) {
    $candidates = Get-ChildItem -Path $buildDir -Recurse -Filter $exeName -ErrorAction SilentlyContinue |
        Where-Object { $_.Directory.Name -eq "bin" } |
        Sort-Object LastWriteTime -Descending
    if ($candidates) {
        $freecadExe = $candidates[0].FullName
    }
}

if (-not (Test-Path $freecadExe -PathType Leaf)) {
    throw "FreeCAD executable not found. Build first or pass -Preset <name>. Looked under $buildDir"
}

$pixiToml = Join-Path $repoRoot "pixi.toml"
$pixiCmd = Get-Command pixi -ErrorAction SilentlyContinue
$pixiExe = if ($pixiCmd) { $pixiCmd.Source } else { $null }

if ($pixiToml -and $pixiExe) {
    $relativeExe = $freecadExe.Substring($repoRoot.Length).TrimStart("\", "/")
    Push-Location $repoRoot
    try {
        & $pixiExe run $relativeExe @FreeCADArgs
        exit $LASTEXITCODE
    }
    finally {
        Pop-Location
    }
}

# Fallback: augment PATH from a local pixi env without invoking pixi run.
$pixiEnv = Join-Path $repoRoot ".pixi\envs\default"
if (-not (Test-Path $pixiEnv -PathType Container)) {
    throw "pixi is unavailable and .pixi\envs\default was not found. Install pixi or run from pixi shell."
}

$pathEntries = @(
    (Split-Path $freecadExe -Parent)
    $pixiEnv
    (Join-Path $pixiEnv "Library\bin")
    (Join-Path $pixiEnv "Library\lib")
    (Join-Path $pixiEnv "Library\lib\qt6\bin")
    (Join-Path $pixiEnv "Library\lib\qt6\plugins")
)

$modDir = Join-Path (Split-Path (Split-Path $freecadExe -Parent) -Parent) "Mod"
if (Test-Path $modDir -PathType Container) {
    foreach ($child in Get-ChildItem $modDir -Directory) {
        if (Get-ChildItem $child.FullName -Filter "*.pyd" -ErrorAction SilentlyContinue) {
            $pathEntries += $child.FullName
        }
    }
}

$env:PATH = ($pathEntries + $env:PATH) -join [System.IO.Path]::PathSeparator
$env:CONDA_PREFIX = $pixiEnv
$pluginRoot = Join-Path $pixiEnv "Library\lib\qt6\plugins"
$env:QT_PLUGIN_PATH = $pluginRoot
$env:QT_QPA_PLATFORM_PLUGIN_PATH = Join-Path $pluginRoot "platforms"

& $freecadExe @FreeCADArgs
exit $LASTEXITCODE
