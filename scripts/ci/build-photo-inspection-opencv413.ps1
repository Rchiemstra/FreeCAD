# SPDX-License-Identifier: LGPL-2.1-or-later

[CmdletBinding()]
param(
    [string]$BaseImage = '127.0.0.1:5001/freecad-ci-deps:24.04',
    [string]$Image = 'freecad-photo-inspection-opencv:4.13-source'
)

$ErrorActionPreference = 'Stop'
$repoRoot = (Resolve-Path -LiteralPath (Join-Path $PSScriptRoot '..\..')).Path
$dockerfile = Join-Path $repoRoot 'scripts\ci\docker\photo-inspection-opencv413-source.Dockerfile'

& docker build `
    --build-arg "BASE_IMAGE=$BaseImage" `
    --tag $Image `
    --file $dockerfile `
    $repoRoot
if ($LASTEXITCODE -ne 0) {
    throw 'OpenCV 4.13 source-lane image build failed'
}

& docker run --rm --network none $Image sh -c `
    'test -f "$OpenCV_DIR/OpenCVConfig.cmake" && grep -Fq "4.13.0" "$OpenCV_DIR/OpenCVConfig-version.cmake"'
if ($LASTEXITCODE -ne 0) {
    throw 'OpenCV 4.13 source-lane image probe failed'
}
