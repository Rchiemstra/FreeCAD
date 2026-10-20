# SPDX-License-Identifier: LGPL-2.1-or-later
"""Shared command-line diff entry point (also run after GUI startup)."""

import FreeCAD
from modeldiff import run_cli

try:
    run_cli(FreeCAD.ConfigGet("OpenFile0"), FreeCAD.ConfigGet("OpenFile1"))
except Exception as exc:
    FreeCAD.Console.PrintError(f"Model diff failed: {exc}\n")
    if FreeCAD.GuiUp:
        from PySide import QtCore

        QtCore.QCoreApplication.exit(1)
    else:
        raise SystemExit(1) from exc
