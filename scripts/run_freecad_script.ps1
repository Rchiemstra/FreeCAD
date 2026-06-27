#!/usr/bin/env pwsh
<#
.SYNOPSIS
    Run a Python script inside FreeCADCmd on Windows (pixi build).

.EXAMPLE
    .\scripts\run_freecad_script.ps1 .\scripts\verify_robotcad_cross.py
#>
param(
    [Parameter(Mandatory = $true, Position = 0)]
    [string]$ScriptPath
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

$repoRoot = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$fc = Join-Path $repoRoot ".pixi\envs\default\Library\bin\FreeCADCmd.exe"
if (-not (Test-Path -LiteralPath $fc)) {
    Write-Error "FreeCADCmd not found at $fc — build FreeCAD with pixi first."
}

$resolved = (Resolve-Path -LiteralPath $ScriptPath).Path.Replace("\", "/")
$code = "import runpy, sys; rc = runpy.run_path(r'$resolved', run_name='__main__'); sys.exit(rc if isinstance(rc, int) else (0 if rc is None else 1))"

& $fc -c $code
exit $LASTEXITCODE
