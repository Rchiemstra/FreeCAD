#!/bin/sh
set -e

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
log=$(mktemp /tmp/freecad-integration.XXXXXX.log)
echo "== integration log: $log =="
if timeout 1200 build/debug/bin/FreeCADCmd -t 0 >"$log" 2>&1; then
  command_rc=0
else
  command_rc=$?
fi
cat "$log"
python3 "$script_dir/check-test-results.py" integration --exit-code "$command_rc" "$log"
