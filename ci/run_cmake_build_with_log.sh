#!/usr/bin/env bash
set -euo pipefail

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
  echo "usage: $0 <build-dir> [label]" >&2
  exit 2
fi

build_dir="$1"
label="${2:-$(basename "$build_dir")}"
jobs="${FREECAD_BUILD_JOBS:-$(nproc)}"
log="/tmp/freecad-${label}-build.log"

echo "== ${label}: cmake --build ${build_dir} -j${jobs} =="
rm -f "$log"

cmake --build "$build_dir" -j"$jobs" >"$log" 2>&1 &
build_pid=$!

while kill -0 "$build_pid" 2>/dev/null; do
  sleep 60
  if kill -0 "$build_pid" 2>/dev/null; then
    echo "== ${label}: build still running; recent output =="
    tail -n 8 "$log" | sed 's/^/  /' || true
  fi
done

set +e
wait "$build_pid"
rc=$?
set -e

if [ "$rc" -ne 0 ]; then
  echo "== ${label}: build failed with exit code ${rc}; last 400 log lines =="
  tail -n 400 "$log" || true
  exit "$rc"
fi

echo "== ${label}: build completed; last 40 log lines =="
tail -n 40 "$log" || true
