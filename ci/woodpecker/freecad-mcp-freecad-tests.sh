#!/bin/sh
set -e

# Sketch/part/rest use when.status: [success, failure] so a sibling e2e failure
# does not skip them. That same flag also starts them when ci-images-ready or
# debug-build never produced FreeCADCmd. Skip cleanly in that case (exit 0).
# Do not skip when the binary exists -- real test failures must still go red.
FC="$CI_WORKSPACE/build/debug/bin/FreeCADCmd"
if [ -n "${CORE_SHARD:-}" ] && [ ! -x "$FC" ]; then
	echo "[freecad-mcp-freecad-tests] CORE_SHARD=$CORE_SHARD: FreeCADCmd not found at $FC" >&2
	echo "[freecad-mcp-freecad-tests] images-ready/debug-build did not run; skipping this shard (not a test failure)" >&2
	exit 0
fi

cd tools/mcp/freecad-mcp
pip install --no-build-isolation --no-deps -e .
rm -f "ci_rc_${MARKER}.txt" "results_${MARKER}.xml"

# Optional CORE_SHARD=collab|sketch|part|rest restricts pytest -m core to one
# filename partition via PYTEST_ADDOPTS (pytest.main honours it). Sequential
# Woodpecker shards then fail fast instead of one ~2.5 h core step.
if [ -n "${CORE_SHARD:-}" ]; then
	shard_script="$CI_WORKSPACE/ci/woodpecker/freecad-mcp-core-shards.py"
	paths=$(python3 "$shard_script" --root "$PWD" --shard "$CORE_SHARD") || {
		echo "core shard '$CORE_SHARD' produced no paths" >&2
		exit 1
	}
	if [ -n "${PYTEST_ADDOPTS:-}" ]; then
		PYTEST_ADDOPTS="$PYTEST_ADDOPTS $paths"
	else
		PYTEST_ADDOPTS=$paths
	fi
	export PYTEST_ADDOPTS
	echo "[freecad-mcp-freecad-tests] CORE_SHARD=$CORE_SHARD PYTEST_ADDOPTS has $(echo "$paths" | wc -w) path(s)"
fi

export LD_LIBRARY_PATH="$CI_WORKSPACE/build/debug/lib${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"

set +e
"$FC" ci/run_freecad_tests.py < /dev/null
rc=$?
[ "$rc" -ne 0 ] && exit "$rc"
if [ ! -f "ci_rc_${MARKER}.txt" ]; then
	echo "ci_rc_${MARKER}.txt missing: FreeCADCmd exited 0 but never wrote a verdict (likely crashed mid-run); treating as failure" >&2
	exit 1
fi
exit "$(cat "ci_rc_${MARKER}.txt")"
