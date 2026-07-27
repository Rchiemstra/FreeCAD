#!/usr/bin/env bash
# Robust WSL entrypoint: file-based container script (no fragile host heredoc).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
IMAGE="${INTERFERENCE_CI_IMAGE:-127.0.0.1:5001/freecad-ci-deps:24.04}"
STAMP="$(date -u +%Y%m%dT%H%M%SZ)-$$"
VOLUME="freecad-interference-validate-${STAMP}"
HOST_LOG_DIR="${INTERFERENCE_CI_HOST_LOG_DIR:-/tmp/freecad-interference-validate-logs}"
SEED_VOLUME="${INTERFERENCE_CI_SEED_VOLUME:-freecad-interference-validate-data}"
SOURCE_REV="${INTERFERENCE_CI_SOURCE_REV:-unknown}"
DIRTY="${INTERFERENCE_CI_DIRTY:-1}"
MANIFEST_SHA="${INTERFERENCE_CI_MANIFEST_SHA:-skipped}"

rm -rf "${HOST_LOG_DIR}"
mkdir -p "${HOST_LOG_DIR}"

IMAGE_DIGEST="$(docker image inspect --format '{{index .RepoDigests 0}}' "${IMAGE}" 2>/dev/null \
  || docker image inspect --format '{{.Id}}' "${IMAGE}")"

cat > "${HOST_LOG_DIR}/meta.txt" <<EOF
stamp=${STAMP}
source_rev=${SOURCE_REV}
dirty_paths=${DIRTY}
image=${IMAGE}
image_digest=${IMAGE_DIGEST}
volume=${VOLUME}
seed_volume=${SEED_VOLUME}
relevant_manifest_sha256=${MANIFEST_SHA}
EOF

cleanup() {
  docker volume rm -f "${VOLUME}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

docker volume create "${VOLUME}" >/dev/null

if [[ -n "${SEED_VOLUME}" ]]; then
  docker run --rm --network=none \
    --mount "type=volume,src=${SEED_VOLUME},dst=/seed,readonly" \
    --mount "type=volume,src=${VOLUME},dst=/data" \
    "${IMAGE}" \
    bash -lc 'if [ -f /seed/build/CMakeCache.txt ]; then cp -a /seed/build /data/; cp -a /seed/ccache /data/ 2>/dev/null || true; echo SEEDED; else echo NO_SEED; fi'
fi

cat > "${HOST_LOG_DIR}/container.sh" <<'EOS'
set -euo pipefail
LOG=/out
mkdir -p /data/logs /data/ccache /data/src /data/build
export CCACHE_DIR=/data/ccache
ccache -M 8G >/dev/null 2>&1 || true

{
  echo "source_rev=${SOURCE_REV}"
  echo "image=${IMAGE}"
  echo "image_digest=${IMAGE_DIGEST}"
  echo "manifest_sha=${MANIFEST_SHA}"
  uname -a
  cmake --version | head -n1
} | tee "${LOG}/environment.txt"

echo "== sync source ==" | tee -a "${LOG}/steps.log"
rm -rf /data/src
mkdir -p /data/src
tar -C /src \
  --exclude='./build' \
  --exclude='./.git/objects' \
  --exclude='./.git/modules' \
  --exclude='./tools/mcp' \
  -cf - . | tar -C /data/src -xf -
echo SYNC_OK | tee -a "${LOG}/steps.log"

NEED_CFG=0
if [ ! -f /data/build/CMakeCache.txt ]; then NEED_CFG=1; fi
if [ "${NEED_CFG}" -eq 1 ]; then
  echo "== configure ==" | tee -a "${LOG}/steps.log"
  set +e
  cmake -S /data/src -B /data/build -G Ninja \
    -DCMAKE_BUILD_TYPE=Debug \
    -DCMAKE_C_COMPILER=/usr/bin/gcc \
    -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache \
    -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    -DENABLE_DEVELOPER_TESTS=ON \
    >"${LOG}/configure.log" 2>&1
  CFG=$?
  set -e
  echo "CONFIGURE_EXIT:${CFG}" | tee -a "${LOG}/steps.log"
  tail -n 40 "${LOG}/configure.log" | tee -a "${LOG}/steps.log"
  test "${CFG}" -eq 0
fi

echo "== build ==" | tee -a "${LOG}/steps.log"
set +e
cmake --build /data/build -j"$(nproc)" \
  --target Part Part_tests_run Assembly Assembly_tests_run AssemblyGui AssemblyGui_tests_run \
  >"${LOG}/build.log" 2>&1
BLD=$?
set -e
echo "BUILD_EXIT:${BLD}" | tee -a "${LOG}/steps.log"
tail -n 80 "${LOG}/build.log" | tee -a "${LOG}/steps.log"
test "${BLD}" -eq 0

mkdir -p /data/build/share/Mod/Material
cp -a /data/src/src/Mod/Material/Resources /data/build/share/Mod/Material/
export LD_LIBRARY_PATH="/data/build/lib:${LD_LIBRARY_PATH:-}"

echo "== Part tests ==" | tee -a "${LOG}/steps.log"
set +e
/data/build/tests/Part_tests_run --gtest_filter=InterferenceDetectionTest.* \
  >"${LOG}/InterferenceDetection.log" 2>&1
PART_RC=$?
set -e
echo "PART_TEST_EXIT:${PART_RC}" | tee -a "${LOG}/steps.log"
tail -n 40 "${LOG}/InterferenceDetection.log" | tee -a "${LOG}/steps.log"
test "${PART_RC}" -eq 0

echo "== Assembly tests ==" | tee -a "${LOG}/steps.log"
set +e
/data/build/tests/Assembly_tests_run \
  --gtest_filter=InterferenceScanTest.*:AssemblyObjectTest.* \
  >"${LOG}/InterferenceScan.log" 2>&1
ASM_RC=$?
set -e
echo "ASSEMBLY_TEST_EXIT:${ASM_RC}" | tee -a "${LOG}/steps.log"
tail -n 80 "${LOG}/InterferenceScan.log" | tee -a "${LOG}/steps.log"
test "${ASM_RC}" -eq 0

echo "== AssemblyGui tests ==" | tee -a "${LOG}/steps.log"
set +e
export QT_QPA_PLATFORM=offscreen
export QT_PLUGIN_PATH="/usr/lib/x86_64-linux-gnu/qt6/plugins:${QT_PLUGIN_PATH:-}"
/data/build/tests/AssemblyGui_tests_run --gtest_filter=TaskInterferenceCheckTest.* \
  >"${LOG}/TaskInterferenceCheck.log" 2>&1
GUI_RC=$?
set -e
echo "ASSEMBLYGUI_TEST_EXIT:${GUI_RC}" | tee -a "${LOG}/steps.log"
tail -n 80 "${LOG}/TaskInterferenceCheck.log" | tee -a "${LOG}/steps.log"
test "${GUI_RC}" -eq 0

echo "== AssemblyGui Xvfb lifecycle/preview tests ==" | tee -a "${LOG}/steps.log"
if [ ! -x /usr/bin/xvfb-run ]; then
  echo "ASSEMBLYGUI_XVFB_TEST_EXIT:127" | tee -a "${LOG}/steps.log"
  echo "FATAL: mandatory /usr/bin/xvfb-run is missing or not executable" | tee -a "${LOG}/steps.log"
  exit 1
fi
set +e
export QT_QPA_PLATFORM=xcb
export ASSEMBLYGUI_REQUIRE_XCB=1
export QT_PLUGIN_PATH="/usr/lib/x86_64-linux-gnu/qt6/plugins:${QT_PLUGIN_PATH:-}"
/usr/bin/xvfb-run -a /data/build/tests/AssemblyGui_tests_run \
  --gtest_filter=TaskInterferenceCheckTest.bThenALateFinishDoesNotMutateNewerUiState:TaskInterferenceCheckTest.placedLeafPreviewRestoresWorldTransform:TaskInterferenceCheckTest.documentCloseDiscardsResultsAndClosesManageExclusions:TaskInterferenceCheckTest.linkedDocumentCloseMarksResultsStaleAndClosesManageExclusions \
  >"${LOG}/TaskInterferenceCheckXvfb.log" 2>&1
XVFB_GUI_RC=$?
set -e
echo "ASSEMBLYGUI_XVFB_TEST_EXIT:${XVFB_GUI_RC}" | tee -a "${LOG}/steps.log"
tail -n 80 "${LOG}/TaskInterferenceCheckXvfb.log" | tee -a "${LOG}/steps.log"
test "${XVFB_GUI_RC}" -eq 0

echo ALL_MANDATORY_PASSED | tee -a "${LOG}/steps.log"
EOS

echo "Running isolated validation (stamp=${STAMP}, volume=${VOLUME})..."
docker run --rm --network=none \
  --name "freecad-interference-validate-${STAMP}" \
  --mount "type=bind,src=${ROOT},dst=/src,readonly" \
  --mount "type=bind,src=${HOST_LOG_DIR},dst=/out" \
  --mount "type=volume,src=${VOLUME},dst=/data" \
  -e SOURCE_REV="${SOURCE_REV}" \
  -e IMAGE="${IMAGE}" \
  -e IMAGE_DIGEST="${IMAGE_DIGEST}" \
  -e MANIFEST_SHA="${MANIFEST_SHA}" \
  "${IMAGE}" \
  bash /out/container.sh

echo "Validation OK. Logs: ${HOST_LOG_DIR}"
