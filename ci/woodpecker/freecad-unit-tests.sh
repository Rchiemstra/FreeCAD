#!/bin/sh
set -e

. "$(dirname "$0")/ensure-coin3d.sh"

mkdir -p /tmp/gtest
rc=0
failed=""
for t in build/debug/tests/*_tests_run; do
  [ -x "$t" ] || continue
  name=$(basename "$t")
  echo "== C++ gtest: $name =="
  if ! "$t" --gtest_output=json:/tmp/gtest/"$name".json >"/tmp/gtest/$name.log" 2>&1; then
    rc=1
    failed="$failed $name"
  fi
done

if [ "$rc" -ne 0 ]; then
  echo "one or more C++ gtest binaries failed:"
  for name in $failed; do
    echo "== $name =="
    tail -n 40 "/tmp/gtest/$name.log" || true
  done
  exit 1
fi
