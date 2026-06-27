#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Strict Docker E2E (RobotCAD export + gz_cli spawn + e2e_smoke).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
cd "$ROOT"
# Self-heal CRLF when invoked from Windows checkouts (WSL).
sed -i 's/\r$//' "${BASH_SOURCE[0]}" 2>/dev/null || true

export CI="${CI:-true}"
export RUN_GAZEBO_LIVE=0
unset RUN_GAZEBO_LIVE 2>/dev/null || true
export E2E_VERSION_STRICT="${E2E_VERSION_STRICT:-1}"

echo "══════════════════════════════════════════════════════════════"
echo " CI: strict Docker E2E"
echo " E2E_VERSION_STRICT=$E2E_VERSION_STRICT"
echo "══════════════════════════════════════════════════════════════"

if ! command -v docker >/dev/null 2>&1; then
  echo "ERROR: docker not found on PATH" >&2
  exit 1
fi

if [[ ! -f robots/arm_2dof.FCStd ]]; then
  echo "ERROR: robots/arm_2dof.FCStd missing (required for strict E2E)" >&2
  exit 1
fi

python3 e2e/verify_robot_source.py

if [[ -d generated ]]; then
  echo "[ci] cleaning generated/ (Docker E2E may leave root-owned files on bind mounts)..."
  docker run --rm -v "${ROOT}:/workspace" -w /workspace alpine:3.20 rm -rf generated
fi
echo "[ci] clean generated/ for E2E"

docker compose -f docker/compose.e2e.yml build

docker compose -f docker/compose.e2e.yml up \
  --abort-on-container-exit \
  --exit-code-from e2e

echo "══════════════════════════════════════════════════════════════"
echo " CI: strict Docker E2E PASSED"
echo "══════════════════════════════════════════════════════════════"
