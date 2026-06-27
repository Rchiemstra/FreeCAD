# SPDX-License-Identifier: LGPL-2.1-or-later
"""
Export ``robots/arm_2dof.FCStd`` to URDF via FreeCADCmd (batch, no MCP).

Usage::

    .\\.pixi\\envs\\default\\Library\\bin\\FreeCADCmd.exe scripts\\export_arm_2dof_fcstd.py
    .\\scripts\\run_freecad_script.ps1 .\\scripts\\export_arm_2dof_fcstd.py

Output::

    generated/arm_2dof/arm_2dof_description/arm_2dof_description/urdf/*.urdf
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
FCSTD = ROOT / "robots" / "arm_2dof.FCStd"
OUT = ROOT / "generated" / "arm_2dof"


def main() -> int:
    if not FCSTD.is_file():
        print(f"Missing {FCSTD} — run scripts/build_arm_2dof_fcstd.py first")
        return 2

    sys.path.insert(0, str(Path(__file__).resolve().parent))
    from robotcad_headless import export_fcstd_to_urdf

    try:
        urdf_path, _robot = export_fcstd_to_urdf(FCSTD, OUT, robot_name="arm_2dof")
    except Exception as exc:
        print(f"Export failed: {exc}", file=sys.stderr)
        return 1

    if not urdf_path.is_file():
        print(f"Export reported success but URDF missing: {urdf_path}", file=sys.stderr)
        return 1

    print(f"URDF_EXPORT_PATH: {urdf_path}")
    print(f"Exported URDF: {urdf_path}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
