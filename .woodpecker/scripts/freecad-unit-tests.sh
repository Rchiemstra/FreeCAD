#!/bin/sh
set -e

mkdir -p /tmp/gtest
rc=0
for t in build/debug/tests/*_tests_run; do
  [ -x "$t" ] || continue
  name=$(basename "$t")
  echo "== C++ gtest: $name =="
  "$t" --gtest_output=json:/tmp/gtest/"$name".json >"/tmp/gtest/$name.log" 2>&1 || rc=1
done

if [ "$rc" -ne 0 ]; then
  echo "one or more C++ gtest binaries failed; logs in /tmp/gtest/*.log"
  exit 1
fi
