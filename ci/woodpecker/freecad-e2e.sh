#!/bin/sh
set -e

# Same contract as freecad-unit-tests.sh Gui_tests_run (xcb + xvfb only).
# Pipeline 354 with LIBGL_ALWAYS_SOFTWARE=1 died in 48s (exit 1). 350-352
# without QT_QPA_PLATFORM ran ~4.5m then SIGSEGV (245). Do not force llvmpipe.
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
export PYTHONUNBUFFERED=1
# Per-module wall-clock budget; timeouts fail the gate after gdb diagnostics.
export FREECAD_GUI_TEST_MODULE_TIMEOUT="${FREECAD_GUI_TEST_MODULE_TIMEOUT:-900}"

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)

# The runner attaches on hangs (the child permits ptrace), with a bounded
# gdb-parent rerun if Docker denies attach. Crashes also get a gdb rerun.
sh "$script_dir/ensure-gdb.sh"

log=$(mktemp /tmp/freecad-e2e.XXXXXX.log)
rc_file="${log}.rc"
echo "== e2e log: $log =="

# Stream live output to CI while capturing it for the log gate and preserving
# run_gui_tests.py's exact exit code in POSIX /bin/sh.
(
  set +e
  env QT_QPA_PLATFORM="$QT_QPA_PLATFORM" \
    xvfb-run -a -s "-screen 0 1024x768x24" \
    python3 .github/scripts/run_gui_tests.py build/debug 2>&1
  printf '%s\n' "$?" >"$rc_file"
) | tee "$log"

runner_rc=""
if [ -f "$rc_file" ]; then
  runner_rc=$(cat "$rc_file")
  rm -f "$rc_file"
fi
runner_rc="${runner_rc:-1}"

log_rc=0
python3 "$script_dir/check-log-errors.py" --format e2e \
  --baseline "${FREECAD_LOG_ERROR_BASELINE:-$script_dir/log-error-baseline.json}" \
  "$log" || log_rc=$?

if [ "$runner_rc" -ne 0 ]; then
  exit "$runner_rc"
fi
exit "$log_rc"
