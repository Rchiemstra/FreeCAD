# SPDX-License-Identifier: LGPL-2.1-or-later
"""
FreeCADCmd batch export via RobotCAD — requires a document containing Cross::Robot.

Usage::

    FreeCADCmd export_robotcad_fcstd.py <path_to.FCStd> <output_dir>

Delegates to ``scripts/robotcad_headless.py`` (same path as Windows batch export).
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "scripts"))

from robotcad_headless import export_fcstd_to_urdf  # noqa: E402


def main() -> int:
    if len(sys.argv) < 3:
        print("Usage: export_robotcad_fcstd.py <doc.FCStd> <out_dir>")
        return 2

    fcstd = Path(sys.argv[1]).resolve()
    out_dir = Path(sys.argv[2]).resolve()

    if not fcstd.is_file():
        print(f"FCStd not found: {fcstd}")
        return 2

    try:
        robot_name = "arm_2dof" if fcstd.stem == "arm_2dof" else fcstd.stem
        urdf_path, _robot = export_fcstd_to_urdf(fcstd, out_dir, robot_name=robot_name)
    except Exception as exc:
        print(f"Export failed: {exc}", file=sys.stderr)
        return 1

    if not urdf_path.is_file():
        print(f"URDF missing after export: {urdf_path}", file=sys.stderr)
        return 1

    print("Exported:", urdf_path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
