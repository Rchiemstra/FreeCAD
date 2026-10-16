#!/bin/sh
set -e

# Same contract as freecad-unit-tests.sh Gui_tests_run (xcb + xvfb only).
# Pipeline 354 with LIBGL_ALWAYS_SOFTWARE=1 died in 48s (exit 1). 350-352
# without QT_QPA_PLATFORM ran ~4.5m then SIGSEGV (245). Do not force llvmpipe.
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
export PYTHONUNBUFFERED=1
# Per-module wall-clock budget; timeouts fail the gate after gdb diagnostics.
export FREECAD_GUI_TEST_MODULE_TIMEOUT="${FREECAD_GUI_TEST_MODULE_TIMEOUT:-900}"

# The deps image has no gdb. The runner attaches on hangs (the child permits
# ptrace), with a bounded gdb-parent rerun if Docker denies attach. Crashes
# also get a gdb rerun. Install it when the step has apt (Woodpecker does).
if ! command -v gdb >/dev/null 2>&1; then
  if [ "$(id -u)" = 0 ]; then
    apt-get update -qq
    apt-get install -y -qq --no-install-recommends gdb >/dev/null
  fi
fi

env QT_QPA_PLATFORM="$QT_QPA_PLATFORM" \
  xvfb-run -a -s "-screen 0 1024x768x24" \
  python3 .github/scripts/run_gui_tests.py build/debug
