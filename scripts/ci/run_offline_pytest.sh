#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Offline pytest gate (bridge, runner, iteration, SimWorkbench logic).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

export CI="${CI:-true}"
export RUN_GAZEBO_LIVE=0
unset RUN_GAZEBO_LIVE 2>/dev/null || true

echo "══════════════════════════════════════════════════════════════"
echo " CI: offline pytest"
echo " ROOT=$ROOT"
echo " CI=$CI  RUN_GAZEBO_LIVE=${RUN_GAZEBO_LIVE-<unset>}"
echo "══════════════════════════════════════════════════════════════"

python3 -m pip install -q -r requirements-dev.txt

python3 -m pytest tests/ \
  -v \
  --tb=short \
  --strict-markers \
  -m "not gazebo and not freecad and not needs_freecad"

echo "══════════════════════════════════════════════════════════════"
echo " CI: offline pytest PASSED"
echo "══════════════════════════════════════════════════════════════"
