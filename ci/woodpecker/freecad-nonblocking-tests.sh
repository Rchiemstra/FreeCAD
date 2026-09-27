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
  env QT_QPA_PLATFORM=xcb xvfb-run -a -s "-screen 0 1024x768x24" \
    build/debug/tests/Gui_tests_run \
    --gtest_filter='CollaborationResponsiveness*:DocumentExecution*'
}

run_presentation() {
  echo "== TIER=presentation: cache, tree/property/selection, providers, 4ms slice =="
  run_lane
  for t in build/debug/tests/Gui_tests_run; do
    [ -x "$t" ] || {
      echo "FATAL: $t not found or not executable (build required for TIER=presentation)" >&2
      exit 1
    }
  done
  env QT_QPA_PLATFORM=xcb xvfb-run -a -s "-screen 0 1024x768x24" \
    build/debug/tests/Gui_tests_run \
    --gtest_filter='DocumentPresentationCache*:PresentationApplyScheduler*'
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
