#!/usr/bin/env bash
# Quick check: gazebo-mcp sees real Gazebo via ros_gz_bridge in Docker.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
GZ_MCP="$ROOT/tools/mcp/gazebo-mcp"

docker run -i --rm --network container:ros-gz-bridge \
  -v "$GZ_MCP:/ws" \
  osrf/ros:humble-desktop \
  bash -lc '
set -e
cd /ws
export PYTHONPATH=/ws/src:/ws
source /opt/ros/humble/setup.bash
python3 -c "from gazebo_mcp.tools._bridge_helper import use_real_gazebo; print(\"use_real_gazebo\", use_real_gazebo())"
python3 -c "from gazebo_mcp.tools.simulation_tools import get_simulation_status; r=get_simulation_status(); print(r.success, r.data)"
'
