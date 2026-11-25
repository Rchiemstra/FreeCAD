#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Capture the configured interpreter's binding origins in isolated mode."""
import importlib
import json
from pathlib import Path
import sys

names = ("PySide6", "PySide6.QtCore", "PySide6.QtGui", "PySide6.QtWidgets", "shiboken6", "shiboken6.Shiboken")
origins = {name: str(Path(importlib.import_module(name).__file__).resolve(strict=True)) for name in names}
Path(sys.argv[1]).write_text(json.dumps(origins), encoding="utf-8")
