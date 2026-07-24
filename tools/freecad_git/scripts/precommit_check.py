#!/usr/bin/env python3
"""Pre-commit entry point that installs the local freecad_git package on sys.path."""

from __future__ import annotations

import sys
from pathlib import Path

_SRC = Path(__file__).resolve().parents[1] / "src"
if str(_SRC) not in sys.path:
    sys.path.insert(0, str(_SRC))

from freecad_git.cli import main  # noqa: E402

if __name__ == "__main__":
    raise SystemExit(main())
