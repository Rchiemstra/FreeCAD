#!/usr/bin/env pwsh
<#
.SYNOPSIS
    CI-friendly headless test runner for the FreeCAD/Gazebo/MCP project.

.DESCRIPTION
    Runs all offline tests (no FreeCAD or Gazebo required).
    Exits with code 0 on success, 1 on failure.

    Usage:
        .\ci_test.ps1                  # normal run
        .\ci_test.ps1 -Verbose         # show full pytest output
        .\ci_test.ps1 -Module bridge   # run only bridge tests

.PARAMETER Verbose
    Print all test output instead of just failures.

.PARAMETER Module
    If specified, run only tests for this module (bridge, runner, iteration, workbench).

.PARAMETER Report
    Generate an HTML test report (requires pytest-html: pip install pytest-html).

.EXAMPLE
    .\ci_test.ps1
    .\ci_test.ps1 -Verbose -Module iteration
#>
param(
    [switch]$Verbose,
    [string]$Module  = "",
    [switch]$Report
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

# ---------------------------------------------------------------------------
# Resolve repo root (the directory containing this script)
# ---------------------------------------------------------------------------
$RepoRoot = Split-Path -Parent $MyInvocation.MyCommand.Path
Push-Location $RepoRoot

# ---------------------------------------------------------------------------
# Verify Python and pytest are available
# ---------------------------------------------------------------------------
if (-not (Get-Command python -ErrorAction SilentlyContinue)) {
    Write-Error "Python not found on PATH. Install Python 3.10+ and retry."
    exit 1
}

$PythonVersion = python --version 2>&1
Write-Host "Python: $PythonVersion"

if (-not (python -m pytest --version 2>&1 | Select-String "pytest")) {
    Write-Host "Installing test dependencies..."
    python -m pip install -r requirements-dev.txt --quiet
}

# ---------------------------------------------------------------------------
# Build pytest arguments
# ---------------------------------------------------------------------------
$PytestArgs = @("--tb=short")

if ($Verbose) {
    $PytestArgs += "-v"
} else {
    $PytestArgs += "-q"
}

if ($Module) {
    $ModuleMap = @{
        "bridge"     = "tests/test_bridge.py"
        "runner"     = "tests/test_runner.py"
        "iteration"  = "tests/test_iteration.py"
        "workbench"  = "tests/test_sim_workbench.py"
    }
    if ($ModuleMap.ContainsKey($Module)) {
        $PytestArgs += $ModuleMap[$Module]
    } else {
        Write-Warning "Unknown module '$Module'. Running all tests instead."
    }
}

if ($Report) {
    $ReportPath = "test-report.html"
    $PytestArgs += "--html=$ReportPath", "--self-contained-html"
    Write-Host "HTML report will be written to: $ReportPath"
}

# ---------------------------------------------------------------------------
# Run tests
# ---------------------------------------------------------------------------
Write-Host ""
Write-Host "=" * 60
Write-Host "Running offline tests (FreeCAD + Gazebo tests are auto-skipped)"
Write-Host "=" * 60
Write-Host ""

python -m pytest @PytestArgs
$ExitCode = $LASTEXITCODE

# ---------------------------------------------------------------------------
# Summary
# ---------------------------------------------------------------------------
Write-Host ""
if ($ExitCode -eq 0) {
    Write-Host "[PASS] All offline tests passed." -ForegroundColor Green
} else {
    Write-Host "[FAIL] Some tests failed. See output above." -ForegroundColor Red
}

Pop-Location
exit $ExitCode
