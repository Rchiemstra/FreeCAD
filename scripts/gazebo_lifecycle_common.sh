#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Shared Gazebo live-stack defaults (sourced by Start-gz-sim / fast / bridge scripts).
#
# Canonical world: worlds/empty_world.sdf → world name empty_world
# See docs/gazebo-lifecycle.md

_gazebo_lifecycle_root() {
  if [[ -n "${GAZEBO_LIFECYCLE_ROOT:-}" ]]; then
    echo "$GAZEBO_LIFECYCLE_ROOT"
    return
  fi
  local script_dir
  script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  GAZEBO_LIFECYCLE_ROOT="$(cd "${script_dir}/.." && pwd)"
  echo "$GAZEBO_LIFECYCLE_ROOT"
}

gz_default_world_name() {
  echo "${GZ_SIM_WORLD_NAME:-${GAZEBO_WORLD_NAME:-empty_world}}"
}

gz_export_live_env() {
  local world
  world="$(gz_default_world_name)"
  export GZ_SIM_CONTAINER_NAME="${GZ_SIM_CONTAINER_NAME:-gz-sim-sever}"
  export ROS_GZ_BRIDGE_CONTAINER="${ROS_GZ_BRIDGE_CONTAINER:-ros-gz-bridge}"
  export GZ_SIM_WORLD_NAME="${GZ_SIM_WORLD_NAME:-$world}"
  export GAZEBO_WORLD_NAME="${GAZEBO_WORLD_NAME:-$world}"
  export GZ_SIM_RESOURCE_PATH="${GZ_SIM_RESOURCE_PATH:-/models}"
}

gz_world_sdf_host() {
  local root
  root="$(_gazebo_lifecycle_root)"
  echo "${root}/worlds/empty_world.sdf"
}

gz_world_sdf_container() {
  echo "/worlds/empty_world.sdf"
}

gz_stop_stack() {
  local gz bridge
  gz="${GZ_SIM_CONTAINER_NAME:-gz-sim-sever}"
  bridge="${ROS_GZ_BRIDGE_CONTAINER:-ros-gz-bridge}"
  echo "[gazebo] Stopping containers: $bridge, $gz"
  docker rm -f "$bridge" "$gz" 2>/dev/null || true
}

gz_stack_status() {
  local gz bridge
  gz="${GZ_SIM_CONTAINER_NAME:-gz-sim-sever}"
  bridge="${ROS_GZ_BRIDGE_CONTAINER:-ros-gz-bridge}"
  for c in "$gz" "$bridge"; do
    if docker container inspect "$c" >/dev/null 2>&1; then
      echo "$c: $(docker inspect -f '{{.State.Status}}' "$c" 2>/dev/null || echo unknown)"
    else
      echo "$c: absent"
    fi
  done
}
