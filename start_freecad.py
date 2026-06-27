#!/usr/bin/env python3
"""Run the launcher from the parent FreeCADModeling directory."""

from __future__ import annotations

import runpy
from pathlib import Path

_PARENT_LAUNCHER = Path(__file__).resolve().parent.parent / "start_freecad.py"

if not _PARENT_LAUNCHER.is_file():
    raise SystemExit(
        f"ERROR: launcher not found at {_PARENT_LAUNCHER}\n"
        "Expected start_freecad.py in the FreeCADModeling folder."
    )

runpy.run_path(str(_PARENT_LAUNCHER), run_name="__main__")
