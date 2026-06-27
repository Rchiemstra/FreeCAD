#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Gazebo lifecycle file/env smoke (no live stack required).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

export CI="${CI:-true}"
export RUN_GAZEBO_LIVE=0
unset RUN_GAZEBO_LIVE 2>/dev/null || true

echo "══════════════════════════════════════════════════════════════"
echo " CI: Gazebo lifecycle smoke (offline)"
echo "══════════════════════════════════════════════════════════════"

if ! python3 -c "import yaml" 2>/dev/null; then
  if python3 -m pip --version >/dev/null 2>&1; then
    python3 -m pip install -q -r requirements-bridge.txt
  elif command -v pip3 >/dev/null 2>&1; then
    pip3 install -q -r requirements-bridge.txt
  else
    echo "ERROR: PyYAML required. Install python3-pip or python3-yaml, then re-run." >&2
    exit 1
  fi
fi

sed -i 's/\r$//' scripts/gazebo_lifecycle_common.sh scripts/smoke_gz_lifecycle.sh 2>/dev/null || true
bash scripts/smoke_gz_lifecycle.sh

echo "══════════════════════════════════════════════════════════════"
echo " CI: lifecycle smoke PASSED"
echo "══════════════════════════════════════════════════════════════"
