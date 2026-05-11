#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Docker / Linux unattended E2E driver (see docker/compose.e2e.yml).
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

export PYTHONPATH="${ROOT}:${PYTHONPATH:-}"
# Gazebo Harmonic apt packages (gz.msgs, gz.transport) live in system dist-packages;
# the MCP venv is not created with --system-site-packages, so prepend them explicitly.
if [[ -d /usr/lib/python3/dist-packages ]]; then
  export PYTHONPATH="${PYTHONPATH}:/usr/lib/python3/dist-packages"
fi
export E2E_BRIDGE_MODULE="${E2E_BRIDGE_MODULE:-gz_cli}"
export SIM_RUNS_DIR="${SIM_RUNS_DIR:-${ROOT}/sim_runs}"

if [[ -f /opt/ros/jazzy/setup.bash ]]; then
  # ROS setup references unset vars — incompatible with nounset
  set +u
  # shellcheck source=/dev/null
  source /opt/ros/jazzy/setup.bash
  set -u
fi

TS="$(date -u +"%Y%m%dT%H%M%SZ")"
E2E_RUN="${SIM_RUNS_DIR}/e2e_${TS}"
mkdir -p "${E2E_RUN}"
export E2E_RUN_DIR="${E2E_RUN}"

mkdir -p "${E2E_RUN_DIR}/logs"
# Optional explicit override; default JSONL path is <E2E_RUN_DIR>/logs/structured.jsonl (see bridge/structured_log.py)
export BRIDGE_STRUCTLOG_PATH="${BRIDGE_STRUCTLOG_PATH:-${E2E_RUN_DIR}/logs/structured.jsonl}"

exec > >(tee -a "${E2E_RUN_DIR}/console.log") 2>&1

echo "══════════════════════════════════════════════════════════════"
echo " E2E run directory: ${E2E_RUN_DIR}"
echo "══════════════════════════════════════════════════════════════"

echo "--- Versions ---"
if [[ -f "${ROOT}/config/runtime_manifest.yaml" ]]; then
  echo "Runtime manifest (CI/E2E pins): ${ROOT}/config/runtime_manifest.yaml"
fi
FREECAD_CMD="$(command -v FreeCADCmd || command -v freecadcmd-daily || true)"
export FREECAD_CMD
if [[ -z "${FREECAD_CMD}" ]]; then
  echo "ERROR: FreeCADCmd / freecadcmd-daily not found in PATH"
  exit 1
fi
"${FREECAD_CMD}" --version || true
(command -v gz >/dev/null && gz sim --version) || echo "(gz missing)"
python3 --version
(command -v ros2 >/dev/null && echo "ros2 CLI: $(command -v ros2)") || true

echo "--- Git submodules (MCP server sources for pip install -e) ---"
git -C "${ROOT}" submodule update --init --depth 1 \
  tools/mcp/gazebo-mcp tools/mcp/freecad-mcp tools/mcp/ros-mcp-server \
  || git -C "${ROOT}" submodule update --init --recursive --depth 1 \
  || true
for _pkg in gazebo-mcp freecad-mcp ros-mcp-server; do
  if [[ ! -f "${ROOT}/tools/mcp/${_pkg}/pyproject.toml" ]] && [[ ! -f "${ROOT}/tools/mcp/${_pkg}/setup.py" ]]; then
    echo "ERROR: tools/mcp/${_pkg} is missing pyproject.toml (submodule not checked out)."
    echo "Fix on host: git submodule update --init tools/mcp/${_pkg}"
    exit 1
  fi
done

echo "--- Pip install MCP servers (editable from mounted workspace) ---"
/opt/mcp-venv/bin/pip install -q -e "${ROOT}/tools/mcp/gazebo-mcp"
/opt/mcp-venv/bin/pip install -q -e "${ROOT}/tools/mcp/freecad-mcp"
/opt/mcp-venv/bin/pip install -q -e "${ROOT}/tools/mcp/ros-mcp-server"

echo "--- RobotCAD module import (FreeCAD Python) ---"
xvfb-run -a "${FREECAD_CMD}" "${ROOT}/e2e/check_robotcad_freecad.py"

echo "--- Stage robot artifacts ---"
bash "${ROOT}/e2e/stage_export.sh"

echo "--- URDF check (staged export path) ---"
check_urdf "${ROOT}/generated/arm_2dof/arm_2dof.urdf"

echo "--- Xvfb + headless Gazebo ---"
export DISPLAY="${DISPLAY:-:99}"
if ! pgrep -f "Xvfb ${DISPLAY}" >/dev/null 2>&1; then
  Xvfb "${DISPLAY}" -screen 0 1280x1024x24 -noreset &
  sleep 2
fi

# Software GL improves ogre2 camera reliability in some headless Docker setups.
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"

WORLD="${ROOT}/worlds/e2e_world.sdf"
# -r: run (unpause) on start — camera sensors publish frames only while sim is running
gz sim -r -s "${WORLD}" &
GZ_PID=$!

cleanup() {
  if kill -0 "${GZ_PID}" 2>/dev/null; then
    kill "${GZ_PID}" 2>/dev/null || true
    wait "${GZ_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

echo "--- Wait for Gazebo ---"
for _ in $(seq 1 60); do
  if gz topic -l >/dev/null 2>&1; then
    break
  fi
  sleep 1
done

echo "--- MCP stdio smoke (gazebo / freecad / ros servers) ---"
/opt/mcp-venv/bin/python3 "${ROOT}/e2e/mcp_smoke.py"

echo "--- Bridge gazebo-mcp APIs (SimWorkbench Gazebo Status panel parity) ---"
/opt/mcp-venv/bin/python3 "${ROOT}/e2e/bridge_gazebo_mcp_smoke.py"

echo "--- Scenario runner (live gz_cli bridge) ---"
python3 -m runner.runner run-all --dir "${ROOT}/tests/scenarios_e2e"

echo "E2E finished OK → ${E2E_RUN_DIR}"
