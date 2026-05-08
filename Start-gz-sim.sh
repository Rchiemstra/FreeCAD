#!/usr/bin/env bash
# Run from WSL: builds gz-sim in Docker (Ubuntu Noble + OSRF packages).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
GZ_SIM_SRC="$ROOT/src/3rdParty/gz-sim"
GZ_BUILD_VOLUME="${GZ_BUILD_VOLUME:-gz-sim-linux-build}"
GZ_IMAGE="${GZ_IMAGE:-ubuntu:noble}"
GZ_ARGS=("$@")

if ! command -v docker >/dev/null 2>&1; then
    echo "Docker was not found on PATH (install Docker in WSL or enable Docker Desktop WSL integration)." >&2
    exit 1
fi

if [[ ! -f "$GZ_SIM_SRC/CMakeLists.txt" ]]; then
    echo "Initializing gz-sim submodule..."
    git -C "$ROOT" submodule update --init --recursive src/3rdParty/gz-sim
fi

docker volume create "$GZ_BUILD_VOLUME" >/dev/null

echo "Building gz-sim in Docker (Ubuntu Noble + OSRF packages)."
echo "Source: $GZ_SIM_SRC"
echo "Build volume: $GZ_BUILD_VOLUME"
echo

docker run --rm -i \
    --workdir /gz-sim \
    --mount "type=bind,source=$GZ_SIM_SRC,target=/gz-sim" \
    --mount "type=volume,source=$GZ_BUILD_VOLUME,target=/gz-sim-build" \
    "$GZ_IMAGE" \
    bash -s -- "${GZ_ARGS[@]}" <<'EOS'
set -euo pipefail

export DEBIAN_FRONTEND=noninteractive

echo "========== Setting up OSRF apt repository =========="
apt-get update -qq
apt-get install -y -qq --no-install-recommends curl lsb-release gnupg ca-certificates

curl -fsSL https://packages.osrfoundation.org/gazebo.gpg \
    -o /usr/share/keyrings/pkgs-osrf-archive-keyring.gpg
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/pkgs-osrf-archive-keyring.gpg] \
    http://packages.osrfoundation.org/gazebo/ubuntu-stable $(lsb_release -cs) main" \
    > /etc/apt/sources.list.d/gazebo-stable.list
echo "deb [arch=$(dpkg --print-architecture) signed-by=/usr/share/keyrings/pkgs-osrf-archive-keyring.gpg] \
    http://packages.osrfoundation.org/gazebo/ubuntu-nightly $(lsb_release -cs) main" \
    > /etc/apt/sources.list.d/gazebo-nightly.list
apt-get update -qq

echo "========== Installing build dependencies =========="
apt-get install -y -qq --no-install-recommends \
    cmake ninja-build gcc g++ git \
    binutils-dev freeglut3-dev libbenchmark-dev libdwarf-dev libdw-dev \
    libfreeimage-dev libglew-dev \
    libgz-rotary-cmake-dev libgz-rotary-common-dev libgz-rotary-fuel-tools-dev \
    libgz-rotary-gui-dev libgz-rotary-math-eigen3-dev libgz-rotary-msgs-dev \
    libgz-rotary-physics-dev libgz-rotary-plugin-dev libgz-rotary-rendering-dev \
    libgz-rotary-sensors-dev libgz-rotary-tools-dev libgz-rotary-transport-dev \
    libgz-rotary-utils-cli-dev libogre-1.9-dev libogre-next-2.3-dev \
    libprotobuf-dev libprotoc-dev libgz-rotary-sdformat-dev libtinyxml2-dev \
    libwebsockets-dev libxi-dev libxmu-dev libpython3-dev \
    python3-gz-rotary-math python3-gz-rotary-msgs python3-gz-rotary-transport \
    python3-pybind11 python3-gz-rotary-sdformat \
    qml6-module-qt-labs-folderlistmodel qml6-module-qt-labs-settings \
    qml6-module-qt5compat-graphicaleffects qml6-module-qtqml-models \
    qml6-module-qtquick-controls qml6-module-qtquick-dialogs \
    qml6-module-qtquick-layouts qml6-module-qtquick \
    qt6-5compat-dev qt6-base-dev qt6-base-private-dev qt6-declarative-dev \
    uuid-dev xvfb x11-utils mesa-utils

echo "========== Configuring gz-sim =========="
mkdir -p /gz-sim-build
cmake -S /gz-sim -B /gz-sim-build \
    -G Ninja \
    -DCMAKE_INSTALL_PREFIX=/usr \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DBUILD_DOCS=OFF \
    -DSKIP_PYBIND11=ON

echo "========== Building gz-sim =========="
cmake --build /gz-sim-build -- -j"$(nproc)"

echo "========== Installing gz-sim =========="
cmake --install /gz-sim-build

echo "========== Starting gz-sim =========="
export DISPLAY=:99
export XDG_RUNTIME_DIR=/tmp/xdg-runtime-root
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
# Xvfb/xkbcomp often warns about missing XF86* keysyms; harmless for headless use.
Xvfb :99 -screen 0 1280x1024x24 -noreset 2>/dev/null &
sleep 1
gz sim "$@"
EOS
