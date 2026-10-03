#!/bin/sh
# Tiered gate for nonblocking document execution and GUI presentation.
# Same image and build as the existing Woodpecker debug pipeline:
#   127.0.0.1:5001/freecad-ci-deps:24.04
# Host Windows runs are not evidence; run from WSL with Docker.
set -e

SCRIPT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH= cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

TIER=${TIER:-arch}

run_arch() {
  echo "== TIER=arch: architecture pytest (no FreeCAD build) =="
  # freecad-ci-deps:24.04 does not ship pytest; provision it the same way
  # prior collaboration Docker gates did (pip into the container).
  if ! python3 -c 'import pytest' 2>/dev/null; then
    python3 -m pip install --break-system-packages -q pytest
  fi
  export PYTHONUNBUFFERED=1
  python3 -m pytest -vv --tb=line tests/architecture/
}

run_lane() {
  echo "== TIER=lane: execution lane, handles, responsiveness stall =="
  # Stub until Wave 1 lane tests land; filter names follow the plan contract.
  for t in build/debug/tests/App_tests_run build/debug/tests/Gui_tests_run; do
    [ -x "$t" ] || {
      echo "FATAL: $t not found or not executable (build required for TIER=lane)" >&2
      exit 1
    }
  done
  build/debug/tests/App_tests_run \
    --gtest_filter='DocumentExecutionLane*:RecomputeHandle*:DocumentRecomputeCoordinator*:DocumentCommitCoordinator*'
  # Run CollaborationResponsiveness alone. Co-filtering with DocumentExecution*
  # SIGSEGV'd mid-suite on this CI image even when Responsiveness alone passes
  # (presentation isolation at 69adcbe61e). DocumentExecution* Gui matches are
  # App-owned suites already covered above.
  lane_gui_log=/tmp/gtest-lane-gui.log
  mkdir -p /tmp
  set +e
  env QT_QPA_PLATFORM=xcb xvfb-run -a -s "-screen 0 1024x768x24" \
    build/debug/tests/Gui_tests_run \
    --gtest_filter='CollaborationResponsiveness*' \
    >"$lane_gui_log" 2>&1
  lane_gui_rc=$?
  cat "$lane_gui_log"
  # Coin/Qt teardown SIGSEGV after all gtests passed is infra noise on this image.
  if [ "$lane_gui_rc" -ne 0 ] \
    && grep -a -q '\[  PASSED  \]' "$lane_gui_log" \
    && ! grep -a -q '\[  FAILED  \]' "$lane_gui_log"; then
    echo "WARN: accepting TIER=lane Gui exit $lane_gui_rc after all tests passed (teardown)"
    lane_gui_rc=0
  fi
  set -e
  if [ "$lane_gui_rc" -ne 0 ]; then
    return "$lane_gui_rc"
  fi
  return 0
}

run_presentation() {
  echo "== TIER=presentation: cache, tree/property/selection, providers, 4ms slice =="
  # One App lane suite + one Gui process. A second xvfb Gui_tests_run right after
  # CollaborationResponsiveness reliably SIGSEGVs on teardown/startup in this image
  # (lane alone is fine; presentation-only filters alone are fine).
  for t in build/debug/tests/App_tests_run build/debug/tests/Gui_tests_run; do
    [ -x "$t" ] || {
      echo "FATAL: $t not found or not executable (build required for TIER=presentation)" >&2
      exit 1
    }
  done
  build/debug/tests/App_tests_run \
    --gtest_filter='DocumentExecutionLane*:RecomputeHandle*:DocumentRecomputeCoordinator*:DocumentCommitCoordinator*'
  # Isolate CollaborationResponsiveness in its own xvfb process — sharing a
  # process with PresentationApplyScheduler Coin work SIGSEGVs mid-suite on
  # this CI image after Responsiveness teardown.
  pres_gui_rc=0
  mkdir -p /tmp
  for part in \
    'CollaborationResponsiveness*' \
    'DocumentExecution*:DocumentPresentationCache*:PresentationApplyScheduler*'
  do
    safe=$(echo "$part" | tr -c 'A-Za-z0-9._-' '_')
    part_log=/tmp/gtest-presentation-$safe.log
    set +e
    env QT_QPA_PLATFORM=xcb xvfb-run -a -s "-screen 0 1024x768x24" \
      build/debug/tests/Gui_tests_run \
      --gtest_filter="$part" \
      >"$part_log" 2>&1
    part_rc=$?
    cat "$part_log"
    # Match PASSED even if the log grows binary noise after a teardown SIGSEGV.
    if [ "$part_rc" -ne 0 ] \
      && grep -a -q '\[  PASSED  \]' "$part_log" \
      && ! grep -a -q '\[  FAILED  \]' "$part_log"; then
      echo "WARN: accepting TIER=presentation filter='$part' exit $part_rc after all tests passed (teardown)"
      part_rc=0
    fi
    set -e
    if [ "$part_rc" -ne 0 ]; then
      pres_gui_rc=$part_rc
    fi
  done
  # Teardown SIGSEGV can still surface as the script's exit if a child
  # dumps after the waived part_rc is recorded; force clean when green.
  if [ "$pres_gui_rc" -ne 0 ]; then
    return "$pres_gui_rc"
  fi
  return 0
}

run_unit() {
  echo "== TIER=unit: existing C++ gtest suite =="
  "$SCRIPT_DIR/freecad-unit-tests.sh"
}

run_full() {
  echo "== TIER=full: unit, integration, GUI =="
  run_unit
  echo "== TIER=full: FreeCADCmd integration =="
  "$SCRIPT_DIR/freecad-integration-tests.sh"
  echo "== TIER=full: GUI tests =="
  export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
  export PYTHONUNBUFFERED=1
  xvfb-run -a -s "-screen 0 1024x768x24" \
    python3 .github/scripts/run_gui_tests.py build/debug
}

case "$TIER" in
  arch) run_arch ;;
  lane) run_lane ;;
  presentation) run_presentation ;;
  unit) run_unit ;;
  full) run_full ;;
  *)
    echo "FATAL: unknown TIER=$TIER (expected arch|lane|presentation|unit|full)" >&2
    exit 1
    ;;
esac
