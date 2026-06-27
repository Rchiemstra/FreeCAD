"""
Canonical Gazebo live-stack lifecycle settings (Windows + WSL + Docker).

World name must match the ``<world name="…">`` in the loaded SDF. This project
uses ``worlds/empty_world.sdf`` → world name ``empty_world`` for E2E and the
normalized live/fast stack.

Environment variables (set together for live work):

| Variable | Default | Role |
| --- | --- | --- |
| ``GZ_SIM_CONTAINER_NAME`` | ``gz-sim-sever`` | Headless gz-sim Docker container |
| ``ROS_GZ_BRIDGE_CONTAINER`` | ``ros-gz-bridge`` | ros_gz sidecar (shares gz network) |
| ``GZ_SIM_WORLD_NAME`` | ``empty_world`` | gz service paths ``/world/<name>/…`` |
| ``GAZEBO_WORLD_NAME`` | ``empty_world`` | ros_gz ``parameter_bridge`` topic prefix |
| ``GAZEBO_SPAWN_VIA_GZ_CLI`` | ``1`` when ``GAZEBO_MCP_DOCKER=1`` | Spawn via ``docker exec gz …`` not ros_gz |
| ``GAZEBO_MCP_DOCKER`` | unset | Launch gazebo-mcp inside gz container network |
| ``GZ_SIM_RESOURCE_PATH`` | ``/models`` (in container) | RobotCAD package mount |

Stack profiles:

- **e2e** — ``docker/compose.e2e.yml``; ``gz_cli_bridge``; world ``empty_world``; no ros_gz.
- **live_fast** — ``scripts/run_gz_sim_fast.sh``; OSRF gz-harmonic in Docker; optional ros_gz.
- **live_source** — ``Start-gz-sim.bat`` → source build (slow first time); same world SDF when possible.

Spawn/control split:

- **gz CLI** (``bridge/gz_cli_bridge``, ``bridge/gazebo_gz_docker``): spawn, unpause, remove — reliable on WSL.
- **ros_gz** (``ros-gz-bridge`` container): MCP pause/unpause/reset when bridge works; can hang on Windows.
"""

from __future__ import annotations

import os
from pathlib import Path
from typing import List, Tuple

_REPO_ROOT = Path(__file__).resolve().parent.parent

DEFAULT_GZ_CONTAINER = "gz-sim-sever"
DEFAULT_BRIDGE_CONTAINER = "ros-gz-bridge"
DEFAULT_WORLD_NAME = "empty_world"
DEFAULT_WORLD_SDF = "worlds/empty_world.sdf"
# Legacy OSRF empty.sdf uses world name "empty" — avoid unless explicitly selected.
LEGACY_EMPTY_WORLD_NAME = "empty"


def repo_root() -> Path:
    return _REPO_ROOT


def world_sdf_path(root: Path | None = None) -> Path:
    return (root or repo_root()) / DEFAULT_WORLD_SDF


def gz_container_name() -> str:
    return os.environ.get("GZ_SIM_CONTAINER_NAME", DEFAULT_GZ_CONTAINER).strip() or DEFAULT_GZ_CONTAINER


def bridge_container_name() -> str:
    return (
        os.environ.get("ROS_GZ_BRIDGE_CONTAINER", DEFAULT_BRIDGE_CONTAINER).strip()
        or DEFAULT_BRIDGE_CONTAINER
    )


def resolve_world_name() -> str:
    """
    Single world name for gz services and ros_gz bridge.

    Prefers ``GZ_SIM_WORLD_NAME``, then ``GAZEBO_WORLD_NAME``, then ``empty_world``.
    """
    for key in ("GZ_SIM_WORLD_NAME", "GAZEBO_WORLD_NAME"):
        val = os.environ.get(key, "").strip()
        if val:
            return val
    return DEFAULT_WORLD_NAME


def log_lifecycle(phase: str, **fields: str) -> None:
    """Record a lifecycle event into the active sim run, if any."""
    from bridge.run_context import record_lifecycle

    record_lifecycle(phase, **fields)


def export_live_defaults() -> dict[str, str]:
    """Env map scripts should apply before starting the live stack."""
    world = resolve_world_name()
    return {
        "GZ_SIM_CONTAINER_NAME": gz_container_name(),
        "ROS_GZ_BRIDGE_CONTAINER": bridge_container_name(),
        "GZ_SIM_WORLD_NAME": world,
        "GAZEBO_WORLD_NAME": world,
        "GZ_SIM_RESOURCE_PATH": os.environ.get("GZ_SIM_RESOURCE_PATH", "/models"),
    }


def validate_world_env() -> Tuple[bool, str]:
    """Return (ok, message) when gz vs ros_gz world names disagree."""
    gz = os.environ.get("GZ_SIM_WORLD_NAME", "").strip()
    ros = os.environ.get("GAZEBO_WORLD_NAME", "").strip()
    if gz and ros and gz != ros:
        return False, f"GZ_SIM_WORLD_NAME={gz!r} != GAZEBO_WORLD_NAME={ros!r}"
    return True, "OK"


def stack_container_names() -> List[str]:
    return [gz_container_name(), bridge_container_name()]
