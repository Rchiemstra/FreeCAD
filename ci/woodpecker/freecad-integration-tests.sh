#!/bin/sh
set -e

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
# Hang guard with headroom for pipelines sharing the CI agent.
integration_timeout="${FREECAD_INTEGRATION_TEST_TIMEOUT:-2400}"
log=$(mktemp /tmp/freecad-integration.XXXXXX.log)
echo "== integration log: $log =="
if timeout "$integration_timeout" build/debug/bin/FreeCADCmd -t 0 >"$log" 2>&1; then
  command_rc=0
else
  command_rc=$?
fi
cat "$log"
if [ "$command_rc" -eq 124 ]; then
  echo "FATAL: integration process exit 124: hang guard reached ${integration_timeout}s" >&2
  exit 1
fi
python3 "$script_dir/check-test-results.py" integration --exit-code "$command_rc" "$log"
