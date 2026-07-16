"""Canonical normalization for numeric values, placements, paths, and expressions."""

from __future__ import annotations

import math
import re
from decimal import Decimal, InvalidOperation
from typing import Any

from .errors import InvalidSchemaError

# Length conversion factors to millimetres
_LENGTH_TO_MM = {
    "mm": Decimal("1"),
    "cm": Decimal("10"),
    "m": Decimal("1000"),
    "km": Decimal("1000000"),
    "in": Decimal("25.4"),
    "ft": Decimal("304.8"),
    "mi": Decimal("1609344"),
    "um": Decimal("0.001"),
    "µm": Decimal("0.001"),
    "nm": Decimal("0.000001"),
}

# Property type to default unit
_PROPERTY_UNITS: dict[str, tuple[str, str]] = {
    "App::PropertyLength": ("length", "mm"),
    "App::PropertyDistance": ("length", "mm"),
    "App::PropertyAngle": ("angle", "rad"),
    "App::PropertyQuantity": ("quantity", "mm"),
    "App::PropertyQuantityConstraint": ("quantity", "mm"),
}

_EXCLUDED_PROPERTY_PATTERNS = (
    re.compile(r"^ExternalLink.*Stamp$", re.I),
    re.compile(r"^ExternalLink.*Time", re.I),
    re.compile(r"^ExternalLink.*Modified", re.I),
    re.compile(r"^.*TimeStamp$", re.I),
    re.compile(r"^.*Timestamp$", re.I),
    re.compile(r"^.*ModifiedDate$", re.I),
    re.compile(r"^.*CreationDate$", re.I),
    re.compile(r"^.*LastModified", re.I),
)

_DEFAULT_EXCLUDED_PROPERTIES = frozenset(
    {
        "ExpressionEngine",
        "Visibility",
        "VisibilityList",
        "DisplayMode",
        "OnTopWhenSelected",
        "ShowInTree",
        "SelectionStyle",
        "ShapeMaterial",
        "DiffuseColor",
        "LineColor",
        "PointColor",
        "Transparency",
        "LineWidth",
        "PointSize",
        "DrawStyle",
        "Lighting",
        "Annotation",
        "BoundingBox",
        "ViewData",
        "Camera",
        "SavedCamera",
        "TreeViewState",
        "Expanded",
        "Selected",
        "Status",
        "Touch",
        "Touched",
        "Error",
        "Invalid",
        "Recompute",
        "Recomputing",
        "MustExecute",
        "Restore",
        "ID",
        "Uid",
        "UUID",
        "DocumentUUID",
        "LastModifiedDate",
        "CreationDate",
        "CreatedBy",
        "ModifiedDate",
        "ModifiedBy",
        "StringHasher",
        "Hasher",
        "Shape",
        "Mesh",
        "TopoShape",
        "Proxy",
        "ViewObject",
        "Icon",
        "Pixmap",
        "Thumbnail",
        "_PartShape",
        "ElementMap",
        "ElementMap2",
        "ElementMap3",
        "ElementMapFile",
        "PythonObject",
        "_Object",
        "ModifiedLink",
        "LinkStamp",
    }
)


def is_excluded_property(name: str, prop_type: str, extra_exclude: frozenset[str]) -> bool:
    """Return True if a property should be excluded from the semantic profile."""
    if name in _DEFAULT_EXCLUDED_PROPERTIES or name in extra_exclude:
        return True
    if prop_type in (
        "App::PropertyPythonObject",
        "App::PropertyPartShape",
        "App::PropertyMeshKernel",
        "App::PropertyComplexGeoData",
        "App::PropertyGeometryList",
        "App::PropertyStringHasher",
        "App::PropertyFile",
        "App::PropertyFileIncluded",
        "Mesh::PropertyMeshKernel",
        "Part::PropertyPartShape",
    ):
        return True
    for pattern in _EXCLUDED_PROPERTY_PATTERNS:
        if pattern.match(name):
            return True
    return False


def canonical_decimal(value: float | int | str | Decimal) -> str:
    """Convert a numeric value to canonical decimal string representation."""
    try:
        if isinstance(value, str):
            text = value.strip()
            if not text:
                raise InvalidSchemaError("empty numeric value")
            d = Decimal(text)
        elif isinstance(value, Decimal):
            d = value
        else:
            if isinstance(value, float) and (math.isnan(value) or math.isinf(value)):
                raise InvalidSchemaError(f"non-finite numeric value: {value}")
            d = Decimal(str(value))
    except (InvalidOperation, ValueError) as exc:
        raise InvalidSchemaError(f"invalid numeric value: {value}") from exc

    if not d.is_finite():
        raise InvalidSchemaError(f"non-finite numeric value: {value}")

    # Normalize negative zero
    if d == 0:
        d = Decimal("0")

    # Use normalized string without exponent when practical
    normalized = format(d.normalize(), "f")
    if "E" in normalized or "e" in normalized:
        normalized = format(d, "E").lower().replace("e+", "e")
        # Remove leading zeros in exponent
        parts = normalized.split("e")
        if len(parts) == 2:
            exp = parts[1].lstrip("+")
            if exp.startswith("-"):
                exp = "-" + exp[1:].lstrip("0") or "0"
            else:
                exp = exp.lstrip("0") or "0"
            normalized = f"{parts[0]}e{exp}"
    else:
        # Remove trailing zeros after decimal point
        if "." in normalized:
            normalized = normalized.rstrip("0").rstrip(".")
        if normalized == "-0":
            normalized = "0"

    return normalized


def normalize_expression(text: str) -> str:
    """Normalize expression newlines to LF only."""
    return text.replace("\r\n", "\n").replace("\r", "\n")


def normalize_path(path: str) -> str:
    """Normalize path separators to forward slashes, preserving case."""
    return path.replace("\\", "/")


def is_absolute_path(path: str) -> bool:
    """Detect absolute, drive-qualified, or UNC paths."""
    if not path:
        return False
    normalized = normalize_path(path)
    if normalized.startswith("/"):
        return True
    if re.match(r"^[A-Za-z]:/", normalized):
        return True
    if normalized.startswith("//"):
        return True
    return False


def safe_external_path(path: str, policy: str) -> dict[str, Any]:
    """Represent an external path according to policy without leaking absolute paths."""
    from .errors import UnsupportedDocumentError

    normalized = normalize_path(path)
    if is_absolute_path(path):
        if policy == "reject":
            raise UnsupportedDocumentError(f"absolute external path rejected: {normalized!r}")
        if policy == "hash":
            import hashlib

            digest = hashlib.sha256(normalized.encode("utf-8")).hexdigest()[:16]
            return {"path": f"<absolute:{digest}>", "relative": False, "absolute": True}
        return {"path": "<absolute>", "relative": False, "absolute": True}
    return {"path": normalized, "relative": True, "absolute": False}


def length_to_mm(value: float | str, unit: str = "mm") -> str:
    """Convert a length value to canonical millimetres string."""
    factor = _LENGTH_TO_MM.get(unit, Decimal("1"))
    d = Decimal(str(value)) * factor
    return canonical_decimal(d)


def normalize_quaternion(q0: float, q1: float, q2: float, q3: float) -> list[str]:
    """Normalize quaternion to x,y,z,w with positive w (or lexicographic tie-break)."""
    # FreeCAD uses Q0,Q1,Q2,Q3 as x,y,z,w
    x, y, z, w = float(q0), float(q1), float(q2), float(q3)

    # Normalize magnitude
    mag = math.sqrt(x * x + y * y + z * z + w * w)
    if mag == 0:
        return ["0", "0", "0", "1"]
    x, y, z, w = x / mag, y / mag, z / mag, w / mag

    # Choose positive w, or lexicographically positive when w is zero
    if w < 0 or (w == 0 and (x < 0 or (x == 0 and (y < 0 or (y == 0 and z < 0))))):
        x, y, z, w = -x, -y, -z, -w

    return [
        canonical_decimal(x),
        canonical_decimal(y),
        canonical_decimal(z),
        canonical_decimal(w),
    ]


def placement_from_attributes(attrs: dict[str, str]) -> dict[str, Any]:
    """Build canonical placement from PropertyPlacement XML attributes."""
    px = attrs.get("Px", "0")
    py = attrs.get("Py", "0")
    pz = attrs.get("Pz", "0")

    if "Q0" in attrs:
        quat = normalize_quaternion(
            float(attrs.get("Q0", "0")),
            float(attrs.get("Q1", "0")),
            float(attrs.get("Q2", "0")),
            float(attrs.get("Q3", "1")),
        )
    elif "A" in attrs:
        # Convert axis-angle to quaternion
        ox = float(attrs.get("Ox", "0"))
        oy = float(attrs.get("Oy", "0"))
        oz = float(attrs.get("Oz", "1"))
        angle = float(attrs.get("A", "0"))
        mag = math.sqrt(ox * ox + oy * oy + oz * oz)
        if mag == 0:
            quat = ["0", "0", "0", "1"]
        else:
            ox, oy, oz = ox / mag, oy / mag, oz / mag
            half = angle / 2
            s = math.sin(half)
            quat = normalize_quaternion(ox * s, oy * s, oz * s, math.cos(half))
    else:
        quat = ["0", "0", "0", "1"]

    return {
        "position_mm": [length_to_mm(px), length_to_mm(py), length_to_mm(pz)],
        "rotation_xyzw": quat,
    }


def quantity_from_float(value: str, prop_type: str) -> dict[str, Any]:
    """Build a quantity dict from a float value and property type."""
    unit_info = _PROPERTY_UNITS.get(prop_type, ("scalar", ""))
    kind, default_unit = unit_info

    if kind == "length":
        return {"type": prop_type, "value": length_to_mm(value), "unit": "mm"}
    if kind == "angle":
        return {"type": prop_type, "value": canonical_decimal(value), "unit": "rad"}
    return {"type": prop_type, "value": canonical_decimal(value), "unit": default_unit}
