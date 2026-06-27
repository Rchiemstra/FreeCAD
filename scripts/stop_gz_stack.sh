#!/usr/bin/env bash
# SPDX-License-Identifier: LGPL-2.1-or-later
# Stop gz-sim + ros_gz_bridge Docker containers (live stack cleanup).
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
# shellcheck source=scripts/gazebo_lifecycle_common.sh
source "${ROOT}/scripts/gazebo_lifecycle_common.sh"
gz_export_live_env
gz_stop_stack
echo "[gazebo] Stack stopped."
