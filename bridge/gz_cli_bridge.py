# SPDX-License-Identifier: LGPL-2.1-or-later
"""
bridge.gz_cli_bridge — Gazebo control via ``gz`` CLI (Linux / Docker E2E).

The stdio :mod:`bridge.gazebo_bridge` path relies on *gazebo-mcp* tool wiring;
``gazebo_spawn_model`` does not pass ``model_xml`` through to the bridge today.
For unattended Docker E2E we spawn URDF/SDF with the Gazebo CLI instead, and
return *executor-shaped* state dicts with conservative telemetry fields.
"""
from __future__ import annotations

import logging
import math
import os
import re
import subprocess
import time
from pathlib import Path
from typing import Any, Dict, List, Optional

log = logging.getLogger(__name__)

_REPO_ROOT = Path(__file__).resolve().parent.parent

# Wall-clock epoch aligned to successful spawn — drives ScenarioExecutor time axis.
_SIM_T0: Optional[float] = None


def _repo(path: str | Path) -> Path:
    p = Path(path)
    if p.is_absolute():
        return p
    return _REPO_ROOT / p


def _run_gz(args: List[str], timeout: float = 30.0) -> subprocess.CompletedProcess:
    return subprocess.run(
        ["gz", *args],
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )


def spawn_model(
    model_name: str,
    urdf_path: str | Path,
    initial_pose: Optional[Dict[str, Any]] = None,
) -> None:
    """Spawn URDF/SDF via the world's ``create`` service (Gazebo Sim Harmonic+).

    ``gz model --spawn-file`` is not available on gz-sim 8 CLI; the documented
    path is ``gz service … /world/<world>/create`` with ``gz.msgs.EntityFactory``.
    """
    global _SIM_T0

    path = _repo(urdf_path).resolve()
    if not path.is_file():
        raise FileNotFoundError(f"Robot file not found: {path}")

    world = os.environ.get("GZ_SIM_WORLD_NAME", "empty_world").strip() or "empty_world"
    initial_pose = initial_pose or {}
    x = float(initial_pose.get("x", 0.0))
    y = float(initial_pose.get("y", 0.0))
    z = float(initial_pose.get("z", 0.0))
    yaw = float(initial_pose.get("yaw", 0.0))
    qz = math.sin(yaw / 2.0)
    qw = math.cos(yaw / 2.0)

    sdf_path = path.as_posix()

    # Best-effort cleanup — ``gz model --remove`` is absent on modern gz CLI.
    rm = _run_gz(
        [
            "service",
            "-s",
            f"/world/{world}/remove",
            "--reqtype",
            "gz.msgs.Entity",
            "--reptype",
            "gz.msgs.Boolean",
            "--timeout",
            "8000",
            "--req",
            f'name: "{model_name}"',
        ],
        timeout=30.0,
    )
    if rm.returncode != 0:
        log.debug(
            "[gz_cli_bridge] pre-remove (ignored): %s",
            (rm.stderr or rm.stdout or "")[:200],
        )

    req = (
        f'name: "{model_name}", sdf_filename: "{sdf_path}", '
        f"pose {{ position {{ x: {x} y: {y} z: {z} }} "
        f"orientation {{ x: 0 y: 0 z: {qz} w: {qw} }} }}"
    )
    proc = _run_gz(
        [
            "service",
            "-s",
            f"/world/{world}/create",
            "--reqtype",
            "gz.msgs.EntityFactory",
            "--reptype",
            "gz.msgs.Boolean",
            "--timeout",
            "15000",
            "--req",
            req,
        ],
        timeout=90.0,
    )
    out = (proc.stderr or proc.stdout or "").strip()
    if proc.returncode != 0:
        log.warning("[gz_cli_bridge] gz service create failed (%s): %s", proc.returncode, out[:500])
        raise RuntimeError(f"gz service spawn failed for {model_name} in world {world}: {out[:800]}")

    log.info("[gz_cli_bridge] Spawn OK via /world/%s/create (%s)", world, model_name)
    _SIM_T0 = time.monotonic()


def _parse_pose_line(text: str) -> Dict[str, float]:
    nums = re.findall(r"[-+]?(?:\d*\.\d+|\d+)(?:[eE][-+]?\d+)?", text)
    out = {"x": 0.0, "y": 0.0, "z": 0.0}
    if len(nums) >= 3:
        out["x"], out["y"], out["z"] = float(nums[0]), float(nums[1]), float(nums[2])
    return out


def get_model_state(model_name: str) -> Dict[str, Any]:
    """Return executor-shaped telemetry dict for *model_name*."""
    proc = _run_gz(["model", "-m", model_name, "-p"], timeout=10.0)
    pose_txt = (proc.stdout or "") + "\n" + (proc.stderr or "")
    pos = _parse_pose_line(pose_txt)

    t0 = _SIM_T0 or time.monotonic()
    sim_elapsed = max(0.0, time.monotonic() - t0)

    return {
        "sim_time": sim_elapsed,
        "rtf": float(os.environ.get("E2E_ASSUMED_RTF", "1.0")),
        "end_effector": {"position": {"x": pos["x"], "y": pos["y"], "z": pos["z"]}},
        "joint_states": [],
        "contacts": [],
    }


def resume_simulation() -> None:
    """Physics usually starts unpaused after spawn; nothing required for E2E smoke."""
    return


def pause_simulation() -> None:
    """Optional hook — gz CLI pause semantics differ by version; keep smoke runs simple."""
    return
