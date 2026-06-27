#!/usr/bin/env bash
# Ensure ros_gz_bridge sidecar is running for gz-sim Docker (canonical world: empty_world).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/gazebo_lifecycle_common.sh
source "${ROOT}/scripts/gazebo_lifecycle_common.sh"
gz_export_live_env

GZ_CONTAINER="$GZ_SIM_CONTAINER_NAME"
BRIDGE_CONTAINER="$ROS_GZ_BRIDGE_CONTAINER"
WORLD_NAME="$GZ_SIM_WORLD_NAME"

if ! docker container inspect "$GZ_CONTAINER" >/dev/null 2>&1; then
  echo "Container $GZ_CONTAINER not found. Run Start-gz-sim.bat or Start-gz-sim-fast.bat first." >&2
  exit 1
fi

status="$(docker inspect -f '{{.State.Status}}' "$GZ_CONTAINER" 2>/dev/null || true)"
if [[ "$status" != "running" ]]; then
  echo "Container $GZ_CONTAINER is not running (status=$status)." >&2
  exit 1
fi

if docker container inspect "$BRIDGE_CONTAINER" >/dev/null 2>&1; then
  bstatus="$(docker inspect -f '{{.State.Status}}' "$BRIDGE_CONTAINER" 2>/dev/null || true)"
  if [[ "$bstatus" == "running" ]]; then
    echo "ros_gz_bridge sidecar '$BRIDGE_CONTAINER' already running (world=$WORLD_NAME)."
    exit 0
  fi
  docker rm -f "$BRIDGE_CONTAINER" >/dev/null
fi

echo "Starting ros_gz_bridge sidecar '$BRIDGE_CONTAINER' (world=$WORLD_NAME)..."
docker run -d --name "$BRIDGE_CONTAINER" --network "container:${GZ_CONTAINER}" \
  osrf/ros:humble-desktop sleep infinity

docker exec "$BRIDGE_CONTAINER" bash -lc "
  set -e
  source /opt/ros/humble/setup.bash
  apt-get update -qq
  apt-get install -y -qq ros-humble-ros-gz-bridge
  nohup ros2 run ros_gz_bridge parameter_bridge \
    /world/${WORLD_NAME}/control@ros_gz_interfaces/srv/ControlWorld \
    /world/${WORLD_NAME}/create@ros_gz_interfaces/srv/SpawnEntity \
    /world/${WORLD_NAME}/remove@ros_gz_interfaces/srv/DeleteEntity \
    /world/${WORLD_NAME}/set_pose@ros_gz_interfaces/srv/SetEntityPose \
    '/clock@rosgraph_msgs/msg/Clock[gz.msgs.Clock]' \
    >/tmp/ros_gz_bridge.log 2>&1 &
  sleep 2
  ros2 service list | grep -q /world/${WORLD_NAME}/create
  echo '--- unpause world ---'
  ros2 service call /world/${WORLD_NAME}/control ros_gz_interfaces/srv/ControlWorld \
    '{world_control: {pause: false}}' >/tmp/unpause.log 2>&1 || true
"

echo "ros_gz_bridge ready (container=$BRIDGE_CONTAINER, world=$WORLD_NAME)."
