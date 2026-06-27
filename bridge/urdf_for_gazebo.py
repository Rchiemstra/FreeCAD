"""
Prepare RobotCAD URDF XML for Gazebo spawn (package:// paths and collision policy).

Collision / mesh policy (Phase 6):
  RobotCAD exports ``meshes/col_end_effector_.dae`` for the end-effector *collision*
  mesh. ODE (Gazebo Bullet) often aborts on this trimesh (``vertices`` assertion) even
  when spawn succeeds. **Visual** meshes are unchanged.

  Default (``GAZEBO_COLLISION_MESH_POLICY=replace_end_effector_mesh``):
  - Any ``<collision>`` whose ``<mesh filename=...>`` contains ``col_end_effector``
    is rewritten to a **sphere** (radius 0.025 m, matches placeholder visual).
  - Set policy to ``keep`` to disable simplification (debug only).
"""

from __future__ import annotations

import os
import re
import xml.etree.ElementTree as ET
from pathlib import Path
from typing import List, Optional, Tuple

_PKG_URI_RE = re.compile(
    r'package://arm_2dof_description/([^"\']+)',
    re.IGNORECASE,
)

# Mesh filename substrings → sphere radius (metres) for collision replacement.
_DEFAULT_MESH_COLLISION_REPLACEMENTS: Tuple[Tuple[str, float], ...] = (
    ("col_end_effector", 0.025),
)

_COLLISION_POLICY_ENV = "GAZEBO_COLLISION_MESH_POLICY"


def collision_mesh_policy() -> str:
    """``replace_end_effector_mesh`` (default), ``keep``, or ``all_mesh_to_sphere``."""
    return os.environ.get(_COLLISION_POLICY_ENV, "replace_end_effector_mesh").strip().lower()


def robotcad_description_root(urdf_path: Path) -> Optional[Path]:
    """
    Return the ROS package root containing ``package.xml`` for a RobotCAD URDF.

    Typical layout: ``.../arm_2dof_description/arm_2dof_description/{urdf,meshes,package.xml}``.
    """
    path = Path(urdf_path).resolve()
    for parent in [path.parent, *path.parents]:
        if (parent / "package.xml").is_file():
            return parent
    return None


def _mesh_filename(mesh_elem: ET.Element) -> str:
    return (mesh_elem.get("filename") or mesh_elem.get("uri") or "").lower()


def _replace_collision_mesh_with_sphere(
    collision: ET.Element,
    radius: float,
) -> bool:
    geom = collision.find("geometry")
    if geom is None:
        return False
    mesh = geom.find("mesh")
    if mesh is None:
        return False
    for child in list(geom):
        geom.remove(child)
    sphere = ET.SubElement(geom, "sphere")
    sphere.set("radius", f"{radius:.6g}".rstrip("0").rstrip("."))
    return True


def simplify_collision_meshes_for_gazebo(
    urdf_xml: str,
    *,
    replacements: Optional[List[Tuple[str, float]]] = None,
) -> Tuple[str, int]:
    """
    Replace problematic collision trimeshes with primitives.

    Returns:
        ``(modified_xml, count_replaced)``
    """
    policy = collision_mesh_policy()
    if policy in ("keep", "none", "0", "false"):
        return urdf_xml, 0

    rules = list(replacements or _DEFAULT_MESH_COLLISION_REPLACEMENTS)
    if policy == "all_mesh_to_sphere":
        rules = [("", 0.025)]

    try:
        root = ET.fromstring(urdf_xml)
    except ET.ParseError:
        return urdf_xml, 0

    replaced = 0
    for collision in root.iter("collision"):
        geom = collision.find("geometry")
        if geom is None:
            continue
        mesh = geom.find("mesh")
        if mesh is None:
            continue
        fn = _mesh_filename(mesh)
        if policy == "all_mesh_to_sphere":
            if _replace_collision_mesh_with_sphere(collision, 0.025):
                replaced += 1
            continue
        for needle, radius in rules:
            if needle.lower() in fn:
                if _replace_collision_mesh_with_sphere(collision, radius):
                    replaced += 1
                break

    if replaced == 0:
        return urdf_xml, 0

    return ET.tostring(root, encoding="unicode"), replaced


def prepare_urdf_for_gazebo(
    urdf_xml: str,
    urdf_path: Path,
    *,
    container_pkg_root: str = "/models/arm_2dof_description",
) -> str:
    """
    Rewrite ``package://`` paths and apply collision mesh policy for Gazebo spawn.
    """
    root = robotcad_description_root(urdf_path)
    out = urdf_xml
    if root is not None:
        use_container = os.environ.get("GAZEBO_URDF_USE_CONTAINER_PATHS", "1").strip().lower() in (
            "1",
            "true",
            "yes",
        )
        prefix = container_pkg_root if use_container else root.as_posix()

        def _replace(match: re.Match[str]) -> str:
            rel = match.group(1).lstrip("/")
            return f"file://{prefix}/{rel}"

        out = _PKG_URI_RE.sub(_replace, out)

    out, _ = simplify_collision_meshes_for_gazebo(out)
    return out


def prepare_urdf_file(urdf_path: Path) -> str:
    """Read URDF from disk and apply :func:`prepare_urdf_for_gazebo`."""
    text = Path(urdf_path).read_text(encoding="utf-8")
    return prepare_urdf_for_gazebo(text, urdf_path)
