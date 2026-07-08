#!/bin/sh
set -e

mkdir -p "$CCACHE_DIR"
export CCACHE_BASEDIR="$CI_WORKSPACE"
ccache -M "$CCACHE_MAXSIZE" || true

echo "== ccache stats before release build =="
ccache -s || true

cmake --preset release -G Ninja \
  -DCMAKE_C_COMPILER=/usr/bin/gcc \
  -DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  -DCMAKE_C_COMPILER_LAUNCHER=ccache \
  -DCMAKE_CXX_COMPILER_LAUNCHER=ccache

ci/run_cmake_build_with_log.sh build/release release

echo "== ccache stats after release build =="
ccache -s || true
