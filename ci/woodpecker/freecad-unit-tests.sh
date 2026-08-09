#!/bin/sh
set -e

mkdir -p /tmp/gtest
rc=0
failed=""

run_gtest() {
  t="$1"
  name=$(basename "$t")
  echo "== C++ gtest: $name =="
  case "$name" in
    Gui_tests_run)
      if [ ! -x /usr/bin/xvfb-run ]; then
        echo "FATAL: mandatory /usr/bin/xvfb-run is missing or not executable (required for $name OpenGL tests)" >&2
        exit 1
      fi
      env QT_QPA_PLATFORM=xcb /usr/bin/xvfb-run -a -s "-screen 0 1024x768x24" \
        "$t" --gtest_output=json:/tmp/gtest/"$name".json >"/tmp/gtest/$name.log" 2>&1
      ;;
    *)
      "$t" --gtest_output=json:/tmp/gtest/"$name".json >"/tmp/gtest/$name.log" 2>&1
      ;;
  esac
}

dump_gtest_failure() {
  name="$1"
  log="/tmp/gtest/$name.log"
  echo "== $name =="
  if [ ! -f "$log" ]; then
    echo "(no log file)"
    return
  fi
  echo "-- failure context (filtered) --"
  grep -E 'FAILED|Expected|FAIL|Assertion|Error|native personal|empty image|renderToImage' "$log" 2>/dev/null \
    | tail -n 80 || true
  lines=$(wc -l <"$log" 2>/dev/null || echo 0)
  if [ "$lines" -le 200 ]; then
    echo "-- full log ($lines lines) --"
    cat "$log"
  else
    echo "-- log tail (last 120 of $lines lines) --"
    tail -n 120 "$log"
  fi
}

for t in build/debug/tests/*_tests_run; do
  [ -x "$t" ] || continue
  name=$(basename "$t")
  if ! run_gtest "$t"; then
    rc=1
    failed="$failed $name"
  fi
done

if [ "$rc" -ne 0 ]; then
  echo "one or more C++ gtest binaries failed:"
  for name in $failed; do
    dump_gtest_failure "$name"
  done
  exit 1
fi
