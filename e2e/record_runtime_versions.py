#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""
Record observed runtime versions for an E2E run → sim_runs/e2e_*/versions.yaml.

Compares against config/runtime-versions.lock.yaml. With E2E_VERSION_STRICT=1
(default in docker/compose.e2e.yml), mismatches fail the E2E run.
"""
from __future__ import annotations

import os
import platform
import re
import subprocess
import sys
from pathlib import Path
from shutil import which
from typing import Any, Dict, List, Optional

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))

from bridge.runtime_versions import (  # noqa: E402
    compare_observed_to_lock,
    fcstd_path,
    load_runtime_lock,
    locked_apt_versions,
    query_apt_versions,
    read_builtin_base_image_ref,
    sha256_file,
    verify_fcstd,
    version_check_strict,
)


def _run(cmd: list[str], *, timeout: float = 30.0) -> str:
    try:
        proc = subprocess.run(
            cmd,
            capture_output=True,
            text=True,
            timeout=timeout,
            check=False,
        )
        return ((proc.stdout or "") + (proc.stderr or "")).strip()
    except Exception as exc:
        return f"error: {exc}"


def _first_match(pattern: str, text: str) -> Optional[str]:
    m = re.search(pattern, text, re.IGNORECASE | re.MULTILINE)
    return m.group(1).strip() if m else None


def _robotcad_commit() -> Optional[str]:
    rc = Path("/opt/robotcad")
    if not (rc / ".git").is_dir():
        return None
    out = _run(["git", "-C", str(rc), "rev-parse", "HEAD"])
    return out if re.fullmatch(r"[0-9a-f]{40}", out or "") else None


def collect_observed(root: Path) -> Dict[str, Any]:
    freecad_cmd = os.environ.get("FREECAD_CMD") or "FreeCADCmd"
    gz_out = _run(["gz", "sim", "--version"]) if which("gz") else ""
    lock = load_runtime_lock(root)
    apt_packages = list(locked_apt_versions(lock).keys())

    observed: Dict[str, Any] = {
        "schema_version": 2,
        "platform": platform.platform(),
        "python": platform.python_version(),
        "freecad": _first_match(r"FreeCAD\s+([\d.]+)", _run([freecad_cmd, "--version"])),
        "gazebo_sim": _first_match(r"version\s+([\d.]+)", gz_out),
        "robotcad_commit": _robotcad_commit(),
        "docker_base_image_builtin": read_builtin_base_image_ref(),
        "docker_base_image_ref": read_builtin_base_image_ref(),
        "apt_versions": query_apt_versions(apt_packages),
        "mcp_venv": {},
        "robot_source": {},
        "lock_file": str(root / "config" / "runtime-versions.lock.yaml"),
    }

    venv_pip = Path("/opt/mcp-venv/bin/pip")
    if venv_pip.is_file():
        for pkg in ("mcp", "pydantic", "PyYAML", "validators"):
            out = _run([str(venv_pip), "show", pkg])
            ver = None
            for line in out.splitlines():
                if line.startswith("Version:"):
                    ver = line.split(":", 1)[1].strip()
                    break
            if ver:
                observed["mcp_venv"][pkg.lower()] = ver

    ok, msg = verify_fcstd(root, lock=lock)
    observed["robot_source"]["verify"] = msg
    observed["robot_source"]["ok"] = ok
    p = fcstd_path(root, lock=lock)
    if p.is_file():
        observed["robot_source"]["path"] = str(p.relative_to(root))
        observed["robot_source"]["sha256"] = sha256_file(p)
        observed["robot_source"]["bytes"] = p.stat().st_size

    return observed


def main() -> int:
    import yaml  # type: ignore

    root = ROOT
    out_dir = Path(os.environ.get("E2E_RUN_DIR", root / "sim_runs" / "e2e_versions_probe"))
    out_dir.mkdir(parents=True, exist_ok=True)
    out_path = out_dir / "versions.yaml"

    lock = load_runtime_lock(root)
    observed = collect_observed(root)
    warnings, errors = compare_observed_to_lock(observed, lock)

    observed["expected"] = {
        "lock_updated": lock.get("updated"),
        "docker_e2e": lock.get("docker_e2e"),
        "pypi": lock.get("pypi"),
        "mcp_servers": lock.get("mcp_servers"),
    }
    observed["drift_warnings"] = warnings
    observed["drift_errors"] = errors
    observed["version_check_strict"] = version_check_strict()

    out_path.write_text(
        yaml.dump(observed, default_flow_style=False, sort_keys=True),
        encoding="utf-8",
    )
    print(f"[e2e] Wrote {out_path}")

    for w in warnings:
        print(f"[e2e] WARN version drift: {w}", file=sys.stderr)

    if errors:
        for e in errors:
            print(f"[e2e] ERROR version drift: {e}", file=sys.stderr)
        if version_check_strict():
            print(
                "[e2e] FATAL: runtime versions do not match "
                "config/runtime-versions.lock.yaml (E2E_VERSION_STRICT=1)",
                file=sys.stderr,
            )
            return 1

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
