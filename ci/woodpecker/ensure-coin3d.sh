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

_ensure_apt_sources() {
    if apt-cache show libcoin-dev >/dev/null 2>&1; then
        return 0
    fi
    echo "Enabling universe repository for libcoin-dev..."
    _apt_install software-properties-common
    add-apt-repository -y universe
    apt-get update -qq
}

_coin_ready() {
    if ! test -f /usr/include/Inventor/So.h; then
        return 1
    fi
    if ! ldconfig -p 2>/dev/null | grep -q 'libCoin\.so'; then
        return 1
    fi
    return 0
}

_ensure_coin3d() {
    _ensure_apt_sources
    if _coin_ready; then
        return 0
    fi
    echo "Coin3D toolchain missing; installing libcoin-dev..."
    _apt_install libcoin-dev
    if ! _coin_ready; then
        echo "ERROR: libcoin-dev installed but Coin headers/library are missing" >&2
        exit 1
    fi
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

if [ -z "$COIN_CMAKE_ARGS" ] && command -v coin-config >/dev/null 2>&1; then
    coin_prefix="$(coin-config --prefix 2>/dev/null || true)"
    if [ -n "$coin_prefix" ]; then
        COIN_CMAKE_ARGS="-DCMAKE_PREFIX_PATH=$coin_prefix"
        echo "Using CMAKE_PREFIX_PATH=$coin_prefix from coin-config"
    fi
fi

export COIN_CMAKE_ARGS
