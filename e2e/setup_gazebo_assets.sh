#!/usr/bin/env bash
# Bind RobotCAD export tree for Gazebo mesh resolution (package:// / file://).
set -e

ROOT="${1:-/workspace}"
source "${ROOT}/e2e/robotcad_export_paths.sh"

PKG_ROOT="$(ROBOTCAD_PKG_ROOT "${ROOT}")"
REQUIRE="${E2E_REQUIRE_MODELS_MOUNT:-0}"

mkdir -p /models
if [[ -n "${PKG_ROOT}" && -f "${PKG_ROOT}/package.xml" ]]; then
  ln -sfn "${PKG_ROOT}" /models/arm_2dof_description
  echo "[e2e] GZ resource: /models/arm_2dof_description -> ${PKG_ROOT}"
else
  if [[ "${REQUIRE}" == "1" ]]; then
    echo "ERROR: E2E_REQUIRE_MODELS_MOUNT=1 but RobotCAD package tree not found" >&2
    exit 1
  fi
  echo "[e2e] No RobotCAD package tree — skipping /models mount"
fi

export GZ_SIM_RESOURCE_PATH="/models:${GZ_SIM_RESOURCE_PATH:-}"
