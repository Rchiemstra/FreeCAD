#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Offline-friendly lifecycle smoke (env + files). Optional: pass --docker to probe containers.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/gazebo_lifecycle_common.sh
source "${ROOT}/scripts/gazebo_lifecycle_common.sh"
gz_export_live_env

echo "=== gazebo lifecycle smoke ==="
echo "ROOT=$ROOT"
echo "GZ_SIM_CONTAINER_NAME=$GZ_SIM_CONTAINER_NAME"
echo "ROS_GZ_BRIDGE_CONTAINER=$ROS_GZ_BRIDGE_CONTAINER"
echo "GZ_SIM_WORLD_NAME=$GZ_SIM_WORLD_NAME"
echo "GAZEBO_WORLD_NAME=$GAZEBO_WORLD_NAME"

if [[ "$GZ_SIM_WORLD_NAME" != "$GAZEBO_WORLD_NAME" ]]; then
  echo "ERROR: world env mismatch" >&2
  exit 1
fi

WORLD_SDF="$(gz_world_sdf_host)"
if [[ ! -f "$WORLD_SDF" ]]; then
  echo "ERROR: missing $WORLD_SDF" >&2
  exit 1
fi
if ! grep -q '<world name="empty_world">' "$WORLD_SDF"; then
  echo "ERROR: $WORLD_SDF must declare <world name=\"empty_world\">" >&2
  exit 1
fi
echo "OK world SDF: $WORLD_SDF"

export PYTHONPATH="${ROOT}:${PYTHONPATH:-}"
python3 -c "
from bridge.gazebo_lifecycle import resolve_world_name, validate_world_env
ok, msg = validate_world_env()
assert ok, msg
assert resolve_world_name() == '${GZ_SIM_WORLD_NAME}'
print('OK bridge.gazebo_lifecycle')
"

if [[ "${1:-}" == "--docker" ]]; then
  if ! command -v docker >/dev/null 2>&1; then
    echo "SKIP docker probes (docker not on PATH)"
    exit 0
  fi
  echo "--- docker status ---"
  gz_stack_status
  gz_c="${GZ_SIM_CONTAINER_NAME}"
  if docker container inspect "$gz_c" >/dev/null 2>&1 \
     && [[ "$(docker inspect -f '{{.State.Status}}' "$gz_c")" == "running" ]]; then
    docker exec "$gz_c" gz service -l 2>/dev/null | grep -q "/world/${GZ_SIM_WORLD_NAME}/create" \
      && echo "OK gz service /world/${GZ_SIM_WORLD_NAME}/create" \
      || echo "WARN: /world/${GZ_SIM_WORLD_NAME}/create not listed (gz still starting?)"
  fi
fi

echo "=== smoke OK ==="
