#!/bin/sh
set -eu
SAN="-fsanitize=undefined,float-cast-overflow -fno-sanitize-recover=all"
ASAN="-fsanitize=address -fno-sanitize-recover=all"
CXX="g++ -std=c++20 -O0 -g -I/src -I/standalone"

echo "=== known-bad old arithmetic (must trap) ==="
$CXX $SAN /standalone/ubsan_old_grid_offsets.cpp -o /tmp/ubsan_old_grid
for case in 1 2 3; do
  if /tmp/ubsan_old_grid "$case"; then
    echo "FAIL: old case $case exited 0"
    exit 1
  fi
  echo "old case $case trapped as expected"
done

echo "=== planner regressions (must pass under the same sanitizers) ==="
$CXX $SAN /standalone/test_grid_offset_overflow.cpp -o /tmp/grid_offsets
/tmp/grid_offsets

echo "=== existing camera/grid guards ==="
$CXX $SAN /standalone/test_camera_grid_guards.cpp -o /tmp/camera_grid_guards
/tmp/camera_grid_guards

echo "=== Coin ownership / LeakSanitizer ==="
$CXX $ASAN /standalone/test_grid_coin_ownership.cpp -o /tmp/grid_coin
/tmp/grid_coin

echo "all grid sanitizer probes ok"
