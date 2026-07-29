#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later

# Runs only inside the disposable validator container. The host source is
# mounted read-only at /src; build/cache/home live in a unique Docker volume.
set -euo pipefail

MODE="${PHOTO_INSPECTION_OPENCV_MODE:-off}"
LOG_DIR=/out
SOURCE_MOUNT=/src
SOURCE_ROOT=/data/src
BUILD_DIR=/data/build

case "${MODE}" in
  off)
    OPENCV_OPTION=OFF
    ;;
  on)
    OPENCV_OPTION=ON
    ;;
  *)
    echo "Unsupported PHOTO_INSPECTION_OPENCV_MODE=${MODE}" >&2
    exit 2
    ;;
esac

mkdir -p "${LOG_DIR}" /cache /data/home
export HOME=/data/home
export XDG_CONFIG_HOME=/data/home/.config
export XDG_CACHE_HOME=/data/home/.cache
export CCACHE_DIR=/cache
ccache -M 8G >/dev/null 2>&1 || true

{
  echo "mode=${MODE}"
  echo "source_revision=${SOURCE_REV:-unknown}"
  echo "source_manifest_sha256=${SOURCE_MANIFEST_SHA256:-unknown}"
  echo "image=${VALIDATION_IMAGE:-unknown}"
  echo "image_id=${VALIDATION_IMAGE_ID:-unknown}"
  id
  uname -a
  cmake --version | head -n1
  ninja --version
  pkg-config --modversion opencv4 2>/dev/null || echo "opencv4=not-installed"
} | tee "${LOG_DIR}/environment.log"

echo "== Create disposable source snapshot ==" | tee "${LOG_DIR}/steps.log"
test ! -w "${SOURCE_MOUNT}"
git config --global --add safe.directory "${SOURCE_MOUNT}"
rm -rf "${SOURCE_ROOT}"
mkdir -p "${SOURCE_ROOT}"

# A Git archive avoids the very slow per-file copy from a Windows bind mount.
# Overlay the exact Phase 0 implementation scope so tracked modifications and
# new files are tested without an expensive whole-worktree status/diff scan.
git -C "${SOURCE_MOUNT}" archive --format=tar HEAD | tar -C "${SOURCE_ROOT}" -xf -
tar -C "${SOURCE_MOUNT}" -cf - \
  .gitignore \
  cMake/FreeCAD_Helpers/InitializeFreeCADBuildOptions.cmake \
  cMake/FreeCAD_Helpers/PrintFinalReport.cmake \
  src/Mod/Inspection \
  tests/CMakeLists.txt \
  tests/src/Mod/CMakeLists.txt \
  tests/src/Mod/Inspection \
  scripts/ci/photo-inspection-validate-container.sh \
  scripts/ci/photo-inspection-validate.ps1 \
  scripts/ci/photo-inspection-validate.sh \
  scripts/ci/docker/photo-inspection-opencv46.Dockerfile \
  scripts/ci/docker/photo-inspection-opencv-current.Dockerfile \
  scripts/ci/docker/photo-inspection-opencv413-source.Dockerfile |
  tar -C "${SOURCE_ROOT}" -xf -

# Git archives contain gitlinks but not submodule content. Copy only the
# initialized build submodules; MCP is intentionally excluded from this
# validator and never enters the disposable snapshot.
while IFS= read -r submodule_path; do
  if [[ "${submodule_path}" == "tools/mcp/freecad-mcp" ]]; then
    continue
  fi
  if [[ -d "${SOURCE_MOUNT}/${submodule_path}" ]]; then
    tar -C "${SOURCE_MOUNT}" -cf - "${submodule_path}" |
      tar -C "${SOURCE_ROOT}" -xf -
  fi
done < <(
  git -C "${SOURCE_MOUNT}" config \
    --file "${SOURCE_MOUNT}/.gitmodules" \
    --get-regexp path |
    awk '{print $2}'
)

test ! -e "${SOURCE_ROOT}/tools/mcp/freecad-mcp/src/freecad_mcp/server.py"

echo "== Validate mandatory 178-case test manifest ==" | tee -a "${LOG_DIR}/steps.log"
python3 "${SOURCE_ROOT}/tests/src/Mod/Inspection/ValidateTestManifest.py" |
  tee "${LOG_DIR}/test-manifest.log"
python3 "${SOURCE_ROOT}/tests/src/Mod/Inspection/ValidateMcpContract.py" \
  "${SOURCE_ROOT}/src/Mod/Inspection/PhotoInspectionMcpContract.json" |
  tee "${LOG_DIR}/mcp-contract.log"

echo "== Configure OpenCV-${MODE} lane ==" | tee -a "${LOG_DIR}/steps.log"
cmake -S "${SOURCE_ROOT}" -B "${BUILD_DIR}" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
  -DBUILD_GUI=ON \
  -DENABLE_DEVELOPER_TESTS=ON \
  -DFREECAD_USE_OPENCV_PHOTO_INSPECTION="${OPENCV_OPTION}" \
  >"${LOG_DIR}/configure.log" 2>&1
tail -n 60 "${LOG_DIR}/configure.log" | tee -a "${LOG_DIR}/steps.log"

echo "== Verify configure capability ==" | tee -a "${LOG_DIR}/steps.log"
grep -E \
  '^(FREECAD_USE_OPENCV_PHOTO_INSPECTION|PHOTO_INSPECTION_OPENCV_(AVAILABLE|VERSION|COMPONENTS|COMPAT_BRANCH|REASON))' \
  "${BUILD_DIR}/CMakeCache.txt" | tee "${LOG_DIR}/capability.log"

if [[ "${MODE}" == "off" ]]; then
  grep -q '^FREECAD_USE_OPENCV_PHOTO_INSPECTION:BOOL=OFF$' "${BUILD_DIR}/CMakeCache.txt"
  grep -q '^PHOTO_INSPECTION_OPENCV_AVAILABLE:INTERNAL=OFF$' "${BUILD_DIR}/CMakeCache.txt"
else
  grep -q '^FREECAD_USE_OPENCV_PHOTO_INSPECTION:BOOL=ON$' "${BUILD_DIR}/CMakeCache.txt"
  grep -q '^PHOTO_INSPECTION_OPENCV_AVAILABLE:INTERNAL=ON$' "${BUILD_DIR}/CMakeCache.txt"
fi

echo "== Build Inspection and Phase 0 tests ==" | tee -a "${LOG_DIR}/steps.log"
if ! cmake --build "${BUILD_DIR}" \
  --parallel "${PHOTO_INSPECTION_BUILD_JOBS:-4}" \
  --target Inspection InspectionGui Inspection_tests_run InspectionGui_tests_run \
  >"${LOG_DIR}/build.log" 2>&1; then
  tail -n 160 "${LOG_DIR}/build.log"
  exit 1
fi
tail -n 80 "${LOG_DIR}/build.log" | tee -a "${LOG_DIR}/steps.log"

export LD_LIBRARY_PATH="${BUILD_DIR}/lib:${LD_LIBRARY_PATH:-}"

echo "== Run Phase 0 tests ==" | tee -a "${LOG_DIR}/steps.log"
"${BUILD_DIR}/tests/Inspection_tests_run" \
  --gtest_filter='PhotoInspection*' \
  | tee "${LOG_DIR}/tests.log"

grep -Eq '\[  PASSED  \] 78 tests?\.' "${LOG_DIR}/tests.log"
echo "== Run digital print-path tests ==" | tee -a "${LOG_DIR}/steps.log"
QT_QPA_PLATFORM=offscreen "${BUILD_DIR}/tests/InspectionGui_tests_run" \
  --gtest_filter='PhotoInspection*' \
  | tee "${LOG_DIR}/gui-tests.log"

grep -Eq '\[  PASSED  \] 4 tests?\.' "${LOG_DIR}/gui-tests.log"
echo ALL_MANDATORY_PASSED | tee -a "${LOG_DIR}/steps.log"
