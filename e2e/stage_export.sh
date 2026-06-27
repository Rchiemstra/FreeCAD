#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Stage URDF via FreeCADCmd + RobotCAD (strict — no placeholder fallback).
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=e2e/robotcad_export_paths.sh
source "${ROOT}/e2e/robotcad_export_paths.sh"

FREECAD_CMD="${FREECAD_CMD:-$(command -v FreeCADCmd || command -v freecadcmd-daily || true)}"
GEN="$(ROBOTCAD_GEN_DIR "${ROOT}")"
FCSTD="${ROOT}/robots/arm_2dof.FCStd"
NESTED="$(ROBOTCAD_NESTED_URDF "${ROOT}")"

if [[ -z "${FREECAD_CMD}" ]]; then
  echo "ERROR: set FREECAD_CMD or install FreeCADCmd"
  exit 1
fi

if [[ ! -f "${FCSTD}" ]]; then
  echo "ERROR: RobotCAD E2E requires ${FCSTD} (commit or mount robots/arm_2dof.FCStd)"
  exit 1
fi

rm -rf "${GEN}"
mkdir -p "${GEN}"

echo "[e2e] RobotCAD export via FreeCADCmd: ${FCSTD} -> ${GEN}"
sed -i 's/\r$//' "${ROOT}/scripts/robotcad_headless.py"
if ! bash "${ROOT}/e2e/export_robotcad_cmd.sh" "${FCSTD}" "${GEN}" arm_2dof; then
  echo "ERROR: FreeCADCmd RobotCAD export failed" >&2
  exit 1
fi

if [[ ! -f "${NESTED}" ]]; then
  echo "ERROR: RobotCAD nested URDF missing: ${NESTED}" >&2
  echo "Listing ${GEN}:" >&2
  find "${GEN}" -type f 2>/dev/null | head -30 >&2 || true
  exit 1
fi

PKG="$(ROBOTCAD_PKG_ROOT "${ROOT}")"
if [[ -z "${PKG}" || ! -f "${PKG}/package.xml" ]]; then
  echo "ERROR: RobotCAD package root (package.xml) not found under ${GEN}" >&2
  exit 1
fi

echo "[e2e] RobotCAD export OK: ${NESTED}"
export E2E_REQUIRE_MODELS_MOUNT=1
bash "${ROOT}/e2e/setup_gazebo_assets.sh" "${ROOT}"
