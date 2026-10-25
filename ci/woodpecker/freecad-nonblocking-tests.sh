#!/bin/sh
# Tiered gate for nonblocking document execution and GUI presentation.
# Same image and build as the existing Woodpecker debug pipeline:
#   127.0.0.1:5001/freecad-ci-deps:24.04
# Host Windows runs are not evidence; run from WSL with Docker.
set -e

SCRIPT_DIR=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
REPO_ROOT=$(CDPATH='' cd -- "$SCRIPT_DIR/../.." && pwd)
cd "$REPO_ROOT"

TIER=${TIER:-arch}

run_arch() {
  echo "== TIER=arch: architecture pytest (no FreeCAD build) =="
  # freecad-ci-deps:24.04 does not ship pytest; provision it the same way
  # prior collaboration Docker gates did (pip into the container).
  # CI image scheduling tests also parse the workflow YAML with PyYAML.
  if ! python3 -c 'import pytest, yaml' 2>/dev/null; then
    python3 -m pip install --break-system-packages -q pytest PyYAML
  fi
  export PYTHONUNBUFFERED=1
  python3 -m pytest -vv --tb=line tests/architecture/
}

run_nonblocking_gtest() {
  binary=$1
  filter=$2
  stem=$(mktemp -d /tmp/gtest-nonblocking.XXXXXX)/result
  status=0
  sh "$SCRIPT_DIR/run-gtest.sh" "build/debug/tests/$binary" "$filter" "$stem" || status=$?
  cat "$stem.log"
  return "$status"
}

run_lane() {
  echo "== TIER=lane: execution lane, handles, responsiveness stall =="
  run_nonblocking_gtest App_tests_run \
    'DocumentExecutionLane*:RecomputeHandle*:DocumentRecomputeCoordinator*:DocumentCommitCoordinator*'
  run_nonblocking_gtest Gui_tests_run 'CollaborationResponsiveness*'
}

run_presentation() {
  echo "== TIER=presentation: cache, tree/property/selection, providers, 4ms slice =="
  run_lane
  run_nonblocking_gtest Gui_tests_run \
    'DocumentExecution*:DocumentPresentationCache*:PresentationApplyScheduler*'
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
