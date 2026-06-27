# SPDX-License-Identifier: LGPL-2.1-or-later
"""
Verify RobotCAD/CROSS (freecad.overcross) inside FreeCAD.

Run with FreeCADCmd (headless)::

    FreeCADCmd scripts/verify_robotcad_cross.py

Or from repo root on Windows (pixi build)::

    .pixi\\envs\\default\\Library\\bin\\FreeCADCmd.exe scripts\\verify_robotcad_cross.py
"""
from __future__ import annotations

import sys


def main() -> int:
    try:
        import FreeCAD  # noqa: F401
    except ImportError as exc:
        print("FreeCAD Python API missing:", exc)
        return 1

    try:
        import freecad.cross as cross  # noqa: F401
    except ImportError as exc:
        print("RobotCAD/CROSS (freecad.cross) import failed:", exc)
        print("Install: .\\scripts\\install_robotcad_cross.ps1 then restart FreeCAD")
        return 1

    version = getattr(cross, "__version__", "unknown")
    msg = f"RobotCAD/CROSS OK (freecad.cross version={version})"
    print(msg, flush=True)
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()
    return 0


if __name__ == "__main__":
    sys.exit(main())
