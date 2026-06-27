#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Placeholder URDF only — for e2e_placeholder_fallback test (not main E2E).
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT}/e2e/robotcad_export_paths.sh"

GEN="$(ROBOTCAD_GEN_DIR "${ROOT}")"
URDF_SRC="${ROOT}/robots/arm_2dof.urdf"

rm -rf "${GEN}"
mkdir -p "${GEN}"
cp "${URDF_SRC}" "${GEN}/arm_2dof.urdf"
echo "[e2e] Placeholder fallback staged: ${GEN}/arm_2dof.urdf"
unset E2E_REQUIRE_MODELS_MOUNT
bash "${ROOT}/e2e/setup_gazebo_assets.sh" "${ROOT}"
