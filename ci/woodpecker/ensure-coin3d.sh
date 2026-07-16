#!/bin/sh
# Ensure Coin3D development files are present and expose Coin_DIR for CMake.
# The CI deps image should already include libcoin-dev, but a stale registry
# image or a minimal runner may not; configure must fail with a clear fix, not
# a blind FATAL_ERROR from SetupCoin3D.

set -e

_ensure_coin3d() {
    if [ -f /usr/include/Inventor/So.h ] || [ -f /usr/include/Inventor/C/basic.h ]; then
        return 0
    fi
    echo "Coin3D headers missing; installing libcoin-dev..."
    export DEBIAN_FRONTEND=noninteractive
    apt-get update -qq
    apt-get install -y --no-install-recommends libcoin-dev
}

_ensure_coin3d

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
