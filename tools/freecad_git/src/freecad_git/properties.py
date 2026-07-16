"""Property XML element parsing into semantic values."""

from __future__ import annotations

from typing import Any

from .config import CollectionLimits
from .errors import InvalidXmlError, UnsupportedDocumentError
from .normalize import (
    canonical_decimal,
    is_excluded_property,
    normalize_expression,
    placement_from_attributes,
    quantity_from_float,
    safe_external_path,
)

_LINK_TYPES = frozenset(
    {
        "App::PropertyLink",
        "App::PropertyLinkChild",
        "App::PropertyLinkGlobal",
        "App::PropertyLinkHidden",
    }
)
_LINK_LIST_TYPES = frozenset(
    {
        "App::PropertyLinkList",
        "App::PropertyLinkListChild",
        "App::PropertyLinkListGlobal",
        "App::PropertyLinkListHidden",
        "App::PropertyLinkListHidden",
    }
)
_LINK_SUB_TYPES = frozenset(
    {
        "App::PropertyLinkSub",
        "App::PropertyLinkSubChild",
        "App::PropertyLinkSubGlobal",
        "App::PropertyLinkSubHidden",
    }
)
_LINK_SUB_LIST_TYPES = frozenset(
    {
        "App::PropertyLinkSubList",
        "App::PropertyLinkSubListChild",
        "App::PropertyLinkSubListGlobal",
        "App::PropertyLinkSubListHidden",
    }
)
_GEOMETRY_LIST_TYPES = frozenset(
    {
        "Sketcher::PropertyGeometryList",
        "Part::PropertyGeometryList",
    }
)


def _child(elements: list[dict[str, Any]], tag: str) -> dict[str, Any] | None:
    for elem in elements:
        if elem["tag"] == tag:
            return elem
    return None


def _children(elements: list[dict[str, Any]], tag: str) -> list[dict[str, Any]]:
    return [elem for elem in elements if elem["tag"] == tag]


def _parse_link_list_container(container: dict[str, Any]) -> list[str]:
    links: list[str] = []
    for link in _children(container.get("children", []), "Link"):
        val = link["attrs"].get("value", "")
        if val:
            links.append(val)
    return links


def _parse_link_sub(container: dict[str, Any]) -> dict[str, Any]:
    obj = container["attrs"].get("value", "")
    subs: list[str] = []
    for sub in _children(container.get("children", []), "Sub"):
        subs.append(sub["attrs"].get("value", ""))
    return {"object": obj, "subelements": subs}


def _parse_link_sub_list(container: dict[str, Any]) -> list[dict[str, str]]:
    result: list[dict[str, str]] = []
    for link in _children(container.get("children", []), "Link"):
        result.append(
            {
                "object": link["attrs"].get("obj", link["attrs"].get("value", "")),
                "subelement": link["attrs"].get("sub", ""),
            }
        )
    return result


def _parse_constraint_list(container: dict[str, Any], limits: CollectionLimits) -> list[dict[str, Any]]:
    constraints: list[dict[str, Any]] = []
    for con in _children(container.get("children", []), "Constrain"):
        if len(constraints) >= limits.max_list_length:
            raise UnsupportedDocumentError("constraint count exceeds limit")
        entry: dict[str, Any] = {"type": con["attrs"].get("Type", "")}
        for key, val in con["attrs"].items():
            if key == "Type":
                continue
            if key == "Value":
                entry["value"] = canonical_decimal(val)
            elif key in ("IsDriving", "IsVisible", "IsActive", "IsInVirtualSpace"):
                entry[key.lower()] = val == "1"
            elif key in ("Name", "MetaData") and val:
                entry[key.lower()] = val
            elif key in (
                "First",
                "Second",
                "Third",
                "FirstPos",
                "SecondPos",
                "ThirdPos",
                "ElementIds",
                "ElementPositions",
                "Orientation",
                "LabelDistance",
                "LabelPosition",
            ):
                entry[key.lower()] = val
        constraints.append(entry)
    return constraints


def _summarize_geometry_primitive(geom: dict[str, Any]) -> dict[str, Any]:
    """Build compact semantic summary for one geometry entry."""
    summary: dict[str, Any] = {
        "type": geom["attrs"].get("type", ""),
    }
    if geom["attrs"].get("id"):
        summary["id"] = geom["attrs"]["id"]

    for child in geom.get("children", []):
        tag = child["tag"]
        attrs = child.get("attrs", {})
        if tag == "Construction":
            summary["construction"] = attrs.get("value", "0") == "1"
        elif tag in ("LineSegment", "ArcOfCircle", "Circle", "Point", "Ellipse"):
            primitive: dict[str, Any] = {"kind": tag}
            for key, val in attrs.items():
                if key in (
                    "StartX",
                    "StartY",
                    "StartZ",
                    "EndX",
                    "EndY",
                    "EndZ",
                    "CenterX",
                    "CenterY",
                    "CenterZ",
                    "Radius",
                    "StartAngle",
                    "EndAngle",
                ):
                    primitive[key] = canonical_decimal(val)
                else:
                    primitive[key] = val
            summary["primitive"] = primitive

    return summary


def _parse_geometry_list(container: dict[str, Any], limits: CollectionLimits) -> list[dict[str, Any]]:
    geometries: list[dict[str, Any]] = []
    for geo in _children(container.get("children", []), "Geometry"):
        if len(geometries) >= limits.max_list_length:
            raise UnsupportedDocumentError("geometry count exceeds limit")
        geometries.append(_summarize_geometry_primitive(geo))
    return geometries


def _parse_expression_engine(container: dict[str, Any]) -> dict[str, str]:
    expressions: dict[str, str] = {}
    for expr in _children(container.get("children", []), "Expression"):
        path = expr["attrs"].get("path", "")
        text = expr.get("text", "") or expr["attrs"].get("value", "")
        if path:
            expressions[path] = normalize_expression(text)
    return expressions


def _parse_spreadsheet_cells(container: dict[str, Any], limits: CollectionLimits) -> dict[str, Any]:
    cells: dict[str, Any] = {}
    for cell in _children(container.get("children", []), "Cell"):
        if len(cells) >= limits.max_list_length:
            raise UnsupportedDocumentError("spreadsheet cell count exceeds limit")
        address = cell["attrs"].get("address", "")
        if not address:
            continue
        entry: dict[str, Any] = {}
        if "content" in cell["attrs"]:
            entry["content"] = cell["attrs"]["content"]
        if "alias" in cell["attrs"]:
            entry["alias"] = cell["attrs"]["alias"]
        if "expression" in cell["attrs"]:
            entry["expression"] = normalize_expression(cell["attrs"]["expression"])
        if entry:
            cells[address] = entry
    return cells


def _typed_fallback(elements: list[dict[str, Any]], prop_type: str, limits: CollectionLimits) -> dict[str, Any]:
    fallback: dict[str, Any] = {"type": prop_type, "xml": []}
    for elem in elements:
        entry: dict[str, Any] = {"tag": elem["tag"]}
        if elem["attrs"]:
            entry["attrs"] = dict(elem["attrs"])
        if elem.get("text"):
            text = elem["text"]
            entry["text"] = text[: limits.max_string_length] + "..." if len(text) > limits.max_string_length else text
        if elem.get("children"):
            entry["children"] = [
                {"tag": c["tag"], "attrs": dict(c.get("attrs", {}))} for c in elem["children"][:20]
            ]
        fallback["xml"].append(entry)
    return fallback


def _parse_property_value(
    prop_name: str,
    prop_type: str,
    elements: list[dict[str, Any]],
    limits: CollectionLimits,
    external_policy: str,
) -> Any:
    """Parse a property's XML elements using FreeCAD property type dispatch."""

    if prop_type in _LINK_LIST_TYPES or _child(elements, "LinkList") is not None:
        container = _child(elements, "LinkList")
        if container is not None:
            return {"type": "link_list", "targets": _parse_link_list_container(container)}
        return None

    if prop_type in _LINK_SUB_LIST_TYPES or _child(elements, "LinkSubList") is not None:
        container = _child(elements, "LinkSubList")
        if container is not None:
            return {"type": "link_sub_list", "targets": _parse_link_sub_list(container)}
        return None

    if prop_type in _LINK_SUB_TYPES or _child(elements, "LinkSub") is not None:
        container = _child(elements, "LinkSub")
        if container is not None:
            parsed = _parse_link_sub(container)
            if parsed["subelements"]:
                return {
                    "type": "link_sub",
                    "object": parsed["object"],
                    "subelement": parsed["subelements"][0],
                    "subelements": parsed["subelements"],
                }
            return {"type": "link", "target": parsed["object"]}
        return None

    if prop_type in _LINK_TYPES or _child(elements, "Link") is not None:
        container = _child(elements, "Link")
        if container is not None:
            val = container["attrs"].get("value", "")
            return {"type": "link", "target": val} if val else None
        return None

    if prop_type == "Sketcher::PropertyConstraintList" or _child(elements, "ConstraintList") is not None:
        container = _child(elements, "ConstraintList")
        if container is not None:
            return {"constraints": _parse_constraint_list(container, limits)}
        return None

    if prop_type in _GEOMETRY_LIST_TYPES or _child(elements, "GeometryList") is not None:
        container = _child(elements, "GeometryList")
        if container is not None:
            return {"geometries": _parse_geometry_list(container, limits)}
        return None

    placement = _child(elements, "PropertyPlacement")
    if placement is not None:
        return placement_from_attributes(placement["attrs"])

    xlink = _child(elements, "XLink")
    if xlink is not None:
        path = xlink["attrs"].get("file", "")
        safe = safe_external_path(path, external_policy)
        return {
            "type": "external_link",
            **safe,
            "document": xlink["attrs"].get("doc", ""),
            "object": xlink["attrs"].get("obj", ""),
            "subelement": xlink["attrs"].get("sub", ""),
        }

    if prop_type == "App::PropertyExpressionEngine" or _child(elements, "ExpressionEngine") is not None:
        container = _child(elements, "ExpressionEngine")
        if container is not None:
            exprs = _parse_expression_engine(container)
            if exprs:
                return {"type": prop_type, "expressions": exprs}
        return None

    if prop_type == "Spreadsheet::PropertySheet" or _child(elements, "Cells") is not None:
        container = _child(elements, "Cells")
        if container is not None:
            return {"cells": _parse_spreadsheet_cells(container, limits)}
        return None

    string_elem = _child(elements, "String")
    if string_elem is not None and prop_type in ("App::PropertyString", "App::PropertyFile", ""):
        return string_elem["attrs"].get("value", string_elem.get("text", ""))

    bool_elem = _child(elements, "Bool")
    if bool_elem is not None:
        return bool_elem["attrs"].get("value", "true").lower() == "true"

    float_elem = _child(elements, "Float")
    if float_elem is not None:
        if prop_type in (
            "App::PropertyLength",
            "App::PropertyDistance",
            "App::PropertyQuantity",
            "App::PropertyQuantityConstraint",
            "App::PropertyAngle",
        ):
            return quantity_from_float(float_elem["attrs"].get("value", "0"), prop_type)
        return {"type": prop_type, "value": canonical_decimal(float_elem["attrs"].get("value", "0"))}

    int_elem = _child(elements, "Integer")
    if int_elem is not None:
        return {"type": prop_type, "value": canonical_decimal(int_elem["attrs"].get("value", "0"))}

    enum_elem = _child(elements, "Enumeration")
    if enum_elem is not None:
        return {"type": prop_type, "value": enum_elem["attrs"].get("value", "")}

    if elements:
        return _typed_fallback(elements, prop_type, limits)
    return None


def _parse_status(status: str | None) -> int:
    if status is None:
        return 0
    try:
        return int(status)
    except ValueError as exc:
        raise InvalidXmlError(f"invalid property status value: {status!r}") from exc


def parse_object_properties(
    properties: list[dict[str, Any]],
    obj_type: str,
    limits: CollectionLimits,
    external_policy: str,
    extra_exclude: frozenset[str],
    extra_include: frozenset[str],
) -> dict[str, Any]:
    """Parse all properties for an object into semantic dict."""
    result: dict[str, Any] = {}
    expressions: dict[str, str] = {}
    membership: dict[str, Any] = {}
    local_links: list[str] = []
    external_links: list[dict[str, Any]] = []
    attachment: dict[str, Any] | None = None
    map_mode: str | None = None
    support: list[dict[str, str]] = []
    spreadsheet: dict[str, Any] | None = None
    sketch: dict[str, Any] | None = None
    partdesign: dict[str, Any] = {}

    for prop in properties:
        name = prop.get("name", "")
        ptype = prop.get("type", "")
        if not name:
            continue

        if name in extra_include:
            pass
        elif is_excluded_property(name, ptype, extra_exclude):
            continue

        status = _parse_status(prop.get("status"))
        if status & 0x1:
            continue

        elements = prop.get("elements", [])
        value = _parse_property_value(name, ptype, elements, limits, external_policy)
        if value is None:
            continue

        if name == "Label":
            result["label"] = value if isinstance(value, str) else str(value)
            continue

        if name == "Group" and isinstance(value, dict) and value.get("type") == "link_list":
            membership["group"] = value["targets"]
            continue

        if name == "OriginFeatures" and isinstance(value, dict) and value.get("type") == "link_list":
            result.setdefault("properties", {})[name] = value["targets"]
            local_links.extend(value["targets"])
            continue

        if name == "Body" and isinstance(value, dict) and value.get("type") == "link":
            membership["body"] = value["target"]
            continue

        if name == "Origin" and isinstance(value, dict) and value.get("type") == "link":
            membership["origin"] = value["target"]
            continue

        if name == "Tip" and isinstance(value, dict) and value.get("type") == "link":
            membership["tip"] = value["target"]
            continue

        if name == "Placement":
            result["placement"] = value
            continue

        if name == "ExpressionEngine":
            if isinstance(value, dict) and "expressions" in value:
                expressions.update(value["expressions"])
            continue

        if name in ("Cells",) or ptype == "Spreadsheet::PropertySheet":
            if isinstance(value, dict) and "cells" in value:
                spreadsheet = value["cells"]
            continue

        if name == "Constraints" or ptype == "Sketcher::PropertyConstraintList":
            if isinstance(value, dict) and "constraints" in value:
                sketch = sketch or {}
                sketch["constraints"] = value["constraints"]
            continue

        if name == "Geometry" or ptype in _GEOMETRY_LIST_TYPES:
            if isinstance(value, dict) and "geometries" in value:
                sketch = sketch or {}
                sketch["geometries"] = value["geometries"]
            continue

        if name in ("AttachmentSupport", "Support") and isinstance(value, dict):
            if value.get("type") == "link_sub_list":
                support = value["targets"]
            continue

        if name == "MapMode" and isinstance(value, str):
            map_mode = value
            continue

        if isinstance(value, dict) and value.get("type") == "external_link":
            external_links.append(value)
            continue

        if isinstance(value, dict) and value.get("type") == "link":
            local_links.append(value["target"])
            continue

        if isinstance(value, dict) and value.get("type") == "link_list":
            if name in ("Group", "Members"):
                membership["group"] = value["targets"]
            else:
                props = result.setdefault("properties", {})
                props[name] = value["targets"]
            local_links.extend(value["targets"])
            continue

        if isinstance(value, dict) and value.get("type") in ("link_sub", "link_sub_list"):
            if value.get("type") == "link_sub_list":
                local_links.extend(t["object"] for t in value.get("targets", []) if t.get("object"))
            elif value.get("object"):
                local_links.append(value["object"])
            props = result.setdefault("properties", {})
            props[name] = value
            continue

        if name in (
            "Length",
            "Length2",
            "Height",
            "Radius",
            "Angle",
            "Offset",
            "Type",
            "Reversed",
            "Midplane",
            "Direction",
            "UseCustomVector",
            "Refine",
            "AllowMultiFace",
            "OffsetFromFace",
            "Offset2",
        ):
            partdesign[name] = value
        else:
            props = result.setdefault("properties", {})
            props[name] = value

    if membership:
        result["membership"] = membership
    if expressions:
        result["expressions"] = expressions
    if local_links:
        result["local_links"] = sorted(set(local_links))
    if external_links:
        result["external_links"] = external_links
    if attachment:
        result["attachment"] = attachment
    if map_mode:
        result["map_mode"] = map_mode
    if support:
        result["support"] = support
    if spreadsheet:
        result["spreadsheet"] = spreadsheet
    if sketch:
        result["sketch"] = sketch
    if partdesign:
        result["partdesign"] = partdesign

    return result
