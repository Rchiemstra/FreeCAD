# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run inside FreeCAD via: ``xvfb-run -a FreeCADCmd check_robotcad_freecad.py``."""
from __future__ import annotations

import sys


def main() -> int:
    try:
        import FreeCAD  # noqa: F401
    except ImportError as exc:
        print("FreeCAD Python API missing:", exc)
        return 1

    try:
        from freecad import robotcad as rc  # type: ignore
    except ImportError:
        try:
            import robotcad as rc  # type: ignore
        except ImportError as exc:
            print("RobotCAD/CROSS import failed:", exc)
            return 1

    ver = getattr(rc, "__version__", "unknown")
    print(f"RobotCAD import OK (version={ver})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
