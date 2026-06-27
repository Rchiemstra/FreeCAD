#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
#
# Local CI gate runner (offline pytest, lifecycle smoke, Docker E2E)
#
# Usage:
#   bash scripts/ci/run_local.sh              # all gates
#   bash scripts/ci/run_local.sh offline      # pytest only
#   bash scripts/ci/run_local.sh lifecycle    # lifecycle smoke only
#   bash scripts/ci/run_local.sh e2e          # Docker E2E only (slow)
#
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"

for f in scripts/ci/*.sh scripts/gazebo_lifecycle_common.sh scripts/smoke_gz_lifecycle.sh; do
  if [[ -f "$f" ]]; then
    sed -i 's/\r$//' "$f" 2>/dev/null || true
  fi
done

run() {
  echo ""
  echo "▶ $*"
  "$@"
}

GATES=("$@")
if [[ ${#GATES[@]} -eq 0 ]]; then
  GATES=(offline lifecycle e2e)
fi

for gate in "${GATES[@]}"; do
  case "$gate" in
    offline)
      run bash scripts/ci/run_offline_pytest.sh
      ;;
    lifecycle)
      run bash scripts/ci/run_lifecycle_smoke.sh
      ;;
    e2e|docker|docker-e2e)
      run bash scripts/ci/run_docker_e2e.sh
      ;;
    *)
      echo "Unknown gate: $gate (use: offline, lifecycle, e2e)" >&2
      exit 2
      ;;
  esac
done

echo ""
echo "All requested CI gates passed: ${GATES[*]}"
