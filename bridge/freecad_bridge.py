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

import ast
import logging
import os
import shutil
import subprocess
import sys
import xmlrpc.client
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, List, Optional

logger = logging.getLogger(__name__)


def _record_export(kind: str, path: Optional[Path], **fields: Any) -> None:
    from bridge.run_context import record_event, record_path

    if path is not None:
        record_path(kind, path)
    record_event("export", f"{kind}_complete", path=str(path) if path else None, **fields)

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


def _extract_execute_code_output(raw: dict) -> str:
    """Return stdout captured by FreeCAD ``execute_code`` (after ``Output:``)."""
    msg = raw.get("message", "")
    if not isinstance(msg, str):
        return ""
    marker = "Output:"
    if marker in msg:
        return msg.split(marker, 1)[1].strip()
    return msg.strip()


def _parse_inner_result_from_output(output: str) -> Optional[dict]:
    """
    Parse the ``repr(dict)`` printed by in-FreeCAD snippets.

    Snippets end with ``print(repr(result))``; that line is what we parse.
    """
    if not output:
        return None

    lines = [ln.strip() for ln in output.strip().splitlines() if ln.strip()]
    candidates = list(reversed(lines)) if lines else [output.strip()]

    for candidate in candidates:
        try:
            parsed = ast.literal_eval(candidate)
        except (ValueError, SyntaxError):
            continue
        if isinstance(parsed, dict) and "success" in parsed:
            return parsed

    try:
        parsed = ast.literal_eval(output.strip())
    except (ValueError, SyntaxError):
        return None
    return parsed if isinstance(parsed, dict) and "success" in parsed else None


def _interpret_execute_code(raw) -> dict:
    """
    Map FreeCAD RPC ``execute_code`` response to the snippet's inner result dict.

    The RPC layer reports ``success=True`` when Python ran without exception.
    RobotCAD export/check snippets print a separate ``{success, message, ...}``
    dict to stdout — that inner dict drives ``ExportResult.ok``.
    """
    if not isinstance(raw, dict):
        return {"success": False, "message": str(raw)}

    if not raw.get("success", False):
        err = raw.get("error", raw.get("message", "execute_code failed"))
        return {"success": False, "message": str(err)}

    inner = _parse_inner_result_from_output(_extract_execute_code_output(raw))
    if inner is not None:
        return inner

    output = _extract_execute_code_output(raw)
    return {
        "success": False,
        "message": (
            "execute_code completed but no structured result in output"
            + (f": {output[:300]}" if output else "")
        ),
    }


# ── RobotCAD export snippet ───────────────────────────────────────────────────
# This Python snippet is executed inside FreeCAD via execute_code().
# It uses the RobotCAD/CROSS Python API to export a robot to URDF.
# The snippet is designed to fail clearly with a descriptive message if
# RobotCAD is not installed, so the caller can report the blocker.

_ROBOTCAD_MOD_SUBDIR = "freecad.overcross"


def _robotcad_mod_path_snippet() -> str:
    """
    Python preamble: merge OVERCROSS ``freecad.cross`` with pixi ``freecad.utils``.

    GUI FreeCAD often lacks pixi ``Lib/site-packages`` on ``sys.path``; OVERCROSS
    needs ``freecad.utils.get_python_exe``. ``execute_code`` reuses ``globals()``.
    """
    return f"""
import os, sys
_mod = os.path.join(os.environ.get('APPDATA', ''), 'FreeCAD', 'v1-2', 'Mod', '{_ROBOTCAD_MOD_SUBDIR}')

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
        _norm = _entry.replace('\\\\', '/')
        if '.pixi' not in _norm or 'Library' not in _entry:
            continue
        _site = os.path.normpath(os.path.join(_entry, '..', '..', 'Lib', 'site-packages'))
        if os.path.isfile(os.path.join(_site, 'freecad', 'utils.py')) and _site not in sys.path:
            sys.path.insert(1, _site)
            return

_purge_cached_freecad()
if os.path.isdir(_mod) and _mod not in sys.path:
    sys.path.insert(0, _mod)
_ensure_freecad_utils_path()
"""


_ROBOTCAD_CHECK_SNIPPET = (
    _robotcad_mod_path_snippet()
    + """
try:
    import freecad.cross as cross  # noqa: F401
    result = {"success": True, "message": "RobotCAD/CROSS (freecad.cross) is installed"}
except ImportError as e:
    result = {"success": False, "message": f"RobotCAD/CROSS addon not found: {e}. Run scripts/install_robotcad_cross.ps1 and restart FreeCAD."}
except Exception as e:
    result = {"success": False, "message": f"RobotCAD/CROSS check failed: {e}"}
print(repr(result))
result
"""
)

_EXPORT_URDF_SNIPPET_TEMPLATE = (
    _robotcad_mod_path_snippet()
    + """
import os
from pathlib import Path

doc_name = {doc_name!r}
out_dir = {out_dir!r}
pkg_name = {pkg_name!r}

try:
    import FreeCAD
    _purge_cached_freecad()
    class _AutoOverwriteDialog:
        def __init__(self, _output_path, write_files):
            self._write_files = list(write_files)

        def exec_(self):
            return [], self._write_files, []

        def close(self):
            pass

    def _install_auto_overwrite_dialog():
        for _m in list(sys.modules.values()):
            if _m is None:
                continue
            if hasattr(_m, 'FileOverwriteConfirmationDialog'):
                setattr(_m, 'FileOverwriteConfirmationDialog', _AutoOverwriteDialog)
        try:
            import freecad.cross.ui.file_overwrite_confirmation_dialog as _fod_mod
            _fod_mod.FileOverwriteConfirmationDialog = _AutoOverwriteDialog
        except Exception:
            pass
        try:
            import freecad.cross.robot_proxy as _rp_mod
            _rp_mod.FileOverwriteConfirmationDialog = _AutoOverwriteDialog
        except Exception:
            pass

    _install_auto_overwrite_dialog()

    import freecad.cross.wb_utils as _wb_utils
    import freecad.cross.robot_proxy as _robot_proxy_mod

    _overcross_mod = Path(os.environ.get('APPDATA', '')) / 'FreeCAD' / 'v1-2' / 'Mod' / 'freecad.overcross'
    if _overcross_mod.is_dir():
        _wb_utils.MOD_PATH = _overcross_mod
        _wb_utils.RESOURCES_PATH = _overcross_mod / 'resources'
        _wb_utils.UI_PATH = _wb_utils.RESOURCES_PATH / 'ui'
        _wb_utils.ICON_PATH = _wb_utils.RESOURCES_PATH / 'icons'
        _wb_utils.MODULES_PATH = _overcross_mod / 'modules'
        _wb_utils.ROS2_CONTROLLERS_PATH = _wb_utils.MODULES_PATH / 'ros2_controllers'
        _wb_utils.SDFORMAT_PATH = _wb_utils.MODULES_PATH / 'sdformat'
        _wb_utils.SDFORMAT_SDF_TEMPLATES_PATH = _wb_utils.SDFORMAT_PATH / 'sdf'
        _wb_utils.ROBOT_DESCRIPTIONS_REPO_PATH = _wb_utils.MODULES_PATH / 'robot_descriptions'
        _wb_utils.ROBOT_DESCRIPTIONS_MODULE_PATH = _wb_utils.ROBOT_DESCRIPTIONS_REPO_PATH / 'robot_descriptions'
        _wb_utils.DYNAMIC_WORLD_GENERATOR_REPO_PATH = _wb_utils.MODULES_PATH / 'Dynamic_World_Generator'
        _wb_utils.DYNAMIC_WORLD_GENERATOR_MODULE_PATH = _wb_utils.DYNAMIC_WORLD_GENERATOR_REPO_PATH / 'code'
        _wb_utils.DYNAMIC_WORLD_GENERATOR_WORLDS_GAZEBO_PATH = (
            _wb_utils.DYNAMIC_WORLD_GENERATOR_REPO_PATH / 'worlds' / 'gazebo'
        )
        for _name in (
            'MOD_PATH', 'MODULES_PATH', 'ROS2_CONTROLLERS_PATH', 'SDFORMAT_PATH',
            'ROBOT_DESCRIPTIONS_REPO_PATH', 'DYNAMIC_WORLD_GENERATOR_REPO_PATH',
        ):
            setattr(_robot_proxy_mod, _name, getattr(_wb_utils, _name))

    _orig_git_init_submodules = _wb_utils.git_init_submodules

    def _git_init_submodules_skip_if_ready(*_args, **_kwargs):
        _path = _kwargs.get('submodule_repo_path', _wb_utils.ROS2_CONTROLLERS_PATH)
        _path = Path(_path)
        if _path.is_dir() and any(_path.iterdir()):
            return

    _wb_utils.git_init_submodules = _git_init_submodules_skip_if_ready
    _robot_proxy_mod.git_init_submodules = _git_init_submodules_skip_if_ready
    for _mod in sys.modules.values():
        if _mod is None:
            continue
        if getattr(_mod, 'git_init_submodules', None) is _orig_git_init_submodules:
            setattr(_mod, 'git_init_submodules', _git_init_submodules_skip_if_ready)

    def _noop_progress_bar(*_a, **_k):
        class _Bar:
            def show(self):
                pass

            def setValue(self, _v):
                pass

            def close(self):
                pass

        return _Bar()

    _wb_utils.get_progress_bar = _noop_progress_bar

    from freecad.cross.ros.utils import split_package_path
    from freecad.cross.wb_utils import get_urdf_path, is_robot

    path = Path(doc_name)
    doc = None
    if path.suffix.lower() == '.fcstd':
        if path.exists():
            try:
                doc = FreeCAD.openDocument(path.as_posix())
            except Exception:
                doc = None
        if doc is None:
            for name in FreeCAD.listDocuments():
                candidate = FreeCAD.getDocument(name)
                if candidate and getattr(candidate, 'FileName', '') and path.as_posix() in candidate.FileName.replace('\\\\', '/'):
                    doc = candidate
                    break
    else:
        doc = FreeCAD.getDocument(doc_name)
    if doc is None:
        raise RuntimeError(f"Document not found: {{doc_name}}")

    robot_objs = [o for o in doc.Objects if is_robot(o)]
    if not robot_objs:
        raise RuntimeError(
            "No Cross::Robot in document. Build robots/arm_2dof.FCStd "
            "(scripts/build_arm_2dof_fcstd_rpc.py) or model the robot in CROSS."
        )

    robot = robot_objs[0]
    gen_root = Path(out_dir).resolve()
    output_path = (gen_root / pkg_name).resolve()
    os.makedirs(output_path, exist_ok=True)

    def _get_rel_and_abs_path(path: str, ask_user_fill_workspace: bool = True):
        p = Path(path)
        if p.is_absolute():
            return str(p), p
        return str(p), p.resolve()

    _wb_utils.get_rel_and_abs_path = _get_rel_and_abs_path
    _robot_proxy_mod.get_rel_and_abs_path = _get_rel_and_abs_path
    try:
        import freecad.cross.wb_gui_utils as _wb_gui
        _wb_gui.get_ros_workspace = lambda _old='': gen_root
    except Exception:
        pass
    robot.OutputPath = str(output_path)
    doc.recompute()

    if not hasattr(robot, 'Proxy') or robot.Proxy is None:
        raise RuntimeError("Cross::Robot has no Proxy")

    _install_auto_overwrite_dialog()

    import types

    def _noop_git_init_submodules(*_args, **_kwargs):
        return

    def _rpc_export_urdf(self, interactive=True):
        import inspect

        _live_mod = inspect.getmodule(self) or inspect.getmodule(self.export_urdf)
        if _live_mod is not None:
            _live_mod.git_init_submodules = _noop_git_init_submodules
            _live_mod.get_rel_and_abs_path = _get_rel_and_abs_path
            _live_mod.FileOverwriteConfirmationDialog = _AutoOverwriteDialog
            if hasattr(_live_mod, 'get_progress_bar'):
                _live_mod.get_progress_bar = _noop_progress_bar
        for _method_name in ('export_urdf', 'get_robot_controllers_yaml'):
            _bound = getattr(self, _method_name)
            _method_fn = _bound.__func__ if hasattr(_bound, '__func__') else _bound
            _g = _method_fn.__globals__
            _g['FileOverwriteConfirmationDialog'] = _AutoOverwriteDialog
            _g['git_init_submodules'] = _noop_git_init_submodules
            _g['get_rel_and_abs_path'] = _get_rel_and_abs_path
        _fn = _bound.__func__ if hasattr(self.export_urdf, '__func__') else self.export_urdf
        return _fn(self, interactive)

    robot.Proxy.export_urdf = types.MethodType(_rpc_export_urdf, robot.Proxy)
    robot.Proxy.export_urdf(interactive=True)
    _, _, description_package_path = split_package_path(output_path)
    urdf_path = get_urdf_path(robot, description_package_path)
    result = {{"success": True, "message": f"Exported URDF to {{urdf_path}}", "path": str(urdf_path)}}
except ImportError as e:
    result = {{"success": False, "message": f"RobotCAD/CROSS not installed: {{e}}"}}
except Exception as e:
    import traceback
    result = {{"success": False, "message": f"Export failed: {{e}}", "trace": traceback.format_exc()[-800:]}}
print(repr(result))
result
"""
)

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
        inner = _interpret_execute_code(raw)
        ok = bool(inner.get("success", False))
        msg = inner.get("message", str(inner))
        return ExportResult(ok=ok, messages=[msg])
    except Exception as exc:
        return ExportResult(ok=False, messages=[f"RPC error: {exc}"])


def resolve_freecad_cmd() -> Optional[Path]:
    """Return FreeCADCmd executable (pixi build or ``FREECAD_CMD`` env)."""
    env = os.environ.get("FREECAD_CMD")
    if env:
        path = Path(env)
        if path.is_file():
            return path

    repo = Path(__file__).resolve().parents[1]
    for candidate in (
        repo / ".pixi" / "envs" / "default" / "Library" / "bin" / "FreeCADCmd.exe",
        repo / ".pixi" / "envs" / "default" / "Library" / "bin" / "FreeCADCmd",
        Path("FreeCADCmd"),
    ):
        if candidate.is_file():
            return candidate
    return None


def expected_exported_urdf_path(robot_name: str, out_dir: Path) -> Path:
    """Canonical OVERCROSS URDF path under ``generated/<robot>/``."""
    return (
        Path(out_dir).resolve()
        / f"{robot_name}_description"
        / f"{robot_name}_description"
        / "urdf"
        / f"{robot_name}.urdf"
    )


def export_urdf_cmd(
    robot_name: str,
    out_dir: Path,
    fcstd_path: Optional[Path] = None,
    *,
    freecad_cmd: Optional[Path] = None,
    timeout: float = 600.0,
) -> ExportResult:
    """
    Export URDF via **FreeCADCmd** batch (no MCP GUI queue).

    Preferred path on Windows: stable, no 120 s ``execute_code`` timeout.
    """
    from bridge.permissions import PermissionDenied, WriteOperation, assert_write_allowed

    try:
        assert_write_allowed(
            WriteOperation.CAD_EXPORT_URDF,
            target=out_dir,
        )
    except PermissionDenied as exc:
        return ExportResult(ok=False, messages=[str(exc)])

    cmd = freecad_cmd or resolve_freecad_cmd()
    if cmd is None:
        return ExportResult(
            ok=False,
            messages=["FreeCADCmd not found. Set FREECAD_CMD or build FreeCAD via pixi."],
        )

    repo = Path(__file__).resolve().parents[1]
    if fcstd_path is None:
        fcstd_path = repo / "robots" / f"{robot_name}.FCStd"
    fcstd_path = Path(fcstd_path).resolve()
    if not fcstd_path.is_file():
        return ExportResult(
            ok=False,
            messages=[f"FCStd not found: {fcstd_path}. Run scripts/build_arm_2dof_fcstd_rpc.py"],
        )

    out_dir = Path(out_dir).resolve()

    from bridge.export_cache import (
        is_cache_enabled,
        record_fcstd_source,
        store_cached_export,
        try_restore_cached_export,
    )

    fcstd_hash = record_fcstd_source(fcstd_path)
    if is_cache_enabled():
        cached = try_restore_cached_export(
            robot_name,
            fcstd_path,
            out_dir,
            fcstd_sha256=fcstd_hash,
            root=repo,
        )
        if cached is not None:
            _record_export("export_urdf", cached, robot=robot_name, via="cache")
            return ExportResult(
                ok=True,
                path=cached,
                messages=[f"Restored RobotCAD export from cache: {cached}"],
            )
    scripts_dir = (repo / "scripts").as_posix()
    fcstd_posix = fcstd_path.as_posix()
    out_posix = out_dir.as_posix()

    code = (
        "import runpy, sys\n"
        f"sys.path.insert(0, r'{scripts_dir}')\n"
        "from robotcad_headless import export_fcstd_to_urdf\n"
        "from pathlib import Path\n"
        f"urdf, _ = export_fcstd_to_urdf(Path(r'{fcstd_posix}'), Path(r'{out_posix}'), "
        f"robot_name={robot_name!r})\n"
        "print('URDF_EXPORT_PATH:', urdf)\n"
    )

    try:
        proc = subprocess.run(
            [str(cmd), "-c", code],
            capture_output=True,
            text=True,
            timeout=timeout,
            cwd=str(repo),
        )
    except subprocess.TimeoutExpired:
        return ExportResult(
            ok=False,
            messages=[f"FreeCADCmd export timed out after {timeout}s"],
        )
    except OSError as exc:
        return ExportResult(ok=False, messages=[f"Failed to run FreeCADCmd: {exc}"])

    combined = (proc.stdout or "") + (proc.stderr or "")
    urdf_path: Optional[Path] = None
    for line in combined.splitlines():
        if line.startswith("URDF_EXPORT_PATH:"):
            urdf_path = Path(line.split(":", 1)[1].strip())
            break

    if proc.returncode != 0:
        tail = combined.strip()[-1200:] if combined.strip() else f"exit code {proc.returncode}"
        return ExportResult(ok=False, messages=[f"FreeCADCmd export failed: {tail}"])

    if urdf_path is None or not urdf_path.is_file():
        fallback = expected_exported_urdf_path(robot_name, out_dir)
        if fallback.is_file():
            urdf_path = fallback
        else:
            return ExportResult(
                ok=False,
                messages=[
                    "FreeCADCmd finished but URDF path not found in output",
                    combined.strip()[-800:],
                ],
            )

    _record_export("export_urdf", urdf_path, robot=robot_name)
    if is_cache_enabled():
        store_cached_export(
            robot_name,
            fcstd_path,
            out_dir,
            urdf_path,
            fcstd_sha256=fcstd_hash,
            root=repo,
        )
    return ExportResult(
        ok=True,
        path=urdf_path,
        messages=[f"Exported URDF via FreeCADCmd to {urdf_path}"],
    )


def export_urdf(
    robot_name: str,
    out_dir: Path,
    fcstd_path: Optional[Path] = None,
    host: str = DEFAULT_HOST,
    port: int = DEFAULT_PORT,
    timeout: float = DEFAULT_TIMEOUT,
    *,
    prefer_cmd: bool = True,
) -> ExportResult:
    """
    Export a robot to URDF via RobotCAD/CROSS inside FreeCAD.

    By default uses **FreeCADCmd** batch export (``export_urdf_cmd``). Falls back to
    MCP ``execute_code`` when ``prefer_cmd=False`` or batch export is unavailable.

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
    from bridge.permissions import PermissionDenied, WriteOperation, assert_write_allowed

    try:
        assert_write_allowed(
            WriteOperation.CAD_EXPORT_URDF,
            target=out_dir,
        )
    except PermissionDenied as exc:
        return ExportResult(ok=False, messages=[str(exc)])

    repo = Path(__file__).resolve().parents[1]
    if fcstd_path is None:
        fcstd_path = repo / "robots" / f"{robot_name}.FCStd"
    fcstd_resolved = Path(fcstd_path).resolve()
    out_resolved = Path(out_dir).resolve()

    from bridge.export_cache import (
        is_cache_enabled,
        record_fcstd_source,
        try_restore_cached_export,
    )

    if fcstd_resolved.is_file() and is_cache_enabled():
        fcstd_hash = record_fcstd_source(fcstd_resolved)
        cached = try_restore_cached_export(
            robot_name,
            fcstd_resolved,
            out_resolved,
            fcstd_sha256=fcstd_hash,
            root=repo,
        )
        if cached is not None:
            _record_export("export_urdf", cached, robot=robot_name, via="cache")
            return ExportResult(
                ok=True,
                path=cached,
                messages=[f"Restored RobotCAD export from cache: {cached}"],
            )

    if prefer_cmd:
        cmd_result = export_urdf_cmd(
            robot_name, out_dir, fcstd_path=fcstd_path, timeout=max(timeout, 120.0)
        )
        if cmd_result.ok:
            return cmd_result
        # If batch failed, try RPC when server is up (optional fallback).
        rpc = _connect(host, port, timeout)
        if rpc is None:
            return cmd_result

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
    if fcstd_path is not None:
        doc_arg = Path(fcstd_path).resolve().as_posix()
    else:
        doc_arg = robot_name

    pkg_name = f"{robot_name}_description"
    snippet = _EXPORT_URDF_SNIPPET_TEMPLATE.format(
        doc_name=doc_arg,
        out_dir=Path(out_dir).resolve().as_posix(),
        pkg_name=pkg_name,
    )

    try:
        raw = rpc.execute_code(snippet)
        inner = _interpret_execute_code(raw)

        ok   = bool(inner.get("success", False))
        msgs = [inner.get("message", "No message returned")]
        if inner.get("trace"):
            msgs.append(str(inner["trace"]))
        path = Path(inner["path"]) if ok and inner.get("path") else None

        if ok and path is not None:
            _record_export("export_urdf", path, robot=robot_name, via="rpc")
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
    from bridge.permissions import PermissionDenied, WriteOperation, assert_write_allowed

    try:
        assert_write_allowed(
            WriteOperation.CAD_EXPORT_WORLD,
            target=out_dir,
        )
    except PermissionDenied as exc:
        return ExportResult(ok=False, messages=[str(exc)])

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
    _record_export("world_sdf", dest, world=world_name)
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
        inner = _interpret_execute_code(raw)
        ok    = bool(inner.get("success", False))
        msgs  = inner.get("issues", []) or [inner.get("message", "")]
        return ExportResult(ok=ok, messages=msgs)
    except Exception as exc:
        return ExportResult(ok=False, messages=[f"RPC error: {exc}"])
