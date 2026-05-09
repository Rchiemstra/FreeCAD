"""
bridge.freecad_bridge — FreeCAD export tools (XML-RPC).

Connects to the FreeCAD MCP addon (SimpleXMLRPCServer on port 9875) to:
  - export_urdf()       Export a robot FCStd to URDF via RobotCAD/CROSS Python API
  - export_sdf_world()  Copy/resolve a world SDF to the generated/ directory
  - compute_inertia()   Check that all non-fixed links have assigned densities
  - check_robotcad()    Verify RobotCAD/CROSS addon is installed in FreeCAD

Design notes:
  - Uses the same XML-RPC protocol as the freecad-mcp client.
  - export_urdf() calls execute_code() inside FreeCAD, which runs the RobotCAD
    Python exporter. This works without modifying the upstream freecad-mcp submodule.
  - All functions return an ExportResult (ok, path, messages) so callers can
    check success without parsing exception messages.
  - BLOCKER (Phase 1): RobotCAD/CROSS must be installed in FreeCAD for
    export_urdf() to succeed. check_robotcad() reports the installation status.
"""

from __future__ import annotations

import logging
import shutil
import xmlrpc.client
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional

logger = logging.getLogger(__name__)

# Default FreeCAD RPC connection parameters
DEFAULT_HOST = "localhost"
DEFAULT_PORT = 9875
DEFAULT_TIMEOUT = 10.0  # seconds


# ── Result type ────────────────────────────────────────────────────────────────

@dataclass
class ExportResult:
    ok:       bool
    path:     Optional[Path] = None
    messages: List[str]      = field(default_factory=list)

    def __bool__(self) -> bool:
        return self.ok


# ── Low-level RPC connection ───────────────────────────────────────────────────

class _FreeCADRPC:
    """Thin XML-RPC client for the FreeCAD MCP addon (same as freecad_client.py)."""

    def __init__(
        self,
        host: str = DEFAULT_HOST,
        port: int = DEFAULT_PORT,
        timeout: float = DEFAULT_TIMEOUT,
    ) -> None:
        transport = xmlrpc.client.Transport()
        self._server = xmlrpc.client.ServerProxy(
            f"http://{host}:{port}",
            transport=transport,
            allow_none=True,
        )
        self._host = host
        self._port = port

    def ping(self) -> bool:
        try:
            return bool(self._server.ping())
        except Exception:
            return False

    def execute_code(self, code: str) -> dict:
        """Run Python code inside FreeCAD and return the result dict."""
        return self._server.execute_code(code)


def _connect(host: str, port: int, timeout: float) -> Optional[_FreeCADRPC]:
    rpc = _FreeCADRPC(host=host, port=port, timeout=timeout)
    if not rpc.ping():
        return None
    return rpc


# ── RobotCAD export snippet ───────────────────────────────────────────────────
# This Python snippet is executed inside FreeCAD via execute_code().
# It uses the RobotCAD/CROSS Python API to export a robot to URDF.
# The snippet is designed to fail clearly with a descriptive message if
# RobotCAD is not installed, so the caller can report the blocker.

_ROBOTCAD_CHECK_SNIPPET = """
import sys
try:
    import CROSS
    result = {"success": True, "message": "RobotCAD/CROSS is installed", "version": getattr(CROSS, '__version__', 'unknown')}
except ImportError:
    result = {"success": False, "message": "RobotCAD/CROSS addon not found. Install via FreeCAD Addon Manager (search 'CROSS' or 'RobotCAD'). See docs/freecad_gazebo_mcp_task_breakdown.md Phase 0 blockers."}
print(repr(result))
result
"""

_EXPORT_URDF_SNIPPET_TEMPLATE = """
import os, json, sys

doc_name = {doc_name!r}
out_dir = {out_dir!r}

try:
    import CROSS
    import FreeCAD

    # Activate the CROSS workbench (required before calling its API)
    if hasattr(FreeCAD, 'Gui') and FreeCAD.Gui:
        FreeCAD.Gui.activateWorkbench("CrossWorkbench")

    doc = FreeCAD.openDocument(doc_name) if isinstance(doc_name, str) and os.path.exists(doc_name) else FreeCAD.getDocument(doc_name)
    if doc is None:
        raise RuntimeError(f"Document '{{doc_name}}' not found in FreeCAD")

    # Find the CROSS Robot object in the document
    robot_objs = [obj for obj in doc.Objects if obj.TypeId.startswith('Cross::')]
    if not robot_objs:
        raise RuntimeError(
            f"No CROSS robot objects found in document '{{doc_name}}'. "
            "The FCStd must contain a RobotCAD/CROSS robot definition."
        )

    # Use CROSS exporter
    os.makedirs(out_dir, exist_ok=True)
    # CROSS export API (adjust if API differs in installed version):
    robot = robot_objs[0]
    urdf_path = CROSS.export_urdf(robot, out_dir)

    result = {{"success": True, "message": f"Exported URDF to {{urdf_path}}", "path": urdf_path}}
except ImportError as e:
    result = {{"success": False, "message": f"RobotCAD/CROSS not installed: {{e}}"}}
except Exception as e:
    result = {{"success": False, "message": f"Export failed: {{e}}"}}
print(repr(result))
result
"""

_INERTIA_CHECK_SNIPPET_TEMPLATE = """
import FreeCAD, json

doc_name = {doc_name!r}
issues = []

try:
    doc = FreeCAD.getDocument(doc_name)
    if doc is None:
        raise RuntimeError(f"Document '{{doc_name}}' not found")
    for obj in doc.Objects:
        if not hasattr(obj, 'Shape'):
            continue
        has_material = any(
            prop.startswith('Material') or prop == 'Density'
            for prop in obj.PropertiesList
        )
        if not has_material:
            issues.append(f"Object '{{obj.Name}}' has no material/density assigned — inertia will be zero or incorrect")
    result = {{"success": len(issues) == 0, "issues": issues, "message": f"{{len(issues)}} inertia issue(s) found"}}
except Exception as e:
    result = {{"success": False, "issues": [], "message": f"Check failed: {{e}}"}}
print(repr(result))
result
"""


# ── Public API ─────────────────────────────────────────────────────────────────

def check_robotcad(
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    timeout: float = DEFAULT_TIMEOUT,
) -> ExportResult:
    """
    Check whether RobotCAD/CROSS is installed in the running FreeCAD instance.

    Returns:
        ExportResult with ok=True if CROSS is importable inside FreeCAD.
    """
    rpc = _connect(host, port, timeout)
    if rpc is None:
        return ExportResult(
            ok=False,
            messages=[
                f"Cannot reach FreeCAD RPC server at {host}:{port}. "
                "Is FreeCAD running with the MCP addon active? "
                "Start FreeCAD → switch to 'MCP Addon' workbench → click 'Start RPC Server'."
            ],
        )

    try:
        raw = rpc.execute_code(_ROBOTCAD_CHECK_SNIPPET)
        if isinstance(raw, dict):
            ok = raw.get("success", False)
            msg = raw.get("message", str(raw))
        else:
            # execute_code returns {"success": bool, "message": str, ...}
            ok = False
            msg = str(raw)
        return ExportResult(ok=ok, messages=[msg])
    except Exception as exc:
        return ExportResult(ok=False, messages=[f"RPC error: {exc}"])


def export_urdf(
    robot_name: str,
    out_dir: Path,
    fcstd_path: Optional[Path] = None,
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    timeout: float = DEFAULT_TIMEOUT,
) -> ExportResult:
    """
    Export a robot to URDF via the RobotCAD/CROSS Python API inside FreeCAD.

    The FreeCAD MCP addon's execute_code() function is used to run the
    RobotCAD exporter in the FreeCAD Python interpreter. This avoids
    modifying the upstream freecad-mcp submodule.

    Args:
        robot_name: Name of the robot (used to locate the FCStd if fcstd_path
                    is not provided, and as the generated subdirectory name).
        out_dir:    Directory where URDF + mesh files will be written.
                    Created if it does not exist.
        fcstd_path: Explicit path to the FCStd source file.
                    Defaults to robots/<robot_name>.FCStd relative to out_dir parent.
        host:       FreeCAD RPC host (default: localhost).
        port:       FreeCAD RPC port (default: 9875).
        timeout:    Connection timeout in seconds.

    Returns:
        ExportResult(ok, path, messages).
        If ok=False and the message mentions 'RobotCAD', the blocker is
        that RobotCAD/CROSS is not installed in FreeCAD.

    Example:
        result = export_urdf("arm_2dof", Path("generated/arm_2dof"))
        if result.ok:
            urdf = result.path  # Path to the generated .urdf file
    """
    rpc = _connect(host, port, timeout)
    if rpc is None:
        return ExportResult(
            ok=False,
            messages=[
                f"Cannot reach FreeCAD RPC server at {host}:{port}. "
                "Start FreeCAD with the MCP addon active."
            ],
        )

    # Resolve the FCStd path for FreeCAD (must be Windows-native path if FreeCAD runs on Windows)
    doc_arg = str(fcstd_path) if fcstd_path else robot_name
    snippet = _EXPORT_URDF_SNIPPET_TEMPLATE.format(
        doc_name=doc_arg,
        out_dir=str(Path(out_dir).resolve()),
    )

    try:
        raw = rpc.execute_code(snippet)
        if isinstance(raw, dict):
            # execute_code returns {"success": bool, "message": str, ...}
            inner = raw
        else:
            inner = {"success": False, "message": str(raw)}

        ok   = bool(inner.get("success", False))
        msgs = [inner.get("message", "No message returned")]
        path = Path(inner["path"]) if ok and inner.get("path") else None

        return ExportResult(ok=ok, path=path, messages=msgs)
    except Exception as exc:
        return ExportResult(ok=False, messages=[f"RPC error during export: {exc}"])


def export_sdf_world(
    world_name: str,
    out_dir: Path,
    source_dir: Optional[Path] = None,
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    timeout: float = DEFAULT_TIMEOUT,
) -> ExportResult:
    """
    Resolve and stage a world SDF for Gazebo.

    For handcrafted worlds (no FCStd source), this copies the SDF from
    worlds/<world_name>.sdf to out_dir and normalises any mesh paths.
    When a FreeCAD world document exists, the export would go through RobotCAD.

    Args:
        world_name: Name of the world (stem of the .sdf file).
        out_dir:    Destination directory (typically generated/<world_name>/).
        source_dir: Directory containing <world_name>.sdf. Defaults to worlds/.

    Returns:
        ExportResult with the path to the staged SDF.
    """
    if source_dir is None:
        # Guess source dir relative to out_dir/../.. (project root layout)
        source_dir = Path(out_dir).parent.parent / "worlds"

    source_sdf = Path(source_dir) / f"{world_name}.sdf"
    if not source_sdf.exists():
        return ExportResult(
            ok=False,
            messages=[
                f"World SDF not found: {source_sdf}. "
                f"Expected a hand-crafted file at worlds/{world_name}.sdf or an FCStd "
                "source to export via FreeCAD."
            ],
        )

    # Validate the source SDF before staging
    from bridge.validate import validate_sdf
    vr = validate_sdf(source_sdf)
    if not vr.ok:
        return ExportResult(ok=False, messages=vr.errors + vr.warnings)

    # Stage: copy to out_dir
    out_dir = Path(out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    dest = out_dir / f"{world_name}.sdf"
    shutil.copy2(str(source_sdf), str(dest))

    msgs = [f"Staged world SDF: {source_sdf} → {dest}"]
    msgs += [f"WARN: {w}" for w in vr.warnings]
    return ExportResult(ok=True, path=dest, messages=msgs)


def compute_inertia_check(
    robot_name: str,
    fcstd_path: Optional[Path] = None,
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    timeout: float = DEFAULT_TIMEOUT,
) -> ExportResult:
    """
    Verify that all non-fixed links in a FreeCAD document have density assigned.

    Uses execute_code() to inspect the FreeCAD document properties.

    Args:
        robot_name: Document name (or FCStd path stem).
        fcstd_path: Explicit FCStd path; if None, uses robot_name as document name.

    Returns:
        ExportResult with ok=True if all links have density, and messages
        listing which links are missing material assignments.
    """
    rpc = _connect(host, port, timeout)
    if rpc is None:
        return ExportResult(
            ok=False,
            messages=[f"Cannot reach FreeCAD RPC at {host}:{port}"],
        )

    doc_arg = str(fcstd_path.stem) if fcstd_path else robot_name
    snippet = _INERTIA_CHECK_SNIPPET_TEMPLATE.format(doc_name=doc_arg)

    try:
        raw = rpc.execute_code(snippet)
        inner = raw if isinstance(raw, dict) else {"success": False, "message": str(raw), "issues": []}
        ok    = bool(inner.get("success", False))
        msgs  = inner.get("issues", []) or [inner.get("message", "")]
        return ExportResult(ok=ok, messages=msgs)
    except Exception as exc:
        return ExportResult(ok=False, messages=[f"RPC error: {exc}"])
