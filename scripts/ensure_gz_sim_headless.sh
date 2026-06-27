#!/usr/bin/env bash
# Configure running gz-sim Docker for live spawn (no full image rebuild).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/gazebo_lifecycle_common.sh
source "${ROOT}/scripts/gazebo_lifecycle_common.sh"
gz_export_live_env

GZ_CONTAINER="$GZ_SIM_CONTAINER_NAME"
BRIDGE="$ROS_GZ_BRIDGE_CONTAINER"
GENERATED_PKG="$ROOT/generated/arm_2dof/arm_2dof_description/arm_2dof_description"
WORLD="$GZ_SIM_WORLD_NAME"
WORLD_SDF_HOST="$(gz_world_sdf_host)"
WORLD_SDF_IN_CONTAINER="/tmp/empty_world.sdf"

if ! docker container inspect "$GZ_CONTAINER" >/dev/null 2>&1; then
  echo "Container $GZ_CONTAINER not found. Run Start-gz-sim.bat first." >&2
  exit 1
fi
if [[ "$(docker inspect -f '{{.State.Status}}' "$GZ_CONTAINER")" != "running" ]]; then
  echo "Container $GZ_CONTAINER is not running. Run Start-gz-sim.bat." >&2
  exit 1
fi

if [[ ! -f "$WORLD_SDF_HOST" ]]; then
  echo "Missing $WORLD_SDF_HOST" >&2
  exit 1
fi
if [[ ! -d "$GENERATED_PKG" ]]; then
  echo "RobotCAD package not found: $GENERATED_PKG" >&2
  exit 1
fi

echo "Syncing RobotCAD package into $GZ_CONTAINER:/models/arm_2dof_description ..."
docker exec "$GZ_CONTAINER" mkdir -p /models/arm_2dof_description
docker cp "$GENERATED_PKG/." "$GZ_CONTAINER:/models/arm_2dof_description/"
docker cp "$WORLD_SDF_HOST" "$GZ_CONTAINER:${WORLD_SDF_IN_CONTAINER}"

echo "Restarting headless gz sim (world=$WORLD) inside $GZ_CONTAINER ..."
docker exec "$GZ_CONTAINER" bash -lc "
  set -e
  killall -9 gz-sim-gui-client gz-sim-main 2>/dev/null || true
  sleep 2
  export GZ_SIM_RESOURCE_PATH=/models:\${GZ_SIM_RESOURCE_PATH:-}
  export DISPLAY=:99
  export XDG_RUNTIME_DIR=/tmp/xdg-runtime-root
  mkdir -p \"\$XDG_RUNTIME_DIR\"
  chmod 700 \"\$XDG_RUNTIME_DIR\"
  if ! pgrep -x Xvfb >/dev/null 2>&1; then
    Xvfb :99 -screen 0 1280x1024x24 -noreset 2>/dev/null &
    sleep 1
  fi
  nohup gz sim -s -r ${WORLD_SDF_IN_CONTAINER} >/tmp/gz-sim-server.log 2>&1 &
  for i in 1 2 3 4 5 6 7 8 9 10; do
    sleep 1
    gz service -l 2>/dev/null | grep -q '/world/${WORLD}/create' && break
  done
  gz service -l 2>/dev/null | grep '/world/${WORLD}' | head -5
"

docker rm -f "$BRIDGE" 2>/dev/null || true
bash "${ROOT}/scripts/ensure_ros_gz_bridge.sh"

echo "gz-sim headless + models ready (world=$WORLD)."
