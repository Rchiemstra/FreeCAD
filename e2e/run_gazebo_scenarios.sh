#!/usr/bin/env bash
# Start headless Gazebo, unpause, run scenario(s) via gz_cli bridge.
set -euo pipefail

ROOT="${ROOT:-/workspace}"
export SIM_RUNS_DIR="${SIM_RUNS_DIR:-${E2E_RUN_DIR:-${ROOT}/sim_runs}}"
GZ_SIM_WORLD_NAME="${GZ_SIM_WORLD_NAME:-empty_world}"
WORLD_SDF="${E2E_WORLD_SDF:-${ROOT}/worlds/empty_world.sdf}"

export DISPLAY="${DISPLAY:-:99}"
if ! pgrep -f "Xvfb ${DISPLAY}" >/dev/null 2>&1; then
  Xvfb "${DISPLAY}" -screen 0 1280x1024x24 -noreset &
  sleep 2
fi

export GZ_SIM_RESOURCE_PATH="/models:${GZ_SIM_RESOURCE_PATH:-}"
gz sim -s -r "${WORLD_SDF}" &
GZ_PID=$!

cleanup() {
  if kill -0 "${GZ_PID}" 2>/dev/null; then
    kill "${GZ_PID}" 2>/dev/null || true
    wait "${GZ_PID}" 2>/dev/null || true
  fi
}
trap cleanup EXIT

for _ in $(seq 1 90); do
  if gz service -l 2>/dev/null | grep -q "/world/${GZ_SIM_WORLD_NAME}/create"; then
    break
  fi
  sleep 1
done

gz service -s "/world/${GZ_SIM_WORLD_NAME}/control" \
  --reqtype gz.msgs.WorldControl --reptype gz.msgs.Boolean \
  --timeout 8000 --req "pause: false" || true

for scenario in "$@"; do
  echo "--- Scenario: ${scenario} ---"
  python3 -m runner.runner run "${scenario}" --dir "${ROOT}/tests/scenarios_e2e"
done
