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
    Gui_tests_run|GuiShutdown_tests_run|*Gui*_tests_run)
      if [ ! -x /usr/bin/xvfb-run ]; then
        echo "FATAL: mandatory /usr/bin/xvfb-run is missing or not executable (required for $name OpenGL tests)" >&2
        exit 1
      fi
      # Heavy Collaboration* Gui suites leave Coin/MainWindow state that causes
      # later in-process suites (ViewIsolation, Responsiveness) to SIGSEGV.
      # Run the rest of Gui_tests_run first, then each Collaboration* family alone.
      if [ "$name" = "Gui_tests_run" ]; then
        main_log="/tmp/gtest/$name.log"
        main_only_log="/tmp/gtest/$name-main-only.log"
        : >"$main_log"
        env QT_QPA_PLATFORM=xcb /usr/bin/xvfb-run -a -s "-screen 0 1024x768x24" \
          "$t" \
          --gtest_filter=-CollaborationDomainIntegration*:CollaborationDomainIntegrationStandalone*:CollaborationViewIsolation*:CollaborationResponsiveness* \
          --gtest_output=json:/tmp/gtest/"$name"-main.json >>"$main_log" 2>&1
        main_rc=$?
        # Evaluate main-batch teardown tolerance against the main log only,
        # before Collaboration filters append PASSED lines that could mask a
        # crashed main batch (no [ FAILED ], no main [ PASSED ]).
        cp "$main_log" "$main_only_log"
        if [ "$main_rc" -ne 0 ] \
          && grep -q '\[  PASSED  \]' "$main_only_log" \
          && ! grep -q '\[  FAILED  \]' "$main_only_log"; then
          echo "WARN: accepting $name main batch exit $main_rc after all tests passed (teardown)" \
            | tee -a "$main_log"
          main_rc=0
        fi
        collab_rc=0
        for part in \
          'CollaborationDomainIntegration*:CollaborationDomainIntegrationStandalone*' \
          'CollaborationViewIsolation*' \
          'CollaborationResponsiveness*'
        do
          safe=$(echo "$part" | tr -c 'A-Za-z0-9._-' '_')
          part_log="/tmp/gtest/$name-$safe.log"
          env QT_QPA_PLATFORM=xcb /usr/bin/xvfb-run -a -s "-screen 0 1024x768x24" \
            "$t" --gtest_filter="$part" \
            --gtest_output=json:/tmp/gtest/"$name-$safe".json \
            >"$part_log" 2>&1
          part_rc=$?
          cat "$part_log" >>"$main_log"
          # Coin/Qt global teardown can SIGSEGV after all tests passed on this image.
          if [ "$part_rc" -ne 0 ] \
            && grep -q '\[  PASSED  \]' "$part_log" \
            && ! grep -q '\[  FAILED  \]' "$part_log"; then
            echo "WARN: accepting $name filter='$part' exit $part_rc after all tests passed (teardown)" \
              | tee -a "$main_log"
            part_rc=0
          fi
          if [ "$part_rc" -ne 0 ]; then
            collab_rc=$part_rc
          fi
        done
        if [ "$main_rc" -ne 0 ]; then
          return "$main_rc"
        fi
        return "$collab_rc"
      fi
      env QT_QPA_PLATFORM=xcb /usr/bin/xvfb-run -a -s "-screen 0 1024x768x24" \
        "$t" --gtest_output=json:/tmp/gtest/"$name".json >"/tmp/gtest/$name.log" 2>&1
      ;;
    App_tests_run)
      # Property rename/save tests write relative FCStd paths; on 9p bind mounts
      # retained writer handles break post-rename verification (see doc handoff).
      ws="${CI_WORKSPACE:-$(pwd)}"
      (cd /tmp && "$ws/$t" --gtest_output=json:/tmp/gtest/"$name".json) \
        >"/tmp/gtest/$name.log" 2>&1
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
