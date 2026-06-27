#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Verify robots/arm_2dof.FCStd matches config/runtime-versions.lock.yaml."""
from __future__ import annotations

import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from bridge.runtime_versions import verify_fcstd  # noqa: E402


def main() -> int:
    ok, msg = verify_fcstd(ROOT)
    if ok:
        print(f"[e2e] robot source: {msg}")
        return 0
    print(f"ERROR: robot source: {msg}", file=sys.stderr)
    return 1


if __name__ == "__main__":
    raise SystemExit(main())
