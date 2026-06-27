#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Docker E2E: RobotCAD export (required) + gz_cli spawn of nested URDF.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

find "${ROOT}/e2e" \( -name '*.sh' -o -name '*.py' \) -exec sed -i 's/\r$//' {} +
sed -i 's/\r$//' "${ROOT}/scripts/robotcad_headless.py" 2>/dev/null || true

export PYTHONPATH="${ROOT}:${PYTHONPATH:-}"
export E2E_BRIDGE_MODULE="${E2E_BRIDGE_MODULE:-gz_cli}"
export SIM_RUNS_DIR="${SIM_RUNS_DIR:-${ROOT}/sim_runs}"
export GZ_SIM_WORLD_NAME="${GZ_SIM_WORLD_NAME:-empty_world}"
export GAZEBO_URDF_CONTAINER_PKG_ROOT="${GAZEBO_URDF_CONTAINER_PKG_ROOT:-/models/arm_2dof_description}"
export E2E_REQUIRE_ROBOTCAD_URDF=1
export E2E_REQUIRE_MODELS_MOUNT=1
export ROOT

if [[ -f /opt/ros/jazzy/setup.bash ]]; then
  set +u
  # shellcheck source=/dev/null
  source /opt/ros/jazzy/setup.bash
  set -u
fi

TS="$(date -u +"%Y%m%dT%H%M%SZ")"
E2E_RUN="${SIM_RUNS_DIR}/e2e_${TS}"
mkdir -p "${E2E_RUN}"
export E2E_RUN_DIR="${E2E_RUN}"

exec > >(tee -a "${E2E_RUN_DIR}/console.log") 2>&1

echo "══════════════════════════════════════════════════════════════"
echo " E2E (RobotCAD export required): ${E2E_RUN_DIR}"
echo "══════════════════════════════════════════════════════════════"

FREECAD_CMD="$(command -v FreeCADCmd || command -v freecadcmd-daily || true)"
export FREECAD_CMD
if [[ -z "${FREECAD_CMD}" ]]; then
  echo "ERROR: FreeCADCmd not found"
  exit 1
fi

echo "--- Versions ---"
"${FREECAD_CMD}" --version || true
(command -v gz >/dev/null && gz sim --version) || true
python3 --version

echo "--- Robot source (FCStd lock) ---"
bash "${ROOT}/e2e/fetch_robot_source.sh"
python3 "${ROOT}/e2e/verify_robot_source.py"

echo "--- MCP venv install ---"
/opt/mcp-venv/bin/pip install -q -e "${ROOT}/tools/mcp/gazebo-mcp"
/opt/mcp-venv/bin/pip install -q -e "${ROOT}/tools/mcp/freecad-mcp"
/opt/mcp-venv/bin/pip install -q -e "${ROOT}/tools/mcp/ros-mcp-server"

echo "--- RobotCAD import check ---"
xvfb-run -a "${FREECAD_CMD}" "${ROOT}/e2e/check_robotcad_freecad.py"

echo "--- RobotCAD export (strict) ---"
bash "${ROOT}/e2e/stage_export.sh"
bash "${ROOT}/e2e/assert_robotcad_export.sh" "${ROOT}"

STAGED_URDF="$(bash "${ROOT}/e2e/resolve_staged_urdf.sh" "${ROOT}")"
echo "--- URDF check: ${STAGED_URDF} ---"
check_urdf "${STAGED_URDF}"

echo "--- MCP smoke ---"
/opt/mcp-venv/bin/python3 "${ROOT}/e2e/mcp_smoke.py"

echo "--- Gazebo + scenario (exported RobotCAD URDF) ---"
bash "${ROOT}/e2e/run_gazebo_scenarios.sh" e2e_smoke

export E2E_VERSION_STRICT="${E2E_VERSION_STRICT:-1}"
if ! python3 "${ROOT}/e2e/record_runtime_versions.py"; then
  echo "ERROR: runtime version check failed (see versions.yaml)" >&2
  exit 1
fi

echo "E2E finished OK (RobotCAD export + spawn) → ${E2E_RUN_DIR}"
