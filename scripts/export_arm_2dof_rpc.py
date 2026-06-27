# SPDX-License-Identifier: LGPL-2.1-or-later
"""Export arm_2dof — prefers FreeCADCmd batch; optional MCP fallback."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from bridge.freecad_bridge import export_urdf, export_urdf_cmd


def main() -> int:
    fcstd = ROOT / "robots" / "arm_2dof.FCStd"
    out = ROOT / "generated" / "arm_2dof"
    if not fcstd.is_file():
        print("Missing FCStd — run scripts/build_arm_2dof_fcstd_rpc.py first")
        return 2

    result = export_urdf_cmd("arm_2dof", out, fcstd_path=fcstd)
    if not result.ok:
        result = export_urdf(
            "arm_2dof", out, fcstd_path=fcstd, prefer_cmd=False, timeout=300
        )
    for msg in result.messages:
        print(msg)
    return 0 if result.ok else 1


if __name__ == "__main__":
    sys.exit(main())
