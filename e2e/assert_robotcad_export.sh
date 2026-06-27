#!/usr/bin/env bash
# Verify RobotCAD nested export + /models mount (main E2E gate).
set -e

ROOT="${1:-/workspace}"
source "${ROOT}/e2e/robotcad_export_paths.sh"

NESTED="$(ROBOTCAD_NESTED_URDF "${ROOT}")"
PKG="$(ROBOTCAD_PKG_ROOT "${ROOT}")"

if [[ ! -f "${NESTED}" ]]; then
  echo "ERROR: required URDF missing: ${NESTED}" >&2
  exit 1
fi

if [[ -z "${PKG}" || ! -f "${PKG}/package.xml" ]]; then
  echo "ERROR: RobotCAD package.xml missing under generated/" >&2
  exit 1
fi

if [[ ! -L /models/arm_2dof_description ]]; then
  echo "ERROR: /models/arm_2dof_description symlink missing" >&2
  exit 1
fi

if [[ ! -f /models/arm_2dof_description/urdf/arm_2dof.urdf ]]; then
  echo "ERROR: mounted URDF not visible at /models/arm_2dof_description/urdf/arm_2dof.urdf" >&2
  exit 1
fi

if grep -q 'package://arm_2dof_description/' "${NESTED}"; then
  echo "[e2e] URDF contains package:// mesh references (RobotCAD export)"
elif grep -q 'file:///models/arm_2dof_description/' "${NESTED}"; then
  echo "[e2e] URDF contains file:///models mesh paths"
else
  echo "[e2e] URDF has no package:// meshes (primitives-only export is OK)"
fi

echo "[e2e] RobotCAD export + /models mount verified"
