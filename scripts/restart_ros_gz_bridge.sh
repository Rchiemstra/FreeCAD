#!/usr/bin/env bash
set -e
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/gazebo_lifecycle_common.sh
source "${ROOT}/scripts/gazebo_lifecycle_common.sh"
gz_export_live_env
BRIDGE="$ROS_GZ_BRIDGE_CONTAINER"
WORLD="$GZ_SIM_WORLD_NAME"
docker cp "$ROOT/scripts/restart_ros_gz_bridge_inner.sh" "$BRIDGE:/tmp/restart_ros_gz_bridge_inner.sh"
docker exec -e "GAZEBO_WORLD_NAME=$WORLD" "$BRIDGE" bash -lc "sed -i 's/\r$//' /tmp/restart_ros_gz_bridge_inner.sh && bash /tmp/restart_ros_gz_bridge_inner.sh"
