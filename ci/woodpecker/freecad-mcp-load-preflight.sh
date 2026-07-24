#!/bin/sh
set -e

cd tools/mcp/freecad-mcp
pip install --no-build-isolation --no-deps -e .

FC="$CI_WORKSPACE/build/debug/bin/FreeCADCmd"
export LD_LIBRARY_PATH="$CI_WORKSPACE/build/debug/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

"$FC" ci/preflight_imports.py < /dev/null > /tmp/pf.log 2>&1 || true
cat /tmp/pf.log
grep -q "PREFLIGHT_OK" /tmp/pf.log || { echo "preflight imports failed (see log above)"; exit 1; }
