#!/bin/sh
set -e

mkdir -p "$CCACHE_DIR"
ccache -M "$CCACHE_MAXSIZE" || true
python3 build_freecad.py --config debug --no-pixi --configure --configure-only \
  --skip-submodules \
  --cmake-arg=-G \
  --cmake-arg=Ninja \
  --cmake-arg=-DCMAKE_C_COMPILER=/usr/bin/gcc \
  --cmake-arg=-DCMAKE_CXX_COMPILER=/usr/bin/g++ \
  --cmake-arg=-DCMAKE_C_COMPILER_LAUNCHER=ccache \
  --cmake-arg=-DCMAKE_CXX_COMPILER_LAUNCHER=ccache
