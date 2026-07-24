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
build_pid=""
started_at="$(date +%s)"

elapsed_minutes() {
  now="$(date +%s)"
  echo "$(((now - started_at) / 60))"
}

dump_interrupted_log() {
  signal="$1"
  echo "== ${label}: received ${signal} after $(elapsed_minutes)m; last 120 log lines =="
  if [ -n "$build_pid" ] && kill -0 "$build_pid" 2>/dev/null; then
    kill "$build_pid" 2>/dev/null || true
  fi
  tail -n 120 "$log" 2>/dev/null || true
}

on_term() {
  dump_interrupted_log "termination signal"
  exit 143
}

on_int() {
  dump_interrupted_log "interrupt signal"
  exit 130
}

trap on_term TERM HUP
trap on_int INT

echo "== ${label}: cmake --build ${build_dir} -j${jobs} =="
rm -f "$log"

cmake --build "$build_dir" -j"$jobs" >"$log" 2>&1 &
build_pid=$!

while kill -0 "$build_pid" 2>/dev/null; do
  sleep 60
  if kill -0 "$build_pid" 2>/dev/null; then
    echo "== ${label}: build still running after $(elapsed_minutes)m; recent output =="
    tail -n 8 "$log" | sed 's/^/  /' || true
  fi
done

set +e
wait "$build_pid"
rc=$?
set -e

if [ "$rc" -ne 0 ]; then
  echo "== ${label}: build failed with exit code ${rc} after $(elapsed_minutes)m; last 400 log lines =="
  tail -n 400 "$log" || true
  exit "$rc"
fi

echo "== ${label}: build completed after $(elapsed_minutes)m; last 40 log lines =="
tail -n 40 "$log" || true
