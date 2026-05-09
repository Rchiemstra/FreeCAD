#!/usr/bin/env bash
# Run from WSL: builds ROS 2 in Docker (Ubuntu Noble + ROS 2 apt packages).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROS2_SRC="$ROOT/src/3rdParty/ros2"
ROS2_WS_VOLUME="${ROS2_WS_VOLUME:-ros2-linux-workspace}"
ROS2_IMAGE="${ROS2_IMAGE:-ubuntu:noble}"
ROS2_CONTAINER_NAME="${ROS2_CONTAINER_NAME:-ros2-server}"
ROS2_ARGS=("$@")

if ! command -v docker >/dev/null 2>&1; then
    echo "Docker was not found on PATH (install Docker in WSL or enable Docker Desktop WSL integration)." >&2
    exit 1
fi

if [[ ! -f "$ROS2_SRC/ros2.repos" ]]; then
    echo "Initializing ros2 submodule..."
    git -C "$ROOT" submodule update --init --recursive src/3rdParty/ros2
fi

docker volume create "$ROS2_WS_VOLUME" >/dev/null

if docker container inspect "$ROS2_CONTAINER_NAME" >/dev/null 2>&1; then
    status="$(docker inspect -f '{{.State.Status}}' "$ROS2_CONTAINER_NAME" 2>/dev/null || true)"
    if [[ "$status" == "running" ]]; then
        echo "Docker container '$ROS2_CONTAINER_NAME' is already running. Nothing to do."
        exit 0
    fi
    echo "Removing stopped container '$ROS2_CONTAINER_NAME'..."
    docker rm "$ROS2_CONTAINER_NAME"
fi

echo "Building and starting ROS 2 in Docker (Ubuntu Noble + ROS 2 apt packages)."
echo "Source: $ROS2_SRC"
echo "Workspace volume: $ROS2_WS_VOLUME"
echo "Container name: $ROS2_CONTAINER_NAME"
echo "Press Ctrl+C to stop the container."
echo

ros2_stop_container() {
    if docker container inspect "$ROS2_CONTAINER_NAME" >/dev/null 2>&1; then
        local status
        status="$(docker inspect -f '{{.State.Status}}' "$ROS2_CONTAINER_NAME" 2>/dev/null || true)"
        if [[ "$status" == "running" ]]; then
            echo ""
            echo "Stopping container '$ROS2_CONTAINER_NAME'..."
            docker stop -t 10 "$ROS2_CONTAINER_NAME" 2>/dev/null || true
        fi
    fi
}

ros2_on_signal() {
    if [[ "${ROS2_RUN_ACTIVE:-0}" == "1" ]]; then
        ros2_stop_container
    fi
    trap - INT TERM
    exit 130
}

ROS2_RUN_ACTIVE=1
trap ros2_on_signal INT TERM

set +e
docker run --rm -i --init \
    --name "$ROS2_CONTAINER_NAME" \
    --workdir /ros2-workspace \
    --mount "type=bind,source=$ROS2_SRC,target=/ros2" \
    --mount "type=volume,source=$ROS2_WS_VOLUME,target=/ros2-workspace" \
    "$ROS2_IMAGE" \
    bash -s -- "${ROS2_ARGS[@]}" <<'EOS'
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive
export LANG=C.UTF-8
export LC_ALL=C.UTF-8

echo "========== Setting up ROS 2 apt repository =========="
apt-get update -qq
apt-get install -y -qq --no-install-recommends curl gnupg lsb-release ca-certificates

curl -fsSL https://raw.githubusercontent.com/ros/rosdistro/master/ros.asc \
    | gpg --dearmor -o /usr/share/keyrings/ros-archive-keyring.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/ros-archive-keyring.gpg] \
    http://packages.ros.org/ros2/ubuntu $(lsb_release -cs) main" \
    > /etc/apt/sources.list.d/ros2.list
apt-get update -qq

echo "========== Installing build dependencies =========="
apt-get install -y -qq --no-install-recommends \
    cmake ninja-build gcc g++ git python3-pip \
    python3-colcon-common-extensions \
    python3-rosdep python3-vcstool \
    libasio-dev libtinyxml2-dev libssl-dev

echo "========== Importing ROS 2 source tree =========="
mkdir -p /ros2-workspace/src
if [[ ! -f /ros2-workspace/src/.repos-imported ]]; then
    vcs import /ros2-workspace/src < /ros2/ros2.repos
    touch /ros2-workspace/src/.repos-imported
else
    echo "Source tree already imported; skipping vcs import."
fi

echo "========== Installing rosdep dependencies =========="
rosdep init 2>/dev/null || true
rosdep update --rosdistro rolling
rosdep install --from-paths /ros2-workspace/src --ignore-src -y \
    --skip-keys "fastcdr rti-connext-dds-6.0.1 urdfdom_headers"

echo "========== Building ROS 2 =========="
colcon build \
    --base-paths /ros2-workspace \
    --build-base /ros2-workspace/build \
    --install-base /ros2-workspace/install \
    --symlink-install \
    --cmake-args -DCMAKE_BUILD_TYPE=Release \
    --event-handlers console_cohesion+

echo "========== Starting ROS 2 =========="
source /ros2-workspace/install/setup.bash
if [[ $# -eq 0 ]]; then
    ros2 daemon start
    exec bash --login
else
    exec ros2 "$@"
fi
EOS
rc=$?
set -e

ROS2_RUN_ACTIVE=0
trap - INT TERM

exit "$rc"
