#!/bin/sh
set -e

mkdir -p "$CCACHE_DIR"
export CCACHE_BASEDIR="$CI_WORKSPACE"
ccache -M "$CCACHE_MAXSIZE" || true
ccache -z || true
ci/run_cmake_build_with_log.sh build/debug debug
echo "== ccache stats after debug build =="
ccache -s || true
