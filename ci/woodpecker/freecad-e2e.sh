#!/bin/sh
set -e

# Same contract as freecad-unit-tests.sh Gui_tests_run (xcb + xvfb only).
# Pipeline 354 with LIBGL_ALWAYS_SOFTWARE=1 died in 48s (exit 1). 350-352
# without QT_QPA_PLATFORM ran ~4.5m then SIGSEGV (245). Do not force llvmpipe.
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
export PYTHONUNBUFFERED=1

# The deps image has no gdb. run_gui_tests.py re-runs a crashing module under
# gdb for a backtrace; install it when the step has apt (Woodpecker does).
if ! command -v gdb >/dev/null 2>&1; then
  apt-get update -qq
  apt-get install -y -qq --no-install-recommends gdb >/dev/null
fi

env QT_QPA_PLATFORM="$QT_QPA_PLATFORM" \
  xvfb-run -a -s "-screen 0 1024x768x24" \
  python3 .github/scripts/run_gui_tests.py build/debug
