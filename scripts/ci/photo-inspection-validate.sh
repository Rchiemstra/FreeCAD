#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later

set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
MODE="${1:-off}"
IMAGE="${PHOTO_INSPECTION_CI_IMAGE:-127.0.0.1:5001/freecad-ci-deps:24.04}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)-$$"
VOLUME="freecad-photo-inspection-${STAMP}"
CACHE_VOLUME="${PHOTO_INSPECTION_CCACHE_VOLUME:-freecad-photo-inspection-ccache-v1}"
LOG_DIR="${PHOTO_INSPECTION_CI_LOG_DIR:-${ROOT}/build/photo-inspection-ci/${STAMP}-${MODE}}"

case "${MODE}" in
  off|on) ;;
  *)
    echo "Usage: $0 [off|on]" >&2
    exit 2
    ;;
esac

mkdir -p "${LOG_DIR}"
SOURCE_REV="$(git -C "${ROOT}" rev-parse HEAD)"
IMAGE_ID="$(docker image inspect --format '{{.Id}}' "${IMAGE}")"
MANIFEST_SHA="$(
  cd "${ROOT}"
  find \
    cMake/FreeCAD_Helpers/InitializeFreeCADBuildOptions.cmake \
    cMake/FreeCAD_Helpers/PrintFinalReport.cmake \
    src/Mod/Inspection \
    tests/src/Mod/Inspection \
    tests/CMakeLists.txt \
    tests/src/Mod/CMakeLists.txt \
    scripts/ci/photo-inspection-validate-container.sh \
    scripts/ci/photo-inspection-validate.sh \
    scripts/ci/docker/photo-inspection-opencv46.Dockerfile \
    scripts/ci/docker/photo-inspection-opencv-current.Dockerfile \
    scripts/ci/docker/photo-inspection-opencv413-source.Dockerfile \
    -type f -print0 |
    sort -z |
    xargs -0 sha256sum |
    sha256sum |
    awk '{print $1}'
)"

cleanup()
{
  docker volume rm -f "${VOLUME}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker volume create "${VOLUME}" >/dev/null
docker volume create "${CACHE_VOLUME}" >/dev/null
docker run --rm --network none \
  --mount "type=volume,src=${VOLUME},dst=/data" \
  --mount "type=volume,src=${CACHE_VOLUME},dst=/cache" \
  "${IMAGE}" \
  bash -lc 'mkdir -p /data/src /data/build /data/home /cache && chown -R 1000:1000 /data /cache'

docker run --rm \
  --network none \
  --cpus 8 \
  --memory 12g \
  --pids-limit 2048 \
  --user 1000:1000 \
  --env "PHOTO_INSPECTION_OPENCV_MODE=${MODE}" \
  --env "PHOTO_INSPECTION_BUILD_JOBS=${PHOTO_INSPECTION_BUILD_JOBS:-8}" \
  --env "SOURCE_REV=${SOURCE_REV}" \
  --env "SOURCE_MANIFEST_SHA256=${MANIFEST_SHA}" \
  --env "VALIDATION_IMAGE=${IMAGE}" \
  --env "VALIDATION_IMAGE_ID=${IMAGE_ID}" \
  --mount "type=bind,src=${ROOT},dst=/src,readonly" \
  --mount "type=bind,src=${LOG_DIR},dst=/out" \
  --mount "type=volume,src=${VOLUME},dst=/data" \
  --mount "type=volume,src=${CACHE_VOLUME},dst=/cache" \
  "${IMAGE}" \
  bash /src/scripts/ci/photo-inspection-validate-container.sh

echo "Validation passed. Logs: ${LOG_DIR}"
