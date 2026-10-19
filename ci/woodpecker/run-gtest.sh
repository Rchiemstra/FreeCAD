#!/bin/sh
# One invocation: preserve process status, then require complete green XML.
set -eu
script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
binary=$(realpath "$1")
name=$(basename "$binary")
filter=$2
stem=$3
# Run the checker as the X client so its subprocess status distinguishes a
# signal from an explicit exit(128 + signal), even when using xvfb-run.
set -- python3 "$script_dir/check-test-results.py" gtest --binary "$name" \
  "--filter=$filter" --executable "$binary" --log "$stem.log" "$stem.xml"
case "$name" in
  Gui_tests_run|GuiShutdown_tests_run|*Gui*_tests_run)
    env QT_QPA_PLATFORM=xcb /usr/bin/xvfb-run -a -s "-screen 0 1024x768x24" \
      "$@"
    ;;
  App_tests_run)
    # Property rename/save tests need a native filesystem rather than a 9p mount.
    (cd /tmp && "$@")
    ;;
  *)
    "$@"
    ;;
esac
