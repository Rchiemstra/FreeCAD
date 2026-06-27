"""
bridge.project — Load and resolve project.yaml configuration.

The project.yaml file lives at the repository root and defines:
  - Path layout (robots/, worlds/, generated/, tests/, sim_runs/, config/)
  - MCP server configuration
  - Environment / version pins

Usage:
    from bridge.project import load_project

    p = load_project()
    urdf = p.paths.robots / "arm_2dof.urdf"
    out  = p.paths.generated / "arm_2dof"
"""

from __future__ import annotations

import os
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

try:
    import yaml  # PyYAML
    _YAML_AVAILABLE = True
except ImportError:
    _YAML_AVAILABLE = False


# ── Public dataclasses ─────────────────────────────────────────────────────────

@dataclass
class PathLayout:
    root:      Path
    robots:    Path
    worlds:    Path
    generated: Path
    tests:     Path
    sim_runs:  Path
    config:    Path


@dataclass
class MCPEndpoint:
    server_package: str
    server_version: str
    transport:      Optional[str] = None
    host:           Optional[str] = None
    port:           Optional[int] = None
    requires_ros2:  bool          = False


@dataclass
class MCPConfig:
    freecad: MCPEndpoint
    gazebo:  MCPEndpoint
    ros:     MCPEndpoint


@dataclass
class ProjectConfig:
    name:        str
    version:     str
    paths:       PathLayout
    mcp:         MCPConfig
    source_file: Path = field(default_factory=Path)


# ── Loader ────────────────────────────────────────────────────────────────────

def _find_project_yaml() -> Path:
    """Walk up from cwd to find project.yaml."""
    start = Path(os.getcwd()).resolve()
    for candidate in [start, *start.parents]:
        p = candidate / "project.yaml"
        if p.exists():
            return p
    raise FileNotFoundError(
        "project.yaml not found in current directory or any parent. "
        "Run from within the FreeCAD robot sim repository."
    )


def load_project(path: Optional[Path] = None) -> ProjectConfig:
    """
    Load project.yaml and return a ProjectConfig.

    Args:
        path: Explicit path to project.yaml. If None, searches upward from cwd.

    Returns:
        ProjectConfig with resolved Path objects.

    Raises:
        FileNotFoundError: If project.yaml cannot be found.
        ImportError: If PyYAML is not installed.
        ValueError: If project.yaml is malformed.
    """
    if not _YAML_AVAILABLE:
        raise ImportError(
            "PyYAML is required to load project.yaml. "
            "Install it with: pip install pyyaml"
        )

    yaml_path = Path(path) if path else _find_project_yaml()
    root = yaml_path.parent

    with yaml_path.open("r", encoding="utf-8") as fh:
        data = yaml.safe_load(fh)

    if not isinstance(data, dict):
        raise ValueError(f"project.yaml must be a YAML mapping, got {type(data)}")

    from bridge.schema_validate import SchemaValidationError, validate_instance

    try:
        validate_instance(data, "project", instance_label=str(yaml_path))
    except SchemaValidationError as exc:
        raise ValueError(str(exc)) from exc

    # ── Paths ──────────────────────────────────────────────────────────────────
    raw_paths = data.get("paths", {})
    paths = PathLayout(
        root      = root,
        robots    = root / raw_paths.get("robots",    "robots"),
        worlds    = root / raw_paths.get("worlds",    "worlds"),
        generated = root / raw_paths.get("generated", "generated"),
        tests     = root / raw_paths.get("tests",     "tests"),
        sim_runs  = root / raw_paths.get("sim_runs",  "sim_runs"),
        config    = root / raw_paths.get("config",    "config"),
    )

    # ── MCP config ─────────────────────────────────────────────────────────────
    raw_mcp = data.get("mcp", {})

    def _ep(key: str) -> MCPEndpoint:
        d = raw_mcp.get(key, {})
        return MCPEndpoint(
            server_package = d.get("server_package", ""),
            server_version = d.get("server_version", ""),
            transport      = d.get("transport"),
            host           = d.get("host"),
            port           = d.get("port"),
            requires_ros2  = bool(d.get("requires_ros2", False)),
        )

    mcp = MCPConfig(
        freecad = _ep("freecad"),
        gazebo  = _ep("gazebo"),
        ros     = _ep("ros"),
    )

    return ProjectConfig(
        name        = data.get("name", "freecad-gazebo-mcp"),
        version     = str(data.get("version", "0.1")),
        paths       = paths,
        mcp         = mcp,
        source_file = yaml_path,
    )
