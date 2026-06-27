"""
bridge — FreeCAD/Gazebo automation bridge for the robot simulation test rig.

Provides Python API for:
  - Exporting URDF/SDF from FreeCAD (via XML-RPC to FreeCAD MCP addon)
  - Spawning models in headless Gazebo (via MCP stdio to gazebo-mcp)
  - Validating URDF/SDF files locally
  - Loading project configuration (project.yaml)
  - High-level handoff: export + spawn in one call

Quick start:
    from bridge import handoff, validate_urdf, load_project

    project = load_project()
    result = validate_urdf(project.paths.robots / "arm_2dof.urdf")
    handoff.export_and_spawn("arm_2dof", "empty_world")
"""

from bridge.project import load_project, ProjectConfig
from bridge.validate import validate_urdf, validate_sdf
from bridge.permissions import (
    PermissionDenied,
    WriteOperation,
    assert_write_allowed,
    effective_write_policy,
    list_write_capabilities,
)
from bridge import freecad_bridge, gazebo_bridge, handoff

__all__ = [
    "load_project",
    "ProjectConfig",
    "validate_urdf",
    "validate_sdf",
    "PermissionDenied",
    "WriteOperation",
    "assert_write_allowed",
    "effective_write_policy",
    "list_write_capabilities",
    "freecad_bridge",
    "gazebo_bridge",
    "handoff",
]
