#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Optional E2E: placeholder URDF only (no RobotCAD export). Not the main acceptance path.
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "${ROOT}"

find "${ROOT}/e2e" \( -name '*.sh' -o -name '*.py' \) -exec sed -i 's/\r$//' {} +

export PYTHONPATH="${ROOT}:${PYTHONPATH:-}"
export E2E_BRIDGE_MODULE=gz_cli
export SIM_RUNS_DIR="${SIM_RUNS_DIR:-${ROOT}/sim_runs}"
export GZ_SIM_WORLD_NAME="${GZ_SIM_WORLD_NAME:-empty_world}"
export E2E_REQUIRE_ROBOTCAD_URDF=0
export E2E_REQUIRE_MODELS_MOUNT=0
export ROOT

TS="$(date -u +"%Y%m%dT%H%M%SZ")"
E2E_RUN="${SIM_RUNS_DIR}/e2e_placeholder_${TS}"
mkdir -p "${E2E_RUN}"
exec > >(tee -a "${E2E_RUN}/console.log") 2>&1

echo "══════════════════════════════════════════════════════════════"
echo " E2E placeholder fallback (NOT RobotCAD export proof)"
echo "══════════════════════════════════════════════════════════════"

bash "${ROOT}/e2e/stage_export_placeholder_fallback.sh"
STAGED="$(bash "${ROOT}/e2e/resolve_staged_urdf.sh" "${ROOT}")"
echo "Staged URDF: ${STAGED}"
check_urdf "${STAGED}"

bash "${ROOT}/e2e/run_gazebo_scenarios.sh" e2e_placeholder_fallback

echo "E2E placeholder fallback OK → ${E2E_RUN}"
