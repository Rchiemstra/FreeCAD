"""
Request FreeCAD's SimWorkbench document mode from outside FreeCAD.

Examples:
  python scripts/set_freecad_document_mode.py read --path robots/arm_2dof.FCStd
  python scripts/set_freecad_document_mode.py write
"""
from __future__ import annotations

import argparse
import os
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
ADDON_DIR = ROOT / "addons" / "SimWorkbench"
if str(ADDON_DIR) not in sys.path:
    sys.path.insert(0, str(ADDON_DIR))

from read_mode import MODE_READ, MODE_WRITE, write_mode_request  # noqa: E402


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description="Request FreeCAD read/write document mode.")
    parser.add_argument("mode", choices=[MODE_READ, MODE_WRITE])
    parser.add_argument(
        "--path",
        help="Saved .FCStd file to watch when requesting read mode. Defaults to FreeCAD's active document.",
    )
    args = parser.parse_args(argv)

    path = os.path.abspath(args.path) if args.path else None
    control_file = write_mode_request(args.mode, path=path, source="agent")
    print("Requested {} mode via {}".format(args.mode, control_file))
    if path:
        print("Path: {}".format(path))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
