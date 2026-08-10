# SPDX-License-Identifier: LGPL-2.1-or-later

[CmdletBinding()]
param(
    [ValidateSet('off', 'on')]
    [string]$Mode = 'off',
    [string]$Image = '127.0.0.1:5001/freecad-ci-deps:24.04',
    [string]$CacheVolume = 'freecad-photo-inspection-ccache-v1',
    [ValidateRange(1, 32)]
    [int]$BuildJobs = 8
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$stamp = '{0}-{1}' -f (Get-Date).ToUniversalTime().ToString('yyyyMMddTHHmmssZ'), $PID
$volume = "freecad-photo-inspection-$stamp"
$logDir = Join-Path $repoRoot "build\photo-inspection-ci\$stamp-$Mode"
New-Item -ItemType Directory -Force -Path $logDir | Out-Null

$sourceRevision = (& git -C $repoRoot rev-parse HEAD).Trim()
$manifestPaths = @(
    'cMake/FreeCAD_Helpers/InitializeFreeCADBuildOptions.cmake',
    'cMake/FreeCAD_Helpers/PrintFinalReport.cmake',
    'src/Mod/Inspection',
    'tests/src/Mod/Inspection',
    'tests/CMakeLists.txt',
    'tests/src/Mod/CMakeLists.txt',
    'scripts/ci/photo-inspection-validate-container.sh',
    'scripts/ci/photo-inspection-validate.ps1',
    'scripts/ci/docker/photo-inspection-opencv46.Dockerfile',
    'scripts/ci/docker/photo-inspection-opencv-current.Dockerfile',
    'scripts/ci/docker/photo-inspection-opencv413-source.Dockerfile'
)
$manifestLines = foreach ($relativePath in $manifestPaths) {
    $absolutePath = Join-Path $repoRoot $relativePath
    if (Test-Path -LiteralPath $absolutePath -PathType Leaf) {
        $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $absolutePath).Hash
        "$hash  $relativePath"
    }
    elseif (Test-Path -LiteralPath $absolutePath -PathType Container) {
        Get-ChildItem -LiteralPath $absolutePath -File -Recurse |
            Sort-Object FullName |
            ForEach-Object {
                $hash = (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash
                $relative = [IO.Path]::GetRelativePath($repoRoot, $_.FullName)
                "$hash  $relative"
            }
    }
}
$manifestText = ($manifestLines -join "`n") + "`n"
$manifestFile = Join-Path $logDir 'source-manifest.txt'
[IO.File]::WriteAllText($manifestFile, $manifestText, [Text.UTF8Encoding]::new($false))
$manifestHash = (Get-FileHash -Algorithm SHA256 -LiteralPath $manifestFile).Hash

& docker image inspect $Image | Out-Null
if ($LASTEXITCODE -ne 0) {
    throw "Docker image is unavailable: $Image"
}
$imageId = (& docker image inspect --format '{{.Id}}' $Image).Trim()

try {
    & docker volume create $volume | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to create Docker volume $volume"
    }
    & docker volume create $CacheVolume | Out-Null
    if ($LASTEXITCODE -ne 0) {
        throw "Unable to create Docker compiler-cache volume $CacheVolume"
    }

    & docker run --rm --network none `
        --mount "type=volume,src=$volume,dst=/data" `
        --mount "type=volume,src=$CacheVolume,dst=/cache" `
        $Image `
        bash -lc 'mkdir -p /data/src /data/build /data/home /cache && chown -R 1000:1000 /data /cache'
    if ($LASTEXITCODE -ne 0) {
        throw 'Unable to initialize the disposable validation volume'
    }

    $dockerArguments = @(
        'run', '--rm',
        '--network', 'none',
        '--cpus', '8',
        '--memory', '12g',
        '--pids-limit', '2048',
        '--user', '1000:1000',
        '--env', "PHOTO_INSPECTION_OPENCV_MODE=$Mode",
        '--env', "PHOTO_INSPECTION_BUILD_JOBS=$BuildJobs",
        '--env', "SOURCE_REV=$sourceRevision",
        '--env', "SOURCE_MANIFEST_SHA256=$manifestHash",
        '--env', "VALIDATION_IMAGE=$Image",
        '--env', "VALIDATION_IMAGE_ID=$imageId",
        '--mount', "type=bind,src=$repoRoot,dst=/src,readonly",
        '--mount', "type=bind,src=$logDir,dst=/out",
        '--mount', "type=volume,src=$volume,dst=/data",
        '--mount', "type=volume,src=$CacheVolume,dst=/cache",
        $Image,
        'bash', '/src/scripts/ci/photo-inspection-validate-container.sh'
    )

    Write-Host "Running isolated photo-inspection validation: mode=$Mode"
    & docker @dockerArguments
    if ($LASTEXITCODE -ne 0) {
        throw "Photo-inspection Docker validation failed. Logs: $logDir"
    }

    Write-Host "Validation passed. Logs: $logDir"
}
finally {
    & docker volume rm -f $volume | Out-Null
}
