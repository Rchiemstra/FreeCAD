"""
Spawn URDF/SDF in a running gz-sim Docker container via ``gz service``.

Used when ros_gz ``parameter_bridge`` service calls hang (common on Windows→WSL
live tests) but ``gz service`` from the gz-sim container works.
"""

from __future__ import annotations

import logging
import math
import os
import subprocess
import tempfile
from pathlib import Path
from typing import Any, Dict, Optional

from bridge.gazebo_bridge import GazeboResult, _get_wsl_path

logger = logging.getLogger(__name__)

_DEFAULT_GZ_CONTAINER = "gz-sim-sever"


def use_gz_docker_spawn() -> bool:
    flag = os.environ.get("GAZEBO_SPAWN_VIA_GZ_CLI", "").strip().lower()
    if flag in ("0", "false", "no"):
        return False
    if flag in ("1", "true", "yes"):
        return True
    return os.environ.get("GAZEBO_MCP_DOCKER", "").strip().lower() in ("1", "true", "yes")


def _gz_container() -> str:
    return os.environ.get("GZ_SIM_CONTAINER_NAME", _DEFAULT_GZ_CONTAINER).strip() or _DEFAULT_GZ_CONTAINER


def _world_name() -> str:
    from bridge.gazebo_lifecycle import resolve_world_name

    return resolve_world_name()


def container_urdf_path(urdf_path: Path) -> Optional[str]:
    """
    Map a host RobotCAD export path to the bind-mount inside gz-sim.

    Mount: ``.../arm_2dof_description`` (package root) → ``/models/arm_2dof_description``.
    """
    from bridge.urdf_for_gazebo import robotcad_description_root

    root = robotcad_description_root(urdf_path)
    if root is None:
        return None
    try:
        rel = urdf_path.resolve().relative_to(root.resolve())
    except ValueError:
        return None
    return f"/models/arm_2dof_description/{rel.as_posix()}"


def _docker_gz(args: list[str], timeout: float) -> subprocess.CompletedProcess[str]:
    cmd = ["wsl", "--", "docker", "exec", _gz_container(), "gz", *args]
    return subprocess.run(
        cmd,
        capture_output=True,
        text=True,
        timeout=timeout,
        check=False,
    )


def unpause_world(timeout: float = 15.0) -> GazeboResult:
    world = _world_name()
    proc = _docker_gz(
        [
            "service",
            "-s",
            f"/world/{world}/control",
            "--reqtype",
            "gz.msgs.WorldControl",
            "--reptype",
            "gz.msgs.Boolean",
            "--timeout",
            str(int(timeout * 1000)),
            "--req",
            "pause: false",
        ],
        timeout=timeout + 5.0,
    )
    out = (proc.stdout or proc.stderr or "").strip()
    ok = proc.returncode == 0 and "data: true" in out
    return GazeboResult(ok=ok, messages=[out or f"unpause exit {proc.returncode}"])


def _docker_cp_to_container(host_path: Path, remote: str) -> None:
    wsl_src = _get_wsl_path(host_path)
    if not wsl_src:
        raise FileNotFoundError(f"Cannot map path to WSL: {host_path}")
    proc = subprocess.run(
        ["wsl", "--", "docker", "cp", wsl_src, f"{_gz_container()}:{remote}"],
        capture_output=True,
        text=True,
        timeout=30.0,
        check=False,
    )
    if proc.returncode != 0:
        raise RuntimeError((proc.stderr or proc.stdout or "docker cp failed")[:500])


def _stage_urdf_in_container(host_path: Path, model_name: str) -> str:
    mounted = container_urdf_path(host_path)
    if mounted:
        return mounted
    remote = f"/tmp/spawn_models/{model_name}.urdf"
    subprocess.run(
        ["wsl", "--", "docker", "exec", _gz_container(), "mkdir", "-p", "/tmp/spawn_models"],
        capture_output=True,
        timeout=15.0,
        check=False,
    )
    _docker_cp_to_container(host_path, remote)
    return remote


def spawn_urdf_file(
    model_name: str,
    urdf_path: Path,
    *,
    pose: Optional[Dict[str, Any]] = None,
    timeout: float = 60.0,
) -> GazeboResult:
    """Spawn via ``/world/<world>/create`` with ``sdf_filename`` inside gz-sim Docker."""
    path = Path(urdf_path)
    if not path.is_file():
        return GazeboResult(ok=False, messages=[f"URDF not found: {path}"])

    pose = pose or {}
    pos = pose.get("position", {})
    x = float(pos.get("x", 0))
    y = float(pos.get("y", 0))
    z = float(pos.get("z", 0))
    ori = pose.get("orientation", {})
    yaw = float(ori.get("yaw", 0))
    qz = math.sin(yaw / 2.0)
    qw = math.cos(yaw / 2.0)

    try:
        remote = _stage_urdf_in_container(path, model_name)
    except Exception as exc:
        return GazeboResult(ok=False, messages=[str(exc)])

    unpause = unpause_world(timeout=min(timeout, 15.0))
    if not unpause.ok:
        logger.warning("gz docker unpause: %s", unpause.messages)

    req = (
        f'sdf_filename: "{remote}", name: "{model_name}", allow_renaming: false, '
        f"pose {{ position {{ x: {x} y: {y} z: {z} }} "
        f"orientation {{ x: 0 y: 0 z: {qz} w: {qw} }} }}"
    )
    world = _world_name()
    proc = _docker_gz(
        [
            "service",
            "-s",
            f"/world/{world}/create",
            "--reqtype",
            "gz.msgs.EntityFactory",
            "--reptype",
            "gz.msgs.Boolean",
            "--timeout",
            str(int(timeout * 1000)),
            "--req",
            req,
        ],
        timeout=timeout + 10.0,
    )
    out = (proc.stdout or proc.stderr or "").strip()
    ok = proc.returncode == 0 and "data: true" in out
    if not ok:
        return GazeboResult(
            ok=False,
            messages=[out or f"spawn exit {proc.returncode}", f"container_path={remote}"],
        )
    return GazeboResult(
        ok=True,
        data={"model_name": model_name, "container_urdf": remote},
        messages=[out, f"spawned via gz service ({_gz_container()}, world={world})"],
    )


def spawn_prepared_xml(
    model_name: str,
    urdf_xml: str,
    *,
    host_urdf_path: Optional[Path] = None,
    pose: Optional[Dict[str, Any]] = None,
    timeout: float = 60.0,
) -> GazeboResult:
    """Spawn prepared URDF XML (writes a temp file when content differs from on-disk export)."""
    from bridge.permissions import PermissionDenied, WriteOperation, assert_write_allowed

    try:
        assert_write_allowed(WriteOperation.GAZEBO_SPAWN)
    except PermissionDenied as exc:
        return GazeboResult(ok=False, messages=[str(exc)])

    if host_urdf_path is not None:
        host_path = Path(host_urdf_path)
        if host_path.is_file():
            try:
                on_disk = host_path.read_text(encoding="utf-8")
            except OSError:
                on_disk = ""
            if urdf_xml.strip() == on_disk.strip():
                return spawn_urdf_file(model_name, host_path, pose=pose, timeout=timeout)

    with tempfile.NamedTemporaryFile(
        mode="w", suffix=".urdf", delete=False, encoding="utf-8"
    ) as tmp:
        tmp_path = Path(tmp.name)
        try:
            assert_write_allowed(
                WriteOperation.FILES_TEMP_URDF,
                target=tmp_path,
            )
        except PermissionDenied as exc:
            return GazeboResult(ok=False, messages=[str(exc)])
        tmp.write(urdf_xml)

    try:
        return spawn_urdf_file(model_name, tmp_path, pose=pose, timeout=timeout)
    finally:
        try:
            tmp_path.unlink(missing_ok=True)
        except OSError:
            pass
