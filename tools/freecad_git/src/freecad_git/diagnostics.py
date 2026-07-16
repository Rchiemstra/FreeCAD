"""Trusted headless FreeCAD diagnostics (explicitly non-authoritative)."""

from __future__ import annotations

import json
import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

from .config import Config
from .errors import DiagnosticFailureError


def _find_freecad_cmd(config: Config) -> str | None:
    """Discover FreeCADCmd executable."""
    candidates: list[str] = []

    if config.freecad_cmd:
        candidates.append(config.freecad_cmd)
    if config.diagnostics.freecad_cmd:
        candidates.append(config.diagnostics.freecad_cmd)

    env_cmd = os.environ.get("FREECAD_CMD")
    if env_cmd:
        candidates.append(env_cmd)

    for name in ("FreeCADCmd", "freecadcmd", "FreeCADCmd.exe"):
        found = shutil.which(name)
        if found:
            candidates.append(found)

    for candidate in candidates:
        if candidate and Path(candidate).is_file():
            return candidate
    return None


_DIAGNOSTIC_SCRIPT = '''
import json, sys, os
sys.path.insert(0, os.environ.get("FREECAD_GIT_DIAG_PATH", ""))
result = {"status": "non_authoritative_diagnostic", "objects": []}
try:
    import FreeCAD
    doc = FreeCAD.openDocument(sys.argv[1])
    result["document"] = doc.Name
    result["recompute_errors"] = []
    try:
        doc.recompute()
    except Exception as e:
        result["recompute_errors"].append(str(e))
    for obj in doc.Objects:
        entry = {"name": obj.Name, "label": obj.Label, "type": obj.TypeId}
        if hasattr(obj, "Shape") and obj.Shape:
            try:
                bb = obj.Shape.BoundBox
                entry["bounding_box"] = {
                    "xmin": bb.XMin, "ymin": bb.YMin, "zmin": bb.ZMin,
                    "xmax": bb.XMax, "ymax": bb.YMax, "zmax": bb.ZMax,
                }
                entry["volume"] = obj.Shape.Volume
                entry["area"] = obj.Shape.Area
            except Exception:
                pass
        if hasattr(obj, "isValid"):
            entry["valid"] = obj.isValid()
        result["objects"].append(entry)
    FreeCAD.closeDocument(doc.Name)
except Exception as e:
    result["error"] = str(e)
print(json.dumps(result, indent=2))
'''


def run_diagnostics(fcstd_path: Path, config: Config) -> str:
    """Run sandboxed FreeCAD diagnostics on a trusted file."""
    fcstd_path = fcstd_path.resolve()
    if not fcstd_path.is_file():
        raise DiagnosticFailureError(f"file not found: {fcstd_path}")

    freecad_cmd = _find_freecad_cmd(config)
    if not freecad_cmd:
        raise DiagnosticFailureError(
            "FreeCADCmd not found. Set freecad_cmd in .freecad-git.toml or FREECAD_CMD env."
        )

    with tempfile.TemporaryDirectory(prefix="freecad-git-diag-") as tmpdir:
        script_path = Path(tmpdir) / "diagnostic.py"
        script_path.write_text(_DIAGNOSTIC_SCRIPT, encoding="utf-8")

        env = os.environ.copy()
        env["HOME"] = tmpdir
        env["USERPROFILE"] = tmpdir
        env["FREECAD_USER_HOME"] = tmpdir
        env["FREECAD_GIT_DIAG_PATH"] = str(Path(__file__).resolve().parents[2])

        cmd = [freecad_cmd, str(script_path), str(fcstd_path)]

        try:
            proc = subprocess.run(
                cmd,
                capture_output=True,
                text=True,
                timeout=config.diagnostics.max_runtime_seconds,
                env=env,
                cwd=tmpdir,
            )
        except subprocess.TimeoutExpired as exc:
            raise DiagnosticFailureError(
                f"diagnostics timed out after {config.diagnostics.max_runtime_seconds}s"
            ) from exc
        except OSError as exc:
            raise DiagnosticFailureError(f"failed to run FreeCADCmd: {exc}") from exc

        if proc.returncode != 0:
            raise DiagnosticFailureError(
                f"FreeCADCmd exited with code {proc.returncode}: {proc.stderr[:500]}"
            )

        header = (
            "=== NON-AUTHORITATIVE DIAGNOSTIC OUTPUT ===\n"
            "This data is from headless FreeCAD and must not be used for CI verification.\n"
            "Opening a document may load modules, linked files, workbenches, and solvers.\n\n"
        )
        return header + (proc.stdout or proc.stderr or "{}")
