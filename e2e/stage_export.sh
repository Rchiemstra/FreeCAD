#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Stage URDF for E2E: prefer RobotCAD export when robots/*.FCStd exists; otherwise use repo URDF.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FREECAD_CMD="${FREECAD_CMD:-$(command -v FreeCADCmd || command -v freecadcmd-daily || true)}"
GEN="${ROOT}/generated/arm_2dof"
FCSTD="${ROOT}/robots/arm_2dof.FCStd"
URDF_SRC="${ROOT}/robots/arm_2dof.urdf"

if [[ -z "${FREECAD_CMD}" ]]; then
  echo "ERROR: set FREECAD_CMD or install FreeCADCmd"
  exit 1
fi

mkdir -p "${GEN}"

if [[ -f "${FCSTD}" ]]; then
  echo "[e2e] Exporting from ${FCSTD} via FreeCAD + RobotCAD (see e2e/export_robotcad_fcstd.py)"
  xvfb-run -a "${FREECAD_CMD}" "${ROOT}/e2e/export_robotcad_fcstd.py" "${FCSTD}" "${GEN}"
else
  echo "[e2e] No ${FCSTD} — staging checked-in URDF under generated/ (add FCStd for true RobotCAD mesh export)"
  cp "${URDF_SRC}" "${GEN}/arm_2dof.urdf"
fi

ls -la "${GEN}"
