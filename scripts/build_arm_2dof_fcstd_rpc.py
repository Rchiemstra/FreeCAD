# SPDX-License-Identifier: LGPL-2.1-or-later
"""
Build robots/arm_2dof.FCStd via FreeCAD XML-RPC (GUI FreeCAD required).

Uses the same import path fix as bridge/freecad_bridge.py (site-packages
``freecad`` shadows overcross).

Usage::

    # FreeCAD running with MCP addon RPC on port 9875
    python scripts/build_arm_2dof_fcstd_rpc.py
"""
from __future__ import annotations

import sys
import textwrap
import xmlrpc.client
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
URDF = ROOT / "robots" / "arm_2dof.urdf"
FCSTD = ROOT / "robots" / "arm_2dof.FCStd"
RPC = "http://localhost:9875"


def main() -> int:
    if not URDF.is_file():
        print("Missing", URDF)
        return 2

    snippet = textwrap.dedent(
        f"""
        import os, sys
        from pathlib import Path
        _mod = os.path.join(os.environ.get('APPDATA', ''), 'FreeCAD', 'v1-2', 'Mod', 'freecad.overcross')

        def _purge_cached_freecad():
            for _name in list(sys.modules.keys()):
                if _name == 'freecad' or _name.startswith('freecad.'):
                    del sys.modules[_name]
            _g = globals()
            for _name in list(_g.keys()):
                if _name == 'freecad' or (isinstance(_name, str) and _name.startswith('freecad.')):
                    del _g[_name]

        def _ensure_freecad_utils_path():
            for _entry in sys.path:
                if os.path.isfile(os.path.join(_entry, 'freecad', 'utils.py')):
                    return
            for _entry in list(sys.path):
                if '.pixi' not in _entry.replace('\\\\', '/') or 'Library' not in _entry:
                    continue
                _site = os.path.normpath(os.path.join(_entry, '..', '..', 'Lib', 'site-packages'))
                if os.path.isfile(os.path.join(_site, 'freecad', 'utils.py')) and _site not in sys.path:
                    sys.path.insert(1, _site)
                    return

        _purge_cached_freecad()
        if os.path.isdir(_mod) and _mod not in sys.path:
            sys.path.insert(0, _mod)
        _ensure_freecad_utils_path()

        import FreeCAD
        from freecad.cross.robot_from_urdf import robot_from_urdf_path

        urdf = r'{URDF.resolve()}'
        fcstd = r'{FCSTD.resolve()}'
        doc = FreeCAD.newDocument('arm_2dof_build')
        robot_from_urdf_path(doc, urdf, package_path=str(Path(urdf).parent), create_without_solids=True)
        doc.recompute()
        Path(fcstd).parent.mkdir(parents=True, exist_ok=True)
        doc.saveAs(fcstd)
        robots = [o for o in doc.Objects if getattr(o, 'TypeId', '') == 'Cross::Robot']
        result = {{'success': True, 'message': f'Saved {{fcstd}}', 'robots': len(robots)}}
        print(repr(result))
        result
        """
    )

    try:
        proxy = xmlrpc.client.ServerProxy(RPC, allow_none=True)
        if not proxy.ping():
            print("FreeCAD RPC not responding on", RPC)
            return 1
        raw = proxy.execute_code(snippet)
    except Exception as exc:
        print("RPC failed:", exc)
        return 1

    import ast

    def _interpret(raw_dict):
        if not raw_dict.get("success"):
            return {"success": False, "message": raw_dict.get("error", "execute_code failed")}
        msg = raw_dict.get("message", "")
        out = msg.split("Output:", 1)[-1].strip() if "Output:" in msg else ""
        for line in reversed([ln.strip() for ln in out.splitlines() if ln.strip()]):
            try:
                parsed = ast.literal_eval(line)
                if isinstance(parsed, dict) and "success" in parsed:
                    return parsed
            except (ValueError, SyntaxError):
                continue
        return {"success": False, "message": out or "no structured output"}

    inner = _interpret(raw)
    print(inner.get("message", inner))
    if not inner.get("success"):
        return 1
    if FCSTD.is_file():
        print("OK:", FCSTD, "size", FCSTD.stat().st_size)
        return 0
    print("RPC reported success but file missing:", FCSTD)
    return 1


if __name__ == "__main__":
    sys.exit(main())
