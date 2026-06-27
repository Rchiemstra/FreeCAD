# SPDX-License-Identifier: LGPL-2.1-or-later
"""
Build robots/arm_2dof.FCStd from the checked-in placeholder URDF.

Creates a Cross::Robot document via robot_from_urdf_path, then saves FCStd.
Requires RobotCAD/CROSS (freecad.overcross) and urdf_parser_py dependencies.

Usage::

    FreeCADCmd scripts/build_arm_2dof_fcstd.py
"""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
URDF = ROOT / "robots" / "arm_2dof.urdf"
FCSTD = ROOT / "robots" / "arm_2dof.FCStd"


def _ensure_freecad_gui_stub() -> None:
    """robot_from_urdf imports FreeCADGui at module level; stub for FreeCADCmd."""
    import sys
    import types

    if "FreeCADGui" in sys.modules:
        return

    class _Bar:
        def setValue(self, *_args, **_kwargs):
            pass

        def show(self, *_args, **_kwargs):
            pass

        def hide(self, *_args, **_kwargs):
            pass

    class _StatusBar:
        def setStyleSheet(self, *_args, **_kwargs):
            pass

        def addWidget(self, *_args, **_kwargs):
            pass

    class _MainWindow:
        def statusBar(self):
            return _StatusBar()

    gui = types.ModuleType("FreeCADGui")
    gui.Selection = types.SimpleNamespace(getSelection=lambda: [])
    gui.getMainWindow = lambda: _MainWindow()
    gui.ActiveDocument = None
    gui.doCommand = lambda *_a, **_k: None
    gui.addModule = lambda *_a, **_k: None
    sys.modules["FreeCADGui"] = gui

    # Avoid real Qt widgets during URDF import progress UI.
    import freecad.cross.freecadgui_utils as fg

    fg.get_progress_bar = lambda *a, **k: _Bar()


def main() -> int:
    if not URDF.is_file():
        _fail(f"Missing URDF: {URDF}")
        return 2

    try:
        import FreeCAD
        _ensure_freecad_gui_stub()
        from freecad.cross.robot_from_urdf import robot_from_urdf_path
    except ImportError as exc:
        _fail(f"RobotCAD/CROSS or FreeCAD unavailable: {exc}")
        return 1

    doc = FreeCAD.newDocument("arm_2dof_build")
    try:
        import FreeCADGui

        FreeCADGui.ActiveDocument = doc
        robot_from_urdf_path(
            doc,
            str(URDF.resolve()),
            package_path=str(URDF.parent),
            create_without_solids=True,
        )
        doc.recompute()
        FCSTD.parent.mkdir(parents=True, exist_ok=True)
        doc.saveAs(str(FCSTD.resolve()))
    except Exception as exc:
        _fail(f"Failed to build FCStd: {exc}")
        return 1

    robots = [o for o in doc.Objects if getattr(o, "TypeId", "") == "Cross::Robot"]
    _ok(f"Saved {FCSTD} ({len(robots)} Cross::Robot object(s))")
    return 0


def _ok(msg: str) -> None:
    print(msg, flush=True)
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()


def _fail(msg: str) -> None:
    print(msg, flush=True)
    sys.stderr.write(msg + "\n")
    sys.stderr.flush()


if __name__ == "__main__":
    sys.exit(main())
