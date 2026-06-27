#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Fetch robots/arm_2dof.FCStd when missing (CI artifact path).
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FCSTD="${ROOT}/robots/arm_2dof.FCStd"
URL="${ROBOTS_ARM_2DOF_FCSTD_URL:-}"

if [[ -f "${FCSTD}" ]]; then
  exit 0
fi

if [[ -z "${URL}" ]]; then
  URL="$(python3 -c "import sys; from pathlib import Path; sys.path.insert(0, '${ROOT}'); from bridge.runtime_versions import fcstd_spec, load_runtime_lock; print((fcstd_spec(load_runtime_lock(Path('${ROOT}'))) or {}).get('ci_artifact_url') or '')")"
fi

if [[ -z "${URL}" ]]; then
  echo "ERROR: ${FCSTD} missing and ROBOTS_ARM_2DOF_FCSTD_URL unset" >&2
  exit 1
fi

mkdir -p "${ROOT}/robots"
echo "[e2e] Fetching robot source from ${URL}"
curl -fsSL -o "${FCSTD}.tmp" "${URL}"
mv "${FCSTD}.tmp" "${FCSTD}"
python3 "${ROOT}/e2e/verify_robot_source.py"
