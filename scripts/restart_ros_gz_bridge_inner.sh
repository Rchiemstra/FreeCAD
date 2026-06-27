#!/usr/bin/env bash
set -e
source /opt/ros/humble/setup.bash
WORLD="${GAZEBO_WORLD_NAME:-empty}"
for pid in $(pgrep -f '/parameter_bridge$' || true); do
  kill "$pid" 2>/dev/null || true
done
sleep 2
nohup ros2 run ros_gz_bridge parameter_bridge \
  "/world/${WORLD}/control@ros_gz_interfaces/srv/ControlWorld" \
  "/world/${WORLD}/create@ros_gz_interfaces/srv/SpawnEntity" \
  "/world/${WORLD}/remove@ros_gz_interfaces/srv/DeleteEntity" \
  "/world/${WORLD}/set_pose@ros_gz_interfaces/srv/SetEntityPose" \
  '/clock@rosgraph_msgs/msg/Clock[gz.msgs.Clock]' \
  >/tmp/ros_gz_bridge.log 2>&1 &
sleep 5
echo "ROS services:"
ros2 service list | grep "/world/${WORLD}/" || true
echo "--- unpause ---"
timeout 20 ros2 service call "/world/${WORLD}/control" ros_gz_interfaces/srv/ControlWorld \
  '{world_control: {pause: false}}'
