#!/usr/bin/env bash
# Fast gz-sim stack: OSRF packages only (no FreeCAD source build). For live spawn tests.
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
WORLD_SDF="$(gz_world_sdf_container)"
IMAGE="${GZ_FAST_IMAGE:-osrf/ros:humble-desktop}"

gz_stop_stack

if [[ ! -f "$WORLD_SDF_HOST" ]]; then
  echo "Missing $WORLD_SDF_HOST" >&2
  exit 1
fi
if [[ ! -d "$GENERATED_PKG" ]]; then
  echo "Missing $GENERATED_PKG — run RobotCAD export first." >&2
  exit 1
fi

echo "Starting fast gz-sim ($IMAGE) world=$WORLD ..."
docker run -d --name "$GZ_CONTAINER" \
  -v "$GENERATED_PKG:/models/arm_2dof_description:ro" \
  -v "$ROOT/worlds:/worlds:ro" \
  "$IMAGE" \
  sleep infinity

docker exec "$GZ_CONTAINER" bash -lc "
  set -e
  export DEBIAN_FRONTEND=noninteractive
  apt-get update -qq
  apt-get install -y -qq curl gnupg lsb-release
  curl -fsSL https://packages.osrfoundation.org/gazebo.gpg -o /usr/share/keyrings/pkgs-osrf-archive-keyring.gpg
  echo \"deb [arch=\$(dpkg --print-architecture) signed-by=/usr/share/keyrings/pkgs-osrf-archive-keyring.gpg] http://packages.osrfoundation.org/gazebo/ubuntu-stable \$(lsb_release -cs) main\" > /etc/apt/sources.list.d/gazebo-stable.list
  apt-get update -qq
  apt-get install -y -qq gz-harmonic ros-humble-ros-gz-bridge ros-humble-ros-gz-interfaces
  export GZ_SIM_RESOURCE_PATH=/models:\${GZ_SIM_RESOURCE_PATH:-}
  nohup gz sim -s -r ${WORLD_SDF} >/tmp/gz-sim-server.log 2>&1 &
  for i in 1 2 3 4 5 6 7 8 9 10; do
    sleep 1
    gz service -l 2>/dev/null | grep -q '/world/${WORLD}/create' && break
  done
  pgrep -af 'gz sim' || true
  gz service -l 2>/dev/null | grep '/world/${WORLD}' | head -5 || true
"

docker run -d --name "$BRIDGE" --network "container:${GZ_CONTAINER}" \
  "$IMAGE" sleep infinity

docker exec "$BRIDGE" bash -lc "
  set -e
  source /opt/ros/humble/setup.bash
  apt-get update -qq
  apt-get install -y -qq ros-humble-ros-gz-bridge
  nohup ros2 run ros_gz_bridge parameter_bridge \
    /world/${WORLD}/control@ros_gz_interfaces/srv/ControlWorld \
    /world/${WORLD}/create@ros_gz_interfaces/srv/SpawnEntity \
    /world/${WORLD}/remove@ros_gz_interfaces/srv/DeleteEntity \
    /world/${WORLD}/set_pose@ros_gz_interfaces/srv/SetEntityPose \
    '/clock@rosgraph_msgs/msg/Clock[gz.msgs.Clock]' \
    >/tmp/ros_gz_bridge.log 2>&1 &
  sleep 3
  ros2 service call /world/${WORLD}/control ros_gz_interfaces/srv/ControlWorld \
    '{world_control: {pause: false}}' || true
"

echo "Fast gz-sim stack ready (world=$WORLD, sdf=$WORLD_SDF)."
echo "Spawn: GAZEBO_MCP_DOCKER=1 GAZEBO_SPAWN_VIA_GZ_CLI=1 (default) uses gz CLI, not ros_gz create."
