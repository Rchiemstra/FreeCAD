#!/bin/sh
# Ensure Coin3D/Pivy build dependencies are present and expose Coin_DIR for CMake.
# The CI deps image should already include these packages, but a stale registry
# image or a minimal runner may not.

set -e

_apt_install() {
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y --no-install-recommends "$@"
}

_ensure_coin3d() {
    if [ -f /usr/include/Inventor/So.h ] || [ -f /usr/include/Inventor/C/basic.h ]; then
        return 0
    fi
    echo "Coin3D headers missing; installing libcoin-dev..."
    _apt_install libcoin-dev
}

_ensure_pivy() {
    if python3 -c "import pivy" >/dev/null 2>&1; then
        return 0
    fi
    echo "pivy missing; installing python3-pivy..."
    _apt_install python3-pivy
}

_ensure_coin3d
_ensure_pivy

COIN_CMAKE_ARGS=""
for candidate in \
    /usr/lib/x86_64-linux-gnu/cmake/Coin \
    /usr/lib/cmake/Coin \
    /usr/local/lib/cmake/Coin; do
    if [ -f "$candidate/CoinConfig.cmake" ] || [ -f "$candidate/coin-config.cmake" ]; then
        COIN_CMAKE_ARGS="-DCoin_DIR=$candidate"
        echo "Using Coin_DIR=$candidate"
        break
    fi
done

export COIN_CMAKE_ARGS
