#!/bin/sh
set -e

mkdir -p /tmp/gtest
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Preflight the whole inventory before running anything. Never infer coverage
# from whichever executables happen to exist in an incomplete build.
expected=$(python3 "$script_dir/check-test-results.py" inventory build/debug/tests)
results=$(mktemp -d /tmp/gtest/run.XXXXXX)
echo "== C++ test results: $results =="
started=$(date +%s)
rc=0
failed=""
all_reports=""

# Keep the GUI families isolated; validate each process and report separately.
run_gtest() {
  t="$1"
  name=$(basename "$t")
  reports=""
  main_log="$results/$name.log"
  : >"$main_log"
  binary_status=0
  if [ "$name" = "Gui_tests_run" ]; then
    filters='-CollaborationDomainIntegration*:CollaborationDomainIntegrationStandalone*:CollaborationViewIsolation*:CollaborationResponsiveness*
CollaborationDomainIntegration*:CollaborationDomainIntegrationStandalone*
CollaborationViewIsolation*
CollaborationResponsiveness*'
  else
    filters='*'
  fi
  index=0
  # Each filter is a line without whitespace; suppress pathname expansion.
  set -f
  for part in $filters; do
    index=$((index + 1))
    stem="$results/$name-$index"
    reports="$reports $stem.xml"
    if ! sh "$script_dir/run-gtest.sh" "$t" "$part" "$stem"; then
      binary_status=1
    fi
    cat "$stem.log" >>"$main_log"
  done
  set +f
  return "$binary_status"
}

dump_gtest_failure() {
  name="$1"
  log="$results/$name.log"
  echo "== $name =="
  if [ ! -f "$log" ]; then
    echo "(no log file)"
    return
  fi
  echo "-- failure context (filtered) --"
  # gtest writes each failure as a "<file>:<line>: Failure" header followed by the
  # message body, terminated by a blank line. Match on that structure rather than on
  # keywords: an EXPECT_NEAR body ("The difference between ... exceeds ...") contains
  # none of FAILED/Expected/FAIL/Assertion/Error, so a keyword grep drops it entirely
  # and leaves only the "[  FAILED  ]" summary with no reason attached.
  awk '
    /^\[  FAILED  \]/ { print; next }
    /: Failure$/ || /: Skipped$/ { hold = 20; print; next }
    hold > 0 {
      if ($0 ~ /^[[:space:]]*$/) { hold = 0 } else { print; hold-- }
      next
    }
    /native personal|empty image|renderToImage/ { print }
  ' "$log" 2>/dev/null | tail -n 200 || true
  lines=$(wc -l <"$log" 2>/dev/null || echo 0)
  if [ "$lines" -le 200 ]; then
    echo "-- full log ($lines lines) --"
    cat "$log"
  else
    echo "-- log tail (last 120 of $lines lines) --"
    tail -n 120 "$log"
  fi
}

for name in $expected; do
  t="build/debug/tests/$name"
  binary_rc=0
  if ! run_gtest "$t"; then
    binary_rc=1
  fi
  # Paths are generated under mktemp's /tmp/gtest/run.* and contain no spaces.
  # Check every invocation, including each Gui filter, even after a process fails.
  if ! python3 "$script_dir/check-test-results.py" xml $reports; then
    binary_rc=1
  fi
  all_reports="$all_reports $reports"
  if [ "$binary_rc" -ne 0 ]; then
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

python3 "$script_dir/check-test-results.py" xml $all_reports
echo "== C++ inventory complete: $(echo "$expected" | wc -w) binaries; duration=$(($(date +%s) - started))s =="
