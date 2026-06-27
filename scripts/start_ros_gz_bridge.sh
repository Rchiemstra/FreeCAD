#!/usr/bin/env bash
# Start ros_gz parameter_bridge inside the bridge sidecar (run from WSL only).
set -euo pipefail

BRIDGE="${ROS_GZ_BRIDGE_CONTAINER:-ros-gz-bridge}"
WORLD="${GAZEBO_WORLD_NAME:-empty}"

docker exec "$BRIDGE" bash -lc "
set -e
source /opt/ros/humble/setup.bash
# Same PID namespace as gz-sim — avoid broad pkill (can stop gz sim).
if pgrep -f 'parameter_bridge' >/dev/null; then
  echo 'parameter_bridge already running'
  ros2 service list | grep /world/${WORLD}/ || true
  exit 0
fi
nohup ros2 run ros_gz_bridge parameter_bridge \
  /world/${WORLD}/control@ros_gz_interfaces/srv/ControlWorld \
  /world/${WORLD}/create@ros_gz_interfaces/srv/SpawnEntity \
  /world/${WORLD}/remove@ros_gz_interfaces/srv/DeleteEntity \
  /world/${WORLD}/set_pose@ros_gz_interfaces/srv/SetEntityPose \
  '/clock@rosgraph_msgs/msg/Clock[gz.msgs.Clock]' \
  >/tmp/ros_gz_bridge.log 2>&1 &
sleep 4
echo 'ROS services:'
ros2 service list | grep /world/${WORLD}/ || true
echo '--- unpause ---'
timeout 15 ros2 service call /world/${WORLD}/control ros_gz_interfaces/srv/ControlWorld \
  '{world_control: {pause: false}}'
"
