"""
Write-capability policy for bridge, runner, and workbench automation paths.

Read operations are always allowed. Write operations must pass
:func:`assert_write_allowed` before modifying disk, FreeCAD documents, or Gazebo state.

Environment:
  BRIDGE_WRITE_POLICY — ``allow`` (default), ``deny``, ``generated_only``
  CI=true — implies ``generated_only`` unless policy is explicitly ``allow``
"""

from __future__ import annotations

import os
from dataclasses import dataclass
from enum import Enum
from pathlib import Path
from typing import FrozenSet, Optional

_REPO = Path(__file__).resolve().parent.parent


class WriteOperation(str, Enum):
    """Registered write surfaces (extend when adding new mutating APIs)."""

    CAD_EXPORT_URDF = "cad.export_urdf"
    CAD_EXPORT_WORLD = "cad.export_world_sdf"
    GAZEBO_SPAWN = "gazebo.spawn_model"
    GAZEBO_REMOVE = "gazebo.remove_model"
    GAZEBO_SIM_CONTROL = "gazebo.sim_control"
    RUNNER_WRITE_RESULT = "runner.write_result"
    FILES_TEMP_URDF = "files.temp_urdf"


class PermissionDenied(PermissionError):
    """Raised when a write operation is blocked by policy."""


@dataclass(frozen=True)
class WriteCapability:
    operation: WriteOperation
    description: str
    paths: str  # human-readable path pattern summary


# Registry for docs and policy (keep in sync with docs/permissions-and-write-surface.md).
WRITE_CAPABILITIES: tuple[WriteCapability, ...] = (
    WriteCapability(
        WriteOperation.CAD_EXPORT_URDF,
        "RobotCAD/FreeCAD export to generated/",
        "generated/<robot>/**, may read robots/<robot>.FCStd",
    ),
    WriteCapability(
        WriteOperation.CAD_EXPORT_WORLD,
        "Copy world SDF into generated/",
        "generated/<world>/**",
    ),
    WriteCapability(
        WriteOperation.GAZEBO_SPAWN,
        "Spawn URDF/SDF in running Gazebo",
        "Gazebo world entity + optional /tmp spawn URDF",
    ),
    WriteCapability(
        WriteOperation.GAZEBO_REMOVE,
        "Remove model from Gazebo world",
        "Gazebo world entity",
    ),
    WriteCapability(
        WriteOperation.GAZEBO_SIM_CONTROL,
        "Pause, resume, reset simulation",
        "Gazebo world state",
    ),
    WriteCapability(
        WriteOperation.RUNNER_WRITE_RESULT,
        "Write scenario run artifacts",
        "sim_runs/<run_id>/**",
    ),
    WriteCapability(
        WriteOperation.FILES_TEMP_URDF,
        "Prepared URDF for gz spawn",
        "/tmp/spawn_*.urdf or container temp paths",
    ),
)

_GENERATED_ONLY_OPS: FrozenSet[WriteOperation] = frozenset(
    {
        WriteOperation.CAD_EXPORT_URDF,
        WriteOperation.CAD_EXPORT_WORLD,
        WriteOperation.GAZEBO_SPAWN,
        WriteOperation.GAZEBO_REMOVE,
        WriteOperation.GAZEBO_SIM_CONTROL,
        WriteOperation.RUNNER_WRITE_RESULT,
        WriteOperation.FILES_TEMP_URDF,
    }
)


def effective_write_policy() -> str:
    explicit = os.environ.get("BRIDGE_WRITE_POLICY", "").strip().lower()
    if explicit in ("allow", "deny", "generated_only"):
        return explicit
    if _truthy("CI"):
        return "generated_only"
    return "allow"


def _truthy(name: str) -> bool:
    return os.environ.get(name, "").strip().lower() in ("1", "true", "yes")


def assert_write_allowed(
    operation: WriteOperation,
    *,
    target: Optional[Path] = None,
    detail: str = "",
) -> None:
    """
    Raise :class:`PermissionDenied` when policy blocks ``operation``.

    ``target`` is optional context (e.g. path being written) for error messages.
    """
    policy = effective_write_policy()
    if policy == "allow":
        return
    if policy == "deny":
        raise PermissionDenied(_msg(operation, target, detail, policy))

    # generated_only
    if operation not in _GENERATED_ONLY_OPS:
        raise PermissionDenied(_msg(operation, target, detail, policy))

    if target is not None:
        _assert_generated_path(target, operation, detail, policy)


def _assert_generated_path(
    path: Path,
    operation: WriteOperation,
    detail: str,
    policy: str,
) -> None:
    """Block writes outside generated/, sim_runs/, and temp spawn paths."""
    p = path.resolve()
    parts = {part.lower() for part in p.parts}
    if parts & {"generated", "sim_runs", "tmp", "temp"}:
        return
    try:
        import tempfile

        temp_root = Path(tempfile.gettempdir()).resolve()
        if p == temp_root or temp_root in p.parents:
            return
    except OSError:
        pass
    raise PermissionDenied(_msg(operation, path, detail, policy))


def _msg(
    operation: WriteOperation,
    target: Optional[Path],
    detail: str,
    policy: str,
) -> str:
    cap = next((c for c in WRITE_CAPABILITIES if c.operation == operation), None)
    desc = cap.description if cap else operation.value
    where = f" → {target}" if target else ""
    extra = f" ({detail})" if detail else ""
    return (
        f"Write blocked by BRIDGE_WRITE_POLICY={policy}: {operation.value} — {desc}{where}{extra}. "
        "See docs/permissions-and-write-surface.md."
    )


def list_write_capabilities() -> list[dict[str, str]]:
    """Summary for docs/tests."""
    return [
        {
            "operation": c.operation.value,
            "description": c.description,
            "paths": c.paths,
        }
        for c in WRITE_CAPABILITIES
    ]
