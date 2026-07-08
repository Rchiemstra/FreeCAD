#!/bin/sh
set -e

mkdir -p "$CCACHE_DIR"
ccache -M "$CCACHE_MAXSIZE" || true
cmake --preset debug -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache
