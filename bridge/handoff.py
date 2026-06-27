"""
bridge.handoff — High-level FreeCAD → Gazebo pipeline.

Single-call orchestration for the full export-validate-spawn workflow:
  1. Validate the source URDF (or export from FreeCAD if FCStd exists)
  2. Stage the world SDF
  3. Wait for Gazebo to be ready
  4. Spawn the robot in Gazebo

Usage:
    from bridge.handoff import export_and_spawn, HandoffResult

    result = export_and_spawn(
        robot_name="arm_2dof",
        world_name="empty_world",
        project_root=Path("."),
    )
    print(result.summary())

Design:
  - export_and_spawn() is the primary entry point for both human operators
    and LLM agents using the MCP tools.
  - It short-circuits early with a clear message on any blocker so LLM
    agents don't burn tokens retrying blocked steps.
  - Each step result is accumulated into HandoffResult.steps for debugging.
"""

from __future__ import annotations

import logging
from dataclasses import dataclass, field
from pathlib import Path
from typing import List, Optional, Dict, Any

logger = logging.getLogger(__name__)


def _record_handoff_step(step: "StepResult") -> None:
    from bridge.run_context import record_event

    record_event(
        "handoff",
        step.name,
        ok=step.ok,
        detail="; ".join(step.messages[:5]),
    )


def _finish_handoff(
    steps: List["StepResult"],
    ok: bool,
    *,
    urdf_path: Optional[Path] = None,
    world_sdf: Optional[Path] = None,
    spawn_path: Optional[Path] = None,
) -> "HandoffResult":
    from bridge.run_context import record_lifecycle, record_path

    for step in steps:
        _record_handoff_step(step)
    if urdf_path is not None:
        record_path("export_urdf", urdf_path)
    if world_sdf is not None:
        record_path("world_sdf", world_sdf)
    if spawn_path is not None:
        record_path("spawn_urdf", spawn_path)
    record_lifecycle("handoff_finished", ok=ok)
    return HandoffResult(ok=ok, steps=steps)

# Prefix for machine-readable / test assertions when Gazebo is unreachable.
GAZEBO_NOT_RUNNING_PREFIX = "GAZEBO_NOT_RUNNING"

_GAZEBO_NOT_RUNNING_HINT = (
    f"{GAZEBO_NOT_RUNNING_PREFIX}: Gazebo simulation is not reachable. "
    "Start headless sim with Start-gz-sim.bat (WSL2/Docker), wait 2–5 s for the "
    "container, then retry. Live pytest: set RUN_GAZEBO_LIVE=1."
)


# ── URDF resolution ────────────────────────────────────────────────────────────

def resolve_robot_urdf(
    robot_name: str,
    *,
    robots_dir: Path,
    generated_dir: Path,
    skip_freecad_export: bool = False,
) -> tuple[Optional[Path], List[str], bool]:
    """
    Pick the best URDF for handoff/spawn.

    Priority:
      1. RobotCAD nested export: ``generated/<robot>/.../urdf/<robot>.urdf``
      2. Flat staged export: ``generated/<robot>/<robot>.urdf``
      3. When ``skip_freecad_export``: ``robots/<robot>.urdf`` placeholder
      4. When FCStd exists and export not skipped: signal ``needs_export=True``
      5. Placeholder only if no FCStd to export from

    Returns:
        ``(urdf_path, messages, needs_export)`` — ``urdf_path`` is None when
        export from FreeCAD is required next.
    """
    from bridge.freecad_bridge import expected_exported_urdf_path

    fcstd_path = robots_dir / f"{robot_name}.FCStd"
    if fcstd_path.is_file() and not skip_freecad_export:
        from bridge.export_cache import is_cache_enabled, try_restore_cached_export

        if is_cache_enabled():
            restored = try_restore_cached_export(
                robot_name,
                fcstd_path,
                generated_dir,
            )
            if restored is not None:
                try:
                    rel = restored.resolve().relative_to(generated_dir.resolve().parent)
                except ValueError:
                    rel = restored
                return (
                    restored,
                    [f"Using cached RobotCAD export: {rel}"],
                    False,
                )

    robotcad_urdf = expected_exported_urdf_path(robot_name, generated_dir)
    if robotcad_urdf.is_file():
        try:
            rel = robotcad_urdf.resolve().relative_to(generated_dir.resolve().parent)
        except ValueError:
            rel = robotcad_urdf
        return (
            robotcad_urdf,
            [f"Using RobotCAD-exported URDF: {rel}"],
            False,
        )

    flat_urdf = generated_dir / f"{robot_name}.urdf"
    if flat_urdf.is_file():
        try:
            rel_flat = flat_urdf.resolve().relative_to(generated_dir.resolve().parent)
        except ValueError:
            rel_flat = flat_urdf
        return (
            flat_urdf,
            [f"Using generated URDF: {rel_flat}"],
            False,
        )

    placeholder = robots_dir / f"{robot_name}.urdf"

    if skip_freecad_export:
        if placeholder.is_file():
            return (
                placeholder,
                [
                    f"Using hand-crafted URDF: {placeholder.relative_to(robots_dir.parent)} "
                    "(no RobotCAD export found under generated/)",
                ],
                False,
            )
        return (
            None,
            [
                f"No URDF under generated/{robot_name}/ and no placeholder at {placeholder}. "
                "Export with scripts/export_arm_2dof_fcstd.py or add robots/<robot>.urdf.",
            ],
            False,
        )

    if fcstd_path.is_file():
        return (
            None,
            [f"FCStd found at {fcstd_path}; RobotCAD export required."],
            True,
        )

    if placeholder.is_file():
        return (
            placeholder,
            [f"Using hand-crafted URDF: {placeholder.relative_to(robots_dir.parent)}"],
            False,
        )

    return (
        None,
        [
            f"No URDF found for {robot_name!r}: no generated export, no FCStd at {fcstd_path}, "
            f"no placeholder at {placeholder}."
        ],
        False,
    )


# ── Result ─────────────────────────────────────────────────────────────────────

@dataclass
class StepResult:
    name:     str
    ok:       bool
    messages: List[str] = field(default_factory=list)


@dataclass
class HandoffResult:
    ok:    bool
    steps: List[StepResult] = field(default_factory=list)

    def summary(self) -> str:
        lines = []
        for step in self.steps:
            icon = "✅" if step.ok else "❌"
            lines.append(f"{icon} {step.name}")
            for msg in step.messages:
                lines.append(f"   {msg}")
        lines.append("")
        lines.append("HANDOFF " + ("SUCCEEDED" if self.ok else "FAILED"))
        return "\n".join(lines)


# ── Main entry point ───────────────────────────────────────────────────────────

def export_and_spawn(
    robot_name: str,
    world_name: str = "empty_world",
    project_root: Optional[Path] = None,
    spawn_pose: Optional[Dict[str, Any]] = None,
    freecad_host: str = "localhost",
    freecad_port: int = 9875,
    gazebo_timeout: float = 30.0,
    skip_freecad_export: bool = False,
    spawn_urdf_path: Optional[Path] = None,
    skip_spawn: bool = False,
) -> HandoffResult:
    """
    Full pipeline: validate/export → stage world → wait for Gazebo → spawn.

    Args:
        robot_name:           Name of the robot (FCStd stem / URDF stem).
        world_name:           Name of the world SDF (without extension).
        project_root:         Path to repository root (defaults to auto-detected).
        spawn_pose:           Spawn pose dict for Gazebo. Default is origin.
        freecad_host:         FreeCAD RPC host.
        freecad_port:         FreeCAD RPC port.
        gazebo_timeout:       Timeout for Gazebo connection attempts.
        skip_freecad_export:  If True, never call FreeCAD export; prefer an existing
                              RobotCAD URDF under generated/, else robots/<robot>.urdf.
        spawn_urdf_path:      Optional alternate URDF for the Gazebo spawn step only
                              (validate/export still use the resolved URDF).
        skip_spawn:           If True, stop after gazebo_ready (for live tests that spawn separately).

    Returns:
        HandoffResult with per-step results and an overall ok flag.
    """
    from bridge.project import load_project
    from bridge.validate import validate_urdf, validate_sdf
    from bridge import freecad_bridge, gazebo_bridge
    from bridge.run_context import record_lifecycle, record_path

    record_lifecycle("handoff_start", robot=robot_name, world=world_name)
    steps: List[StepResult] = []
    urdf_path: Optional[Path] = None
    world_sdf: Optional[Path] = None
    spawn_path: Optional[Path] = None

    # ── 0. Load project ──────────────────────────────────────────────────────
    try:
        project = load_project(
            (project_root / "project.yaml") if project_root else None
        )
        root = project.paths.root
    except Exception as exc:
        steps.append(StepResult("load_project", ok=False, messages=[str(exc)]))
        return _finish_handoff(steps, ok=False)
    steps.append(StepResult("load_project", ok=True, messages=[f"Root: {root}"]))

    # ── 1. Resolve / export URDF ─────────────────────────────────────────────
    gen_dir    = project.paths.generated / robot_name
    fcstd_path = project.paths.robots / f"{robot_name}.FCStd"

    urdf_path, resolve_msgs, needs_export = resolve_robot_urdf(
        robot_name,
        robots_dir    = project.paths.robots,
        generated_dir = gen_dir,
        skip_freecad_export = skip_freecad_export,
    )

    if urdf_path is not None:
        steps.append(StepResult("resolve_urdf", ok=True, messages=resolve_msgs))
    elif needs_export and fcstd_path.is_file():
        export_result = freecad_bridge.export_urdf(
            robot_name  = robot_name,
            out_dir     = gen_dir,
            fcstd_path  = fcstd_path,
            host        = freecad_host,
            port        = freecad_port,
        )
        steps.append(StepResult(
            "export_urdf", ok=export_result.ok,
            messages=resolve_msgs + export_result.messages,
        ))
        if not export_result.ok:
            return _finish_handoff(steps, ok=False, urdf_path=urdf_path)
        urdf_path = export_result.path
        if urdf_path is None:
            urdf_path, fallback_msgs, _ = resolve_robot_urdf(
                robot_name,
                robots_dir    = project.paths.robots,
                generated_dir = gen_dir,
                skip_freecad_export = True,
            )
            if urdf_path is None:
                steps[-1] = StepResult(
                    "export_urdf", ok=False,
                    messages=export_result.messages + fallback_msgs,
                )
                return _finish_handoff(steps, ok=False, urdf_path=urdf_path)
            steps.append(StepResult("resolve_urdf", ok=True, messages=fallback_msgs))
    else:
        steps.append(StepResult("resolve_urdf", ok=False, messages=resolve_msgs))
        return _finish_handoff(steps, ok=False)

    # ── 2. Validate URDF ─────────────────────────────────────────────────────
    vr = validate_urdf(urdf_path)
    steps.append(StepResult(
        "validate_urdf",
        ok=vr.ok,
        messages=vr.errors + [f"WARN: {w}" for w in vr.warnings],
    ))
    if not vr.ok:
        return _finish_handoff(steps, ok=False, urdf_path=urdf_path)

    # ── 3. Stage world SDF ───────────────────────────────────────────────────
    world_gen_dir = project.paths.generated / world_name
    world_result  = freecad_bridge.export_sdf_world(
        world_name = world_name,
        out_dir    = world_gen_dir,
        source_dir = project.paths.worlds,
    )
    steps.append(StepResult(
        "stage_world",
        ok=world_result.ok,
        messages=world_result.messages,
    ))
    if not world_result.ok:
        return _finish_handoff(steps, ok=False, urdf_path=urdf_path)
    world_sdf = world_result.path

    # ── 4. Wait for Gazebo ───────────────────────────────────────────────────
    ready_result = gazebo_bridge.wait_for_ready(
        retries=8, delay=2.0, timeout=gazebo_timeout
    )
    if ready_result.ok and gazebo_bridge._gazebo_connected(ready_result.data) is False:
        ready_result = gazebo_bridge.GazeboResult(
            ok=False,
            data=ready_result.data,
            messages=ready_result.messages + [
                "Gazebo MCP reachable but simulation not connected.",
            ],
        )
    steps.append(StepResult(
        "gazebo_ready",
        ok=ready_result.ok,
        messages=ready_result.messages,
    ))
    if not ready_result.ok:
        steps[-1] = StepResult(
            "gazebo_ready",
            ok=False,
            messages=[_GAZEBO_NOT_RUNNING_HINT] + ready_result.messages,
        )
        return _finish_handoff(
            steps, ok=False, urdf_path=urdf_path, world_sdf=world_sdf
        )

    if skip_spawn:
        return _finish_handoff(
            steps,
            ok=all(s.ok for s in steps),
            urdf_path=urdf_path,
            world_sdf=world_sdf,
        )

    # ── 5. Spawn model ───────────────────────────────────────────────────────
    spawn_path = spawn_urdf_path or urdf_path
    spawn_msgs: List[str] = []
    if spawn_urdf_path is not None and spawn_urdf_path != urdf_path:
        spawn_msgs.append(f"Spawn URDF override: {spawn_urdf_path}")

    spawn_result = gazebo_bridge.spawn_model(
        model_name = robot_name,
        urdf_path  = spawn_path,
        pose       = spawn_pose or {"position": {"x": 0, "y": 0, "z": 0}},
        timeout    = gazebo_timeout,
    )
    steps.append(StepResult(
        "spawn_model",
        ok=spawn_result.ok,
        messages=spawn_msgs + spawn_result.messages,
    ))

    ok = all(s.ok for s in steps)
    return _finish_handoff(
        steps,
        ok=ok,
        urdf_path=urdf_path,
        world_sdf=world_sdf,
        spawn_path=spawn_path,
    )
