# SPDX-License-Identifier: LGPL-2.1-or-later
"""
FreeCADCmd batch export via RobotCAD — requires a document containing Cross::* robots.

Usage::
    FreeCADCmd export_robotcad_fcstd.py <path_to.FCStd> <output_dir>
"""
from __future__ import annotations

import os
import sys


def main() -> int:
    if len(sys.argv) < 3:
        print("Usage: export_robotcad_fcstd.py <doc.FCStd> <out_dir>")
        return 2

    fcstd = sys.argv[1]
    out_dir = sys.argv[2]

    import FreeCAD

    try:
        from freecad import robotcad as CROSS  # type: ignore
    except ImportError:
        import robotcad as CROSS  # type: ignore

    if hasattr(FreeCAD, "Gui") and FreeCAD.GuiUp:
        FreeCAD.Gui.activateWorkbench("CrossWorkbench")

    doc = FreeCAD.openDocument(fcstd)
    robot_objs = [o for o in doc.Objects if getattr(o, "TypeId", "").startswith("Cross::")]
    if not robot_objs:
        print("No Cross:: robot objects in document")
        return 1

    os.makedirs(out_dir, exist_ok=True)
    robot = robot_objs[0]

    export_fn = getattr(CROSS, "export_urdf", None)
    if export_fn is None:
        gen = getattr(CROSS, "export", None)
        if callable(gen):
            export_fn = gen
    if export_fn is None:
        print("RobotCAD API has no export_urdf/export — upgrade RobotCAD or extend script")
        return 1

    path = export_fn(robot, out_dir)
    print("Exported:", path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
