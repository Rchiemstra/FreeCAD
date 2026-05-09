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
        skip_freecad_export:  If True, use existing URDF from robots/ without
                              trying to export from FreeCAD. Useful when FreeCAD
                              is not running (hand-crafted URDF workflow).

    Returns:
        HandoffResult with per-step results and an overall ok flag.
    """
    from bridge.project import load_project
    from bridge.validate import validate_urdf, validate_sdf
    from bridge import freecad_bridge, gazebo_bridge

    steps: List[StepResult] = []

    # ── 0. Load project ──────────────────────────────────────────────────────
    try:
        project = load_project(
            (project_root / "project.yaml") if project_root else None
        )
        root = project.paths.root
    except Exception as exc:
        steps.append(StepResult("load_project", ok=False, messages=[str(exc)]))
        return HandoffResult(ok=False, steps=steps)
    steps.append(StepResult("load_project", ok=True, messages=[f"Root: {root}"]))

    # ── 1. Resolve / export URDF ─────────────────────────────────────────────
    urdf_path  = project.paths.robots / f"{robot_name}.urdf"
    fcstd_path = project.paths.robots / f"{robot_name}.FCStd"
    gen_dir    = project.paths.generated / robot_name

    if skip_freecad_export or not fcstd_path.exists():
        # Use hand-crafted URDF (Phase 1 workflow)
        if not urdf_path.exists():
            steps.append(StepResult(
                "resolve_urdf", ok=False,
                messages=[
                    f"No URDF found at {urdf_path} and no FCStd at {fcstd_path}. "
                    "Create robots/<robot>.urdf or open the FCStd in FreeCAD."
                ],
            ))
            return HandoffResult(ok=False, steps=steps)
        steps.append(StepResult(
            "resolve_urdf", ok=True,
            messages=[f"Using hand-crafted URDF: {urdf_path}"],
        ))
    else:
        # Export from FreeCAD via RobotCAD
        export_result = freecad_bridge.export_urdf(
            robot_name  = robot_name,
            out_dir     = gen_dir,
            fcstd_path  = fcstd_path,
            host        = freecad_host,
            port        = freecad_port,
        )
        steps.append(StepResult(
            "export_urdf", ok=export_result.ok,
            messages=export_result.messages,
        ))
        if not export_result.ok:
            return HandoffResult(ok=False, steps=steps)
        urdf_path = export_result.path or urdf_path

    # ── 2. Validate URDF ─────────────────────────────────────────────────────
    vr = validate_urdf(urdf_path)
    steps.append(StepResult(
        "validate_urdf",
        ok=vr.ok,
        messages=vr.errors + [f"WARN: {w}" for w in vr.warnings],
    ))
    if not vr.ok:
        return HandoffResult(ok=False, steps=steps)

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
        return HandoffResult(ok=False, steps=steps)
    world_sdf = world_result.path

    # ── 4. Wait for Gazebo ───────────────────────────────────────────────────
    ready_result = gazebo_bridge.wait_for_ready(
        retries=8, delay=2.0, timeout=gazebo_timeout
    )
    steps.append(StepResult(
        "gazebo_ready",
        ok=ready_result.ok,
        messages=ready_result.messages,
    ))
    if not ready_result.ok:
        return HandoffResult(ok=False, steps=steps)

    # ── 5. Spawn model ───────────────────────────────────────────────────────
    spawn_result = gazebo_bridge.spawn_model(
        model_name = robot_name,
        urdf_path  = urdf_path,
        pose       = spawn_pose or {"position": {"x": 0, "y": 0, "z": 0}},
        timeout    = gazebo_timeout,
    )
    steps.append(StepResult(
        "spawn_model",
        ok=spawn_result.ok,
        messages=spawn_result.messages,
    ))

    ok = all(s.ok for s in steps)
    return HandoffResult(ok=ok, steps=steps)
