#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later

"""Public entrypoint: start FreeCAD and bring up the MCP RPC server.

The implementation lives in ``tools/launcher/start_freecad_impl.py`` and is
tracked in this repository. It used to live in the parent FreeCADModeling
directory, which is not version controlled, so launcher changes could not be
reviewed, tested in CI, or recovered.

Examples:

  python start_freecad.py
  python start_freecad.py model.FCStd
  python start_freecad.py --freecad build/release/bin/FreeCAD.exe
  python start_freecad.py --no-wait-for-mcp
"""

from __future__ import annotations

import sys
from pathlib import Path

_IMPL_DIR = Path(__file__).resolve().parent / "tools" / "launcher"
_IMPL = _IMPL_DIR / "start_freecad_impl.py"

if not _IMPL.is_file():
    raise SystemExit(f"ERROR: launcher implementation not found at {_IMPL}")

if str(_IMPL_DIR) not in sys.path:
    sys.path.insert(0, str(_IMPL_DIR))

import start_freecad_impl  # noqa: E402  (path must be set up first)

# Re-export so `import start_freecad` keeps working as a module API.
main = start_freecad_impl.main
JsonRpcClient = start_freecad_impl.JsonRpcClient
JsonRpcError = start_freecad_impl.JsonRpcError
JsonRpcTransportError = start_freecad_impl.JsonRpcTransportError
MCP_RPC_PORT = start_freecad_impl.MCP_RPC_PORT
REUSE_DISABLED_REASON = start_freecad_impl.REUSE_DISABLED_REASON

if __name__ == "__main__":
    raise SystemExit(start_freecad_impl.main())
