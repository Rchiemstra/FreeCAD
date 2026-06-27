#!/usr/bin/env bash
# FreeCADCmd batch RobotCAD export (inline -c avoids CRLF on .py script path).
set -e

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
FREECAD_CMD="${FREECAD_CMD:-$(command -v FreeCADCmd || command -v freecadcmd-daily)}"
FCSTD="${1:?FCStd path}"
OUT_DIR="${2:?output dir}"
ROBOT_NAME="${3:-arm_2dof}"

if [[ -z "${FREECAD_CMD}" ]]; then
  echo "ERROR: FreeCADCmd not found" >&2
  exit 1
fi

FCSTD_POSIX="$(realpath "${FCSTD}")"
OUT_POSIX="$(realpath "${OUT_DIR}")"
SCRIPTS_POSIX="$(realpath "${ROOT}/scripts")"

CODE="
import sys
sys.path.insert(0, r'${SCRIPTS_POSIX}')
from pathlib import Path
from robotcad_headless import export_fcstd_to_urdf
urdf, _ = export_fcstd_to_urdf(
    Path(r'${FCSTD_POSIX}'),
    Path(r'${OUT_POSIX}'),
    robot_name='${ROBOT_NAME}',
)
print('URDF_EXPORT_PATH:', urdf)
"

OUTPUT="$(xvfb-run -a "${FREECAD_CMD}" -c "${CODE}" 2>&1)" || {
  echo "${OUTPUT}" >&2
  exit 1
}
echo "${OUTPUT}"
if ! echo "${OUTPUT}" | grep -q '^URDF_EXPORT_PATH:'; then
  echo "ERROR: FreeCADCmd did not print URDF_EXPORT_PATH" >&2
  exit 1
fi
