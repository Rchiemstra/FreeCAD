"""
bridge.validate — Local URDF and SDF validation (no FreeCAD/Gazebo needed).

Checks that are always run before export or spawn:
  - XML well-formed
  - Correct root element (<robot> for URDF, <sdf> for SDF)
  - All link/joint names are valid identifiers
  - Inertia values are non-zero for non-fixed links
  - Joint axes are unit vectors (within tolerance)
  - Mesh paths (if present) use relative, not absolute, paths

These checks encode the Phase 1 friction list items #1–#5 and give fast
feedback without requiring a Gazebo instance.

Usage:
    from bridge.validate import validate_urdf, validate_sdf

    result = validate_urdf(Path("robots/arm_2dof.urdf"))
    if not result.ok:
        for issue in result.errors:
            print("ERROR:", issue)
    for w in result.warnings:
        print("WARN:", w)
"""

from __future__ import annotations

import math
import re
import xml.etree.ElementTree as ET
from dataclasses import dataclass, field
from pathlib import Path
from typing import List


# ── Result ─────────────────────────────────────────────────────────────────────

@dataclass
class ValidationResult:
    ok:       bool
    errors:   List[str] = field(default_factory=list)
    warnings: List[str] = field(default_factory=list)

    def __bool__(self) -> bool:
        return self.ok

    def summary(self) -> str:
        parts = [f"{len(self.errors)} error(s), {len(self.warnings)} warning(s)"]
        for e in self.errors:
            parts.append(f"  ERROR: {e}")
        for w in self.warnings:
            parts.append(f"  WARN:  {w}")
        return "\n".join(parts)


# ── Helpers ────────────────────────────────────────────────────────────────────

_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_\-\.]*$")


def _valid_ident(name: str) -> bool:
    return bool(_IDENT_RE.match(name)) if name else False


def _parse_floats(text: str) -> list[float]:
    try:
        return [float(x) for x in text.strip().split()]
    except (ValueError, AttributeError):
        return []


def _vec3_is_unit(xyz_text: str, tol: float = 1e-3) -> bool:
    vals = _parse_floats(xyz_text)
    if len(vals) != 3:
        return False
    mag = math.sqrt(sum(v * v for v in vals))
    return abs(mag - 1.0) < tol


def _inertia_nonzero(inertial_el: ET.Element) -> bool:
    """Return True if any principal inertia (Ixx, Iyy, Izz) is non-zero."""
    inertia = inertial_el.find("inertia")
    if inertia is None:
        return False
    for attr in ("ixx", "iyy", "izz"):
        val_str = inertia.get(attr, "0")
        try:
            if abs(float(val_str)) > 1e-12:
                return True
        except ValueError:
            pass
    return False


# ── URDF validator ─────────────────────────────────────────────────────────────

def validate_urdf(path: Path) -> ValidationResult:
    """
    Validate a URDF file without a Gazebo instance.

    Checks:
      1. XML parses without error.
      2. Root element is <robot>.
      3. Robot has a name attribute.
      4. All link names are valid identifiers.
      5. All joint names are valid identifiers.
      6. All joint types are recognised.
      7. Non-fixed links with <inertial> have non-zero principal inertia.
      8. Non-fixed links without <inertial> are warned about.
      9. Joint <axis xyz="…"> vectors are unit vectors.
     10. Mesh <filename> values are relative paths (not absolute).

    Args:
        path: Path to the .urdf file.

    Returns:
        ValidationResult with errors and warnings lists.
    """
    errors:   List[str] = []
    warnings: List[str] = []

    path = Path(path)
    if not path.exists():
        return ValidationResult(ok=False, errors=[f"File not found: {path}"])

    # 1. XML parse
    try:
        tree = ET.parse(str(path))
        root = tree.getroot()
    except ET.ParseError as exc:
        return ValidationResult(ok=False, errors=[f"XML parse error: {exc}"])

    # 2. Root element
    if root.tag != "robot":
        errors.append(f"Root element must be <robot>, got <{root.tag}>")
        return ValidationResult(ok=False, errors=errors)

    # 3. Robot name
    if not root.get("name"):
        warnings.append("Robot has no 'name' attribute")

    # 4 & 5. Link and joint names
    link_names = set()
    for link in root.findall("link"):
        name = link.get("name", "")
        if not _valid_ident(name):
            errors.append(f"Link has invalid/missing name: '{name}'")
        link_names.add(name)

    joint_names = set()
    for joint in root.findall("joint"):
        name = joint.get("name", "")
        if not _valid_ident(name):
            errors.append(f"Joint has invalid/missing name: '{name}'")
        joint_names.add(name)

    # 6. Joint types
    VALID_JOINT_TYPES = {
        "revolute", "prismatic", "continuous", "fixed", "floating", "planar"
    }
    for joint in root.findall("joint"):
        jtype = joint.get("type", "")
        if jtype not in VALID_JOINT_TYPES:
            errors.append(
                f"Joint '{joint.get('name', '?')}' has unknown type '{jtype}'. "
                f"Expected one of: {sorted(VALID_JOINT_TYPES)}"
            )

    # 7 & 8. Inertial checks for non-fixed links
    joint_children = {
        j.find("child").get("link", "") if j.find("child") is not None else ""
        for j in root.findall("joint")
        if j.get("type", "") != "fixed"
    }
    for link in root.findall("link"):
        link_name = link.get("name", "")
        if link_name not in joint_children:
            continue  # base link or truly fixed — OK
        inertial = link.find("inertial")
        if inertial is None:
            warnings.append(
                f"Link '{link_name}' is a non-fixed child but has no <inertial>. "
                "This will cause simulation instability."
            )
        elif not _inertia_nonzero(inertial):
            errors.append(
                f"Link '{link_name}' has zero principal inertia (ixx=iyy=izz≈0). "
                "Assign material density in FreeCAD before export (friction point #3)."
            )

    # 9. Joint axis unit vectors
    for joint in root.findall("joint"):
        if joint.get("type", "") == "fixed":
            continue
        axis = joint.find("axis")
        if axis is None:
            # Default URDF axis is (1 0 0), which is fine — no warning needed.
            continue
        xyz = axis.get("xyz", "")
        if not _vec3_is_unit(xyz):
            errors.append(
                f"Joint '{joint.get('name', '?')}' axis xyz='{xyz}' is not a unit vector "
                "(friction point #2). Check frame conventions after RobotCAD export."
            )

    # 10. Mesh path portability
    for mesh in root.iter("mesh"):
        filename = mesh.get("filename", "")
        if not filename:
            continue
        if filename.startswith("/") or (len(filename) > 1 and filename[1] == ":"):
            errors.append(
                f"Mesh filename '{filename}' is absolute — this breaks portability "
                "(friction point #5). Use relative paths or package:// URIs."
            )

    ok = len(errors) == 0
    return ValidationResult(ok=ok, errors=errors, warnings=warnings)


# ── SDF validator ──────────────────────────────────────────────────────────────

def validate_sdf(path: Path) -> ValidationResult:
    """
    Validate an SDF world file without a Gazebo instance.

    Checks:
      1. XML parses without error.
      2. Root element is <sdf>.
      3. SDF version attribute present.
      4. Contains at least one <world> element.
      5. Each <world> has a name attribute.
      6. Contains physics settings (<physics>).
      7. Mesh paths (if present) are relative.

    Args:
        path: Path to the .sdf file.

    Returns:
        ValidationResult with errors and warnings lists.
    """
    errors:   List[str] = []
    warnings: List[str] = []

    path = Path(path)
    if not path.exists():
        return ValidationResult(ok=False, errors=[f"File not found: {path}"])

    try:
        tree = ET.parse(str(path))
        root = tree.getroot()
    except ET.ParseError as exc:
        return ValidationResult(ok=False, errors=[f"XML parse error: {exc}"])

    if root.tag != "sdf":
        errors.append(f"Root element must be <sdf>, got <{root.tag}>")
        return ValidationResult(ok=False, errors=errors)

    if not root.get("version"):
        warnings.append("SDF has no version attribute")

    worlds = root.findall("world")
    if not worlds:
        errors.append("SDF file contains no <world> element")
    for world in worlds:
        if not world.get("name"):
            warnings.append("<world> element has no name attribute")
        if world.find("physics") is None:
            warnings.append(
                f"World '{world.get('name', '?')}' has no <physics> element — "
                "Gazebo will use defaults (may differ from expected 1 ms step)."
            )

    # Mesh path portability (same as URDF check)
    for mesh in root.iter("mesh"):
        uri = mesh.find("uri")
        if uri is not None and uri.text:
            text = uri.text.strip()
            if text.startswith("/") or (len(text) > 1 and text[1] == ":"):
                errors.append(
                    f"Mesh URI '{text}' is absolute — use relative paths or model:// URIs."
                )

    ok = len(errors) == 0
    return ValidationResult(ok=ok, errors=errors, warnings=warnings)
