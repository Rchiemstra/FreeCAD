#!/usr/bin/env bash
# Print staged URDF path. Main E2E: RobotCAD nested export only.
set -e

ROOT="${1:-/workspace}"
source "${ROOT}/e2e/robotcad_export_paths.sh"

NESTED="$(ROBOTCAD_NESTED_URDF "${ROOT}")"
GEN="$(ROBOTCAD_GEN_DIR "${ROOT}")"
FLAT="${GEN}/arm_2dof.urdf"
PLACE="${ROOT}/robots/arm_2dof.urdf"

if [[ "${E2E_REQUIRE_ROBOTCAD_URDF:-0}" == "1" ]]; then
  if [[ -f "${NESTED}" ]]; then
    echo "${NESTED}"
    exit 0
  fi
  echo "ERROR: E2E_REQUIRE_ROBOTCAD_URDF=1 but missing ${NESTED}" >&2
  exit 1
fi

if [[ -f "${NESTED}" ]]; then
  echo "${NESTED}"
elif [[ -f "${FLAT}" ]]; then
  echo "${FLAT}"
elif [[ -f "${PLACE}" ]]; then
  echo "${PLACE}"
else
  echo "ERROR: no arm_2dof URDF under generated/ or robots/" >&2
  exit 1
fi
