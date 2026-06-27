# SPDX-License-Identifier: LGPL-2.1-or-later
[CmdletBinding()]
param(
    [string]$StatusFile,

    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$RemainingArgs
)

$ErrorActionPreference = "Stop"

$Root = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$LogDir = Join-Path $Root ".log"
New-Item -ItemType Directory -Force -Path $LogDir | Out-Null

$Stamp = Get-Date -Format "yyyyMMdd-HHmmss"
$MainLog = Join-Path $LogDir "start-all-$Stamp.log"
$script:WslCommandId = 0

if ($StatusFile) {
    $statusDir = Split-Path -Parent $StatusFile
    if ($statusDir) {
        New-Item -ItemType Directory -Force -Path $statusDir | Out-Null
    }
    Remove-Item -LiteralPath $StatusFile -Force -ErrorAction SilentlyContinue
}

function Write-Log {
    param([string]$Message)
    Add-Content -LiteralPath $MainLog -Value ("[{0}] {1}" -f (Get-Date -Format "yyyy-MM-dd HH:mm:ss"), $Message)
}

function Write-StatusLine {
    param([string]$Message)
    if ($StatusFile) {
        Add-Content -LiteralPath $StatusFile -Value $Message
    } else {
        Write-Output $Message
    }
}

trap {
    Write-Log "Unhandled error: $($_.Exception.Message)"
    if ($_.InvocationInfo -and $_.InvocationInfo.ScriptLineNumber) {
        Write-Log "Unhandled error line: $($_.InvocationInfo.ScriptLineNumber)"
    }
    Write-StatusLine "Start-All failed - see $MainLog"
    exit 1
}

function Write-Running {
    param([string]$Name)
    Write-Log "$Name running"
    Write-StatusLine "$Name running"
}

function Stop-WithFailure {
    param(
        [string]$Name,
        [string]$Reason
    )
    Write-Log "$Name not running: $Reason"
    Write-StatusLine "$Name not running - see $MainLog"
    exit 1
}

function Quote-Bash {
    param([string]$Value)
    return "'" + $Value.Replace("'", "'\''") + "'"
}

function Start-LoggedProcess {
    param(
        [string]$Name,
        [string]$FilePath,
        [string[]]$ArgumentList,
        [string]$WorkingDirectory,
        [string]$StdOutLog,
        [string]$StdErrLog
    )

    Write-Log ("Starting {0}: {1} {2}" -f $Name, $FilePath, ($ArgumentList -join " "))
    $process = Start-Process `
        -FilePath $FilePath `
        -ArgumentList $ArgumentList `
        -WorkingDirectory $WorkingDirectory `
        -WindowStyle Hidden `
        -RedirectStandardOutput $StdOutLog `
        -RedirectStandardError $StdErrLog `
        -PassThru
    Write-Log "$Name launcher PID: $($process.Id)"
    return $process
}

function Wait-NewProcess {
    param(
        [string]$ProcessName,
        [int[]]$ExistingIds,
        [int]$TimeoutSeconds
    )

    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $matches = @(
            Get-Process -Name $ProcessName -ErrorAction SilentlyContinue |
                Where-Object { $ExistingIds -notcontains $_.Id }
        )
        if ($matches.Count -gt 0) {
            return $matches
        }
        Start-Sleep -Milliseconds 500
    }
    return @()
}

function Find-FreeCadExecutable {
    $candidates = @(
        (Join-Path $Root ".pixi\envs\default\Library\bin\FreeCAD.exe"),
        (Join-Path $Root "build\release\bin\FreeCAD.exe"),
        (Join-Path $Root "build\debug\bin\FreeCAD.exe")
    )

    foreach ($base in @($env:ProgramFiles, ${env:ProgramFiles(x86)}, $env:LOCALAPPDATA)) {
        if (-not $base) {
            continue
        }
        $searchRoot = if ($base -eq $env:LOCALAPPDATA) {
            Join-Path $base "Programs"
        } else {
            $base
        }
        Get-ChildItem -Path (Join-Path $searchRoot "FreeCAD*") -Directory -ErrorAction SilentlyContinue |
            ForEach-Object {
                $candidates += Join-Path $_.FullName "bin\FreeCAD.exe"
                $candidates += Join-Path $_.FullName "FreeCAD.exe"
            }
    }

    $pathEntry = Get-Command "FreeCAD.exe" -ErrorAction SilentlyContinue
    if ($pathEntry) {
        $candidates += $pathEntry.Source
    }

    foreach ($candidate in $candidates) {
        if ($candidate -and (Test-Path -LiteralPath $candidate -PathType Leaf)) {
            return (Resolve-Path -LiteralPath $candidate).Path
        }
    }

    return $null
}

function Set-FreeCadMcpSettings {
    $settings = [ordered]@{
        remote_enabled = $true
        allowed_ips = "127.0.0.1, 172.16.0.0/12"
        auto_start_rpc = $true
    }
    $json = $settings | ConvertTo-Json -Depth 3
    $settingsPaths = @(
        (Join-Path $env:APPDATA "FreeCAD\freecad_mcp_settings.json"),
        (Join-Path $env:APPDATA "FreeCAD\v1-2\freecad_mcp_settings.json")
    )
    foreach ($path in $settingsPaths) {
        $dir = Split-Path -Parent $path
        New-Item -ItemType Directory -Force -Path $dir | Out-Null
        [System.IO.File]::WriteAllText($path, $json + [Environment]::NewLine, [System.Text.UTF8Encoding]::new($false))
        Write-Log "Wrote FreeCAD MCP settings: $path"
    }
}

function Install-SimWorkbenchAddon {
    $installer = Join-Path $Root "addons\SimWorkbench\install_addon.py"
    if (-not (Test-Path -LiteralPath $installer -PathType Leaf)) {
        Write-Log "SimWorkbench installer not found: $installer"
        return
    }

    $pythonCandidates = @(
        (Join-Path $Root ".pixi\envs\default\python.exe"),
        "python"
    )
    $python = $null
    foreach ($candidate in $pythonCandidates) {
        if ($candidate -eq "python") {
            $cmd = Get-Command "python" -ErrorAction SilentlyContinue
            if ($cmd) {
                $python = $cmd.Source
                break
            }
        } elseif (Test-Path -LiteralPath $candidate -PathType Leaf) {
            $python = $candidate
            break
        }
    }

    if (-not $python) {
        Write-Log "SimWorkbench install skipped: python was not found"
        Write-StatusLine "SimWorkbench install skipped - python was not found"
        return
    }

    $outLog = Join-Path $LogDir "start-all-$Stamp-simworkbench-install.out.log"
    $errLog = Join-Path $LogDir "start-all-$Stamp-simworkbench-install.err.log"
    Write-Log "Installing SimWorkbench addon with $python"
    $env:PYTHONIOENCODING = "utf-8"
    $process = Start-Process `
        -FilePath $python `
        -ArgumentList @($installer) `
        -WorkingDirectory $Root `
        -WindowStyle Hidden `
        -RedirectStandardOutput $outLog `
        -RedirectStandardError $errLog `
        -PassThru
    $process.WaitForExit()
    if ($process.ExitCode -eq 0) {
        Write-Log "SimWorkbench addon installed"
        Write-StatusLine "SimWorkbench addon installed"
    } else {
        Write-Log "SimWorkbench install failed with exit code $($process.ExitCode); see $outLog / $errLog"
        Write-StatusLine "SimWorkbench install failed - FreeCAD will still start"
    }
}

function Invoke-WslBash {
    param([string]$Command)
    $script:WslCommandId += 1
    $scriptPath = Join-Path $LogDir ("start-all-{0}-wsl-{1}.sh" -f $Stamp, $script:WslCommandId)
    $lfCommand = ($Command -replace "`r`n", "`n") -replace "`r", "`n"
    [System.IO.File]::WriteAllText($scriptPath, $lfCommand, [System.Text.UTF8Encoding]::new($false))

    try {
        $wslScriptPath = Get-WslPath $scriptPath
        $output = & wsl.exe -- bash $wslScriptPath 2>> $MainLog
        return @{
            ExitCode = $LASTEXITCODE
            Output = @($output)
        }
    } finally {
        Remove-Item -LiteralPath $scriptPath -Force -ErrorAction SilentlyContinue
    }
}

function Get-WslPath {
    param([string]$WindowsPath)
    $normalizedPath = $WindowsPath.Replace("\", "/")
    $result = & wsl.exe -- wslpath -a $normalizedPath 2>> $MainLog
    if ($LASTEXITCODE -ne 0 -or -not $result) {
        throw "Could not convert Windows path to WSL path: $WindowsPath"
    }
    return ([string]$result).Trim()
}

function Wait-WslDockerContainer {
    param(
        [string]$Name,
        [int]$TimeoutSeconds
    )

    $nameQ = Quote-Bash $Name
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $result = Invoke-WslBash "docker inspect -f '{{.State.Running}}' $nameQ 2>/dev/null || true"
        $state = ($result.Output -join "`n").Trim()
        if ($state -eq "true") {
            Write-Log "Docker container $Name is running"
            return $true
        }
        Start-Sleep -Seconds 2
    }
    return $false
}

function Wait-WslProcess {
    param(
        [string]$Name,
        [string]$Pattern,
        [int]$TimeoutSeconds
    )

    $patternQ = Quote-Bash $Pattern
    $deadline = (Get-Date).AddSeconds($TimeoutSeconds)
    while ((Get-Date) -lt $deadline) {
        $result = Invoke-WslBash "pgrep -af -- $patternQ >/dev/null 2>&1"
        if ($result.ExitCode -eq 0) {
            Write-Log "$Name process matched: $Pattern"
            return $true
        }
        Start-Sleep -Milliseconds 500
    }
    return $false
}

function Ensure-WslVenv {
    param(
        [string]$Name,
        [string]$WslDir,
        [string]$EntryPoint
    )

    $dirQ = Quote-Bash $WslDir
    $entryPath = ".venv/bin/$EntryPoint"
    $cmd = @"
set -e
cd $dirQ
if [ ! -x "$entryPath" ]; then
    if [ ! -x ".venv/bin/python3" ]; then
        python3 -m venv .venv 2>/dev/null || python3 -m venv --without-pip .venv
    fi
    if [ ! -x ".venv/bin/pip" ]; then
        curl -sS https://bootstrap.pypa.io/get-pip.py | .venv/bin/python3
    fi
    .venv/bin/pip install -e . -q
fi
test -x "$entryPath"
"@

    $result = Invoke-WslBash $cmd
    if ($result.ExitCode -ne 0) {
        Stop-WithFailure $Name "could not prepare WSL venv at $WslDir"
    }
}

function Install-GazeboGeometryStub {
    param([string]$WslDir)

    $dirQ = Quote-Bash $WslDir
    $cmd = @'
set -e
cd __DIR__
site="$(.venv/bin/python3 -c 'import sys; print(next(p for p in sys.path if "site-packages" in p and ".venv" in p))')"
mkdir -p "$site/geometry_msgs/msg"
: > "$site/geometry_msgs/__init__.py"
{
    printf '%s\n' 'class _M:'
    printf '%s\n' '    def __init__(self, **k):'
    printf '%s\n' '        for a, b in k.items(): setattr(self, a, b)'
    printf '%s\n' 'class Pose(_M): pass'
    printf '%s\n' 'class Twist(_M): pass'
    printf '%s\n' 'class Vector3(_M): pass'
    printf '%s\n' 'class Quaternion(_M): pass'
    printf '%s\n' 'class Point(_M): pass'
    printf '%s\n' 'class Wrench(_M): pass'
    printf '%s\n' 'class Transform(_M): pass'
    printf '%s\n' 'class PoseStamped(_M): pass'
    printf '%s\n' 'class TwistStamped(_M): pass'
} > "$site/geometry_msgs/msg/__init__.py"
'@.Replace("__DIR__", $dirQ)

    $result = Invoke-WslBash $cmd
    if ($result.ExitCode -ne 0) {
        Write-Log "geometry_msgs stub install failed for gazebo-mcp"
    }
}

function Start-WslMcp {
    param(
        [string]$Name,
        [string]$WslDir,
        [string]$EntryPoint,
        [string[]]$ExtraArgs = @(),
        [string]$OutLog,
        [string]$ErrLog
    )

    $entryPath = "$WslDir/.venv/bin/$EntryPoint"
    $dirQ = Quote-Bash $WslDir
    $entryQ = Quote-Bash $entryPath
    $quotedArgs = @($ExtraArgs | ForEach-Object { Quote-Bash $_ })
    $cmd = "cd $dirQ && tail -f /dev/null | PYTHONUNBUFFERED=1 $entryQ $($quotedArgs -join ' ')"
    [void](Start-LoggedProcess `
        -Name $Name `
        -FilePath "wsl.exe" `
        -ArgumentList @("--", "bash", "-lc", $cmd) `
        -WorkingDirectory $Root `
        -StdOutLog $OutLog `
        -StdErrLog $ErrLog)
    return $entryPath
}

Write-Log "Starting full stack from $Root"
if ($RemainingArgs.Count -gt 0) {
    Write-Log "Arguments: $($RemainingArgs -join ' ')"
}

Set-FreeCadMcpSettings
Install-SimWorkbenchAddon

$freeCadBefore = @(
    Get-Process -Name "FreeCAD" -ErrorAction SilentlyContinue |
        Select-Object -ExpandProperty Id
)
Write-Log "Existing FreeCAD PIDs before launch: $($freeCadBefore -join ', ')"

$freeCadExe = Find-FreeCadExecutable
if (-not $freeCadExe) {
    Stop-WithFailure "FreeCAD" "FreeCAD.exe was not found"
}
$freeCadArgs = @($RemainingArgs | Where-Object { -not [string]::IsNullOrWhiteSpace($_) })
$freeCadStart = @{
    FilePath = $freeCadExe
    WorkingDirectory = $Root
    PassThru = $true
    RedirectStandardOutput = (Join-Path $LogDir "start-all-$Stamp-freecad.out.log")
    RedirectStandardError = (Join-Path $LogDir "start-all-$Stamp-freecad.err.log")
}
if ($freeCadArgs.Count -gt 0) {
    $freeCadStart.ArgumentList = $freeCadArgs
}
$freeCadProcess = Start-Process @freeCadStart
Write-Log "Started FreeCAD from $freeCadExe with launcher PID $($freeCadProcess.Id)"

$newFreeCad = @(Wait-NewProcess -ProcessName "FreeCAD" -ExistingIds $freeCadBefore -TimeoutSeconds 45)
if ($newFreeCad.Count -eq 0) {
    $allFreeCad = @(
        Get-Process -Name "FreeCAD" -ErrorAction SilentlyContinue |
            Select-Object -ExpandProperty Id
    )
    Write-Log "FreeCAD PIDs after launch: $($allFreeCad -join ', ')"
    Stop-WithFailure "FreeCAD" "no new FreeCAD process was detected"
}
Write-Log "New FreeCAD PIDs: $($newFreeCad.Id -join ', ')"
Write-Running "FreeCAD"

try {
    $wslRoot = Get-WslPath $Root
    Write-Log "WSL root: $wslRoot"
} catch {
    Stop-WithFailure "WSL" $_.Exception.Message
}

$gzOutLog = Join-Path $LogDir "start-all-$Stamp-gz-sim.out.log"
$gzErrLog = Join-Path $LogDir "start-all-$Stamp-gz-sim.err.log"
$rosOutLog = Join-Path $LogDir "start-all-$Stamp-ros2.out.log"
$rosErrLog = Join-Path $LogDir "start-all-$Stamp-ros2.err.log"

[void](Start-LoggedProcess `
    -Name "gz-sim" `
    -FilePath $env:ComSpec `
    -ArgumentList @("/d", "/c", "call `"$Root\Start-gz-sim.bat`"") `
    -WorkingDirectory $Root `
    -StdOutLog $gzOutLog `
    -StdErrLog $gzErrLog)

[void](Start-LoggedProcess `
    -Name "ros2" `
    -FilePath $env:ComSpec `
    -ArgumentList @("/d", "/c", "call `"$Root\Start-ros2.bat`"") `
    -WorkingDirectory $Root `
    -StdOutLog $rosOutLog `
    -StdErrLog $rosErrLog)

if (-not (Wait-WslDockerContainer -Name "gz-sim-sever" -TimeoutSeconds 120)) {
    Stop-WithFailure "gz-sim" "Docker container gz-sim-sever was not running after timeout"
}
Write-Running "gz-sim"

if (-not (Wait-WslDockerContainer -Name "ros2-server" -TimeoutSeconds 120)) {
    Stop-WithFailure "ros2" "Docker container ros2-server was not running after timeout"
}
Write-Running "ros2"

$freeCadMcpDir = "$wslRoot/tools/mcp/freecad-mcp"
$rosMcpDir = "$wslRoot/tools/mcp/ros-mcp-server"
$gazeboMcpDir = "$wslRoot/tools/mcp/gazebo-mcp"
$windowsHostResult = Invoke-WslBash "ip route show default | awk '{print `$3; exit}'"
$windowsHostFromWsl = ($windowsHostResult.Output -join "`n").Trim()
if (-not $windowsHostFromWsl) {
    Stop-WithFailure "mcp-freecad" "could not determine Windows host IP from WSL"
}
Write-Log "Windows host from WSL: $windowsHostFromWsl"

Ensure-WslVenv -Name "mcp-freecad" -WslDir $freeCadMcpDir -EntryPoint "freecad-mcp"
$freeCadMcpOut = Join-Path $LogDir "start-all-$Stamp-mcp-freecad.out.log"
$freeCadMcpErr = Join-Path $LogDir "start-all-$Stamp-mcp-freecad.err.log"
$freeCadMcpEntry = Start-WslMcp -Name "mcp-freecad" -WslDir $freeCadMcpDir -EntryPoint "freecad-mcp" -ExtraArgs @("--host", $windowsHostFromWsl) -OutLog $freeCadMcpOut -ErrLog $freeCadMcpErr
if (-not (Wait-WslProcess -Name "mcp-freecad" -Pattern $freeCadMcpEntry -TimeoutSeconds 30)) {
    Stop-WithFailure "mcp-freecad" "process was not running after timeout"
}
Write-Running "mcp-freecad"

Ensure-WslVenv -Name "mcp-ros2" -WslDir $rosMcpDir -EntryPoint "ros-mcp"
$rosMcpOut = Join-Path $LogDir "start-all-$Stamp-mcp-ros2.out.log"
$rosMcpErr = Join-Path $LogDir "start-all-$Stamp-mcp-ros2.err.log"
$rosMcpEntry = Start-WslMcp -Name "mcp-ros2" -WslDir $rosMcpDir -EntryPoint "ros-mcp" -OutLog $rosMcpOut -ErrLog $rosMcpErr
if (-not (Wait-WslProcess -Name "mcp-ros2" -Pattern $rosMcpEntry -TimeoutSeconds 30)) {
    Stop-WithFailure "mcp-ros2" "process was not running after timeout"
}
Write-Running "mcp-ros2"

Ensure-WslVenv -Name "mcp-gz-sim" -WslDir $gazeboMcpDir -EntryPoint "gazebo-mcp-server"
Install-GazeboGeometryStub -WslDir $gazeboMcpDir
$gazeboMcpOut = Join-Path $LogDir "start-all-$Stamp-mcp-gz-sim.out.log"
$gazeboMcpErr = Join-Path $LogDir "start-all-$Stamp-mcp-gz-sim.err.log"
$gazeboMcpEntry = Start-WslMcp -Name "mcp-gz-sim" -WslDir $gazeboMcpDir -EntryPoint "gazebo-mcp-server" -OutLog $gazeboMcpOut -ErrLog $gazeboMcpErr
if (-not (Wait-WslProcess -Name "mcp-gz-sim" -Pattern $gazeboMcpEntry -TimeoutSeconds 30)) {
    Stop-WithFailure "mcp-gz-sim" "process was not running after timeout"
}
Write-Running "mcp-gz-sim"

Write-Log "Full stack startup completed"
exit 0
