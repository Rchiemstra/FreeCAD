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

# A2 boundary: preserve the existing PASSED/no-FAILED teardown crash waiver.
# This changes only process status; structured result validation is mandatory
# after this function returns and cannot be waived here.
accept_teardown_exit() {
  waiver_rc=$1
  waiver_log=$2
  waiver_description=$3
  if [ "$waiver_rc" -ne 0 ] \
    && grep -q '\[  PASSED  \]' "$waiver_log" \
    && ! grep -q '\[  FAILED  \]' "$waiver_log"; then
    echo "WARN: accepting $waiver_description exit $waiver_rc after all tests passed (teardown)" \
      | tee -a "$main_log"
    return 0
  fi
  return "$waiver_rc"
}

run_gtest() {
  t="$1"
  name=$(basename "$t")
  reports=""
  echo "== C++ gtest: $name =="
  case "$name" in
    Gui_tests_run|GuiShutdown_tests_run|*Gui*_tests_run)
      if [ ! -x /usr/bin/xvfb-run ]; then
        echo "FATAL: mandatory /usr/bin/xvfb-run is missing or not executable (required for $name OpenGL tests)" >&2
        exit 1
      fi
      # Heavy Collaboration* Gui suites leave Coin/MainWindow state that causes
      # later in-process suites (ViewIsolation, Responsiveness) to SIGSEGV.
      # Run the rest of Gui_tests_run first, then each Collaboration* family alone.
      if [ "$name" = "Gui_tests_run" ]; then
        main_log="$results/$name.log"
        main_only_log="$results/$name-main-only.log"
        main_xml="$results/$name-main.xml"
        reports="$main_xml"
        : >"$main_log"
        env QT_QPA_PLATFORM=xcb /usr/bin/xvfb-run -a -s "-screen 0 1024x768x24" \
          "$t" \
          --gtest_filter=-CollaborationDomainIntegration*:CollaborationDomainIntegrationStandalone*:CollaborationViewIsolation*:CollaborationResponsiveness* \
          --gtest_output=xml:"$main_xml" >>"$main_log" 2>&1
        main_rc=$?
        # Evaluate main-batch teardown tolerance against the main log only,
        # before Collaboration filters append PASSED lines that could mask a
        # crashed main batch (no [ FAILED ], no main [ PASSED ]).
        cp "$main_log" "$main_only_log"
        accept_teardown_exit "$main_rc" "$main_only_log" "$name main batch"
        main_rc=$?
        collab_rc=0
        for part in \
          'CollaborationDomainIntegration*:CollaborationDomainIntegrationStandalone*' \
          'CollaborationViewIsolation*' \
          'CollaborationResponsiveness*'
        do
          safe=$(echo "$part" | tr -c 'A-Za-z0-9._-' '_')
          part_log="$results/$name-$safe.log"
          part_xml="$results/$name-$safe.xml"
          reports="$reports $part_xml"
          env QT_QPA_PLATFORM=xcb /usr/bin/xvfb-run -a -s "-screen 0 1024x768x24" \
            "$t" --gtest_filter="$part" \
            --gtest_output=xml:"$part_xml" \
            >"$part_log" 2>&1
          part_rc=$?
          cat "$part_log" >>"$main_log"
          # Coin/Qt global teardown can SIGSEGV after all tests passed on this image.
          accept_teardown_exit "$part_rc" "$part_log" "$name filter='$part'"
          part_rc=$?
          if [ "$part_rc" -ne 0 ]; then
            collab_rc=$part_rc
          fi
        done
        if [ "$main_rc" -ne 0 ]; then
          return "$main_rc"
        fi
        return "$collab_rc"
      fi
      reports="$results/$name.xml"
      env QT_QPA_PLATFORM=xcb /usr/bin/xvfb-run -a -s "-screen 0 1024x768x24" \
        "$t" --gtest_output=xml:"$reports" >"$results/$name.log" 2>&1
      ;;
    App_tests_run)
      # Property rename/save tests write relative FCStd paths; on 9p bind mounts
      # retained writer handles break post-rename verification (see doc handoff).
      ws="${CI_WORKSPACE:-$(pwd)}"
      reports="$results/$name.xml"
      (cd /tmp && "$ws/$t" --gtest_output=xml:"$reports") \
        >"$results/$name.log" 2>&1
      ;;
    *)
      reports="$results/$name.xml"
      "$t" --gtest_output=xml:"$reports" >"$results/$name.log" 2>&1
      ;;
  esac
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
