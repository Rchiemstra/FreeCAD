#!/bin/sh
set -e

cd tools/mcp/freecad-mcp
pip install --no-build-isolation --no-deps -e .
rm -f "ci_rc_${MARKER}.txt" "results_${MARKER}.xml"

FC="$CI_WORKSPACE/build/debug/bin/FreeCADCmd"
export LD_LIBRARY_PATH="$CI_WORKSPACE/build/debug/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

set +e
"$FC" ci/run_freecad_tests.py < /dev/null
rc=$?
[ "$rc" -ne 0 ] && exit "$rc"
exit "$(cat "ci_rc_${MARKER}.txt")"
