#!/usr/bin/env bash
# Shared RobotCAD export paths for E2E (source from other e2e/*.sh).
ROBOTCAD_GEN_DIR() {
  echo "${1:-/workspace}/generated/arm_2dof"
}

ROBOTCAD_NESTED_URDF() {
  local root="${1:-/workspace}"
  echo "$(ROBOTCAD_GEN_DIR "${root}")/arm_2dof_description/arm_2dof_description/urdf/arm_2dof.urdf"
}

ROBOTCAD_PKG_ROOT() {
  local root="${1:-/workspace}"
  local gen
  gen="$(ROBOTCAD_GEN_DIR "${root}")"
  if [[ -d "${gen}/arm_2dof_description/arm_2dof_description" ]]; then
    echo "${gen}/arm_2dof_description/arm_2dof_description"
  elif [[ -d "${gen}/arm_2dof_description" ]]; then
    echo "${gen}/arm_2dof_description"
  fi
}
