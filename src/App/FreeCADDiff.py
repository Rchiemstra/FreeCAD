# SPDX-License-Identifier: LGPL-2.1-or-later
"""Shared command-line diff entry point (also run after GUI startup)."""

import FreeCAD
from modeldiff import run_cli

run_cli(FreeCAD.ConfigGet("OpenFile0"), FreeCAD.ConfigGet("OpenFile1"))
