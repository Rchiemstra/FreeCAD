# SPDX-License-Identifier: LGPL-2.1-or-later
"""Conservative, explicit repair of cached Sketcher references.

Ranking is read-only and restricted to the original source object. This first
slice accepts one-to-one projections whose ordered geometry is unchanged within
model tolerance. A nearby edge is offered for inspection, but cannot silently
move a constrained endpoint. Faces are supported for single-face attachments.
No persistent cache or document properties are added.
"""

from dataclasses import dataclass
import math

import FreeCAD as App
import Part

TOLERANCE = 1e-7


class RepairError(ValueError):
    """A repair cannot be proven to preserve sketch identity."""


@dataclass(frozen=True)
class Reference:
    kind: str
    key: str
    source: str
    subelement: str
    geo_id: int = 0


@dataclass(frozen=True)
class Candidate:
    source: str
    subelement: str
    score: float
    compatible: bool
    reason: str = ""


@dataclass(frozen=True)
class ExternalGeometry:
    geometry: object
    id: int
    reference: str
    missing: bool
    frozen: bool


def external_geometry(sketch):
    """Read cached geometry copies in negative geometry ID order (-3, -4, ...)."""
    result = []
    for geometry in sketch.ExternalGeo[2:]:
        external = geometry.getExtensionOfType("Sketcher::ExternalGeometryExtension")
        identity = geometry.getExtensionOfType("Sketcher::SketchGeometryExtension")
        result.append(
            ExternalGeometry(
                geometry,
                identity.Id,
                external.Ref,
                external.testFlag("Missing"),
                external.testFlag("Frozen"),
            )
        )
    return result


def _element(source, name):
    try:
        element = source.getSubObject(name)
        if element is None or (hasattr(element, "isNull") and element.isNull()):
            return None
        return element
    except (AttributeError, IndexError, RuntimeError, ValueError, Part.OCCError):
        return None


def broken_references(sketch):
    """List cached missing projections and unresolved attachment subelements."""
    result = []
    for index, facade in enumerate(external_geometry(sketch)):
        if facade.reference and facade.missing:
            source, _, sub = facade.reference.partition(".")
            result.append(
                Reference("external", facade.reference, source, sub, -index - 3)
            )
    for index, (source, subs) in enumerate(sketch.AttachmentSupport):
        for sub in subs:
            if sub and _element(source, sub) is None:
                result.append(Reference("attachment", str(index), source.Name, sub))
    return result


def _project(sketch, point):
    # Match SketchObject::rebuildExternalGeometry, which uses Placement.
    point = sketch.Placement.inverse().multVec(point)
    return App.Vector(point.x, point.y, 0)


def _samples(shape):
    if shape.ShapeType == "Vertex":
        return [shape.Point]
    return [
        shape.valueAt(
            shape.FirstParameter + (shape.LastParameter - shape.FirstParameter) * i / 8
        )
        for i in range(9)
    ]


def _distance(first, second):
    return math.sqrt(
        sum((a - b).Length ** 2 for a, b in zip(first, second)) / len(first)
    )


def _external_score(sketch, facade, shape):
    geometry = facade.geometry
    is_point = isinstance(geometry, Part.Point)
    if is_point:
        old = [App.Vector(geometry.X, geometry.Y, geometry.Z)]
    else:
        old = _samples(geometry.toShape())
    new = [_project(sketch, p) for p in _samples(shape)]
    if is_point:
        old *= len(new)
    score = _distance(old, new)
    if not math.isfinite(score):
        return math.inf, False, "Invalid replacement geometry"
    if len(old) != len(new):
        return score, False, "Incompatible geometry type"
    if is_point and shape.ShapeType == "Edge":
        # Native straight edges perpendicular to the plane project to one point.
        if (
            not isinstance(shape.Curve, Part.Line)
            or (new[-1] - new[0]).Length >= TOLERANCE
        ):
            return score, False, "Replacement edge no longer projects to a point"
    elif not is_point:
        if type(geometry.toShape().Curve) is not type(shape.Curve):
            return score, False, "Incompatible curve type"
        # Ordered samples include both endpoints. Native reattachment is also
        # checked below for exact geometry type, cardinality and geometry IDs;
        # a projection that splits cannot commit.
        if (old[0] - new[0]).Length > TOLERANCE or (
            old[-1] - new[-1]
        ).Length > TOLERANCE:
            return (
                score,
                False,
                "Split, reversed or moved edge would change constraint endpoints",
            )
    if score > TOLERANCE:
        return score, False, "Replacement would move constraint endpoints"
    return score, True, ""


def _face_score(sketch, face):
    if not isinstance(face.Surface, Part.Plane):
        return math.inf, False, "Attachment requires a planar face"
    frame = sketch.Placement.multiply(sketch.AttachmentOffset.inverse())
    normal = face.normalAt(0, 0)
    direction = frame.Rotation.multVec(App.Vector(0, 0, 1))
    distance = abs((face.CenterOfMass - frame.Base).dot(normal))
    angle = 1 - abs(normal.dot(direction))
    score = distance + angle
    return (
        score,
        score <= TOLERANCE,
        "" if score <= TOLERANCE else "Face would move attachment plane",
    )


def candidates(sketch, reference):
    """Rank same-type subelements; include refusal reasons for inspection."""
    if reference not in broken_references(sketch):
        raise RepairError("Reference is no longer broken; refresh the list")
    if reference.kind == "attachment":
        source = sketch.AttachmentSupport[int(reference.key)][0]
        if source is None or source.Document != sketch.Document:
            raise RepairError(
                "Attachments in another document cannot be repaired by this command"
            )
    else:
        source = sketch.Document.getObject(reference.source)
    if source is None or not hasattr(source, "Shape"):
        return []
    facade = None
    if reference.kind == "external":
        facades = external_geometry(sketch)
        facade = facades[-reference.geo_id - 3]
        kind = sketch.getExternalReferenceType(reference.geo_id)
        if kind not in ("Edge", "Vertex"):
            raise RepairError(
                "Cannot safely map this external reference type; only edges and vertices are supported"
            )
        grouped = sum(g.reference == facade.reference for g in facades) > 1
    else:
        kind = "Face"
        grouped = False
    result = []
    for index, shape in enumerate(
        getattr(
            source.Shape, {"Edge": "Edges", "Vertex": "Vertexes", "Face": "Faces"}[kind]
        )
    ):
        try:
            score, compatible, reason = (
                _face_score(sketch, shape)
                if facade is None
                else _external_score(sketch, facade, shape)
            )
        except (RuntimeError, ValueError, Part.OCCError) as error:
            score, compatible, reason = (
                math.inf,
                False,
                f"Cannot compare geometry: {error}",
            )
        if facade is None and sketch.MapMode != "FlatFace":
            compatible, reason = False, "Only FlatFace attachments are supported"
        if grouped:
            compatible, reason = (
                False,
                "Multiple projections cannot be mapped one-to-one",
            )
        if facade is not None and facade.frozen:
            compatible, reason = (
                False,
                "Frozen projections must be synchronized separately",
            )
        result.append(
            Candidate(source.Name, f"{kind}{index + 1}", score, compatible, reason)
        )
    return sorted(result, key=lambda c: (c.score, c.subelement))


_CONSTRAINT_FIELDS = (
    "Type",
    "Name",
    "First",
    "FirstPos",
    "Second",
    "SecondPos",
    "Third",
    "ThirdPos",
    "Value",
    "Driving",
    "IsActive",
    "InVirtualSpace",
)


def _identity(sketch):
    constraints = tuple(
        tuple(getattr(c, name) for name in _CONSTRAINT_FIELDS)
        for c in sketch.Constraints
    )
    ids = tuple(sketch.getGeometryId(i) for i in range(sketch.GeometryCount))
    external_ids = tuple(g.id for g in external_geometry(sketch))
    return constraints, tuple(sketch.ExpressionEngine), ids, external_ids


def _geometry(sketch):
    return list(sketch.Geometry) + [g.geometry for g in external_geometry(sketch)]


def repair(sketch, choices):
    """Validate all choices, then reattach in one transaction or roll back all.

    Choices are (Reference, Candidate) pairs from the read-only discovery API.
    Revalidate immediately before mutation, including ties and stale selections.
    The caller must finish any existing transaction first.
    """
    choices = list(choices)
    if not choices:
        raise RepairError("Choose at least one broken reference")
    doc = sketch.Document
    if doc.HasPendingTransaction:
        raise RepairError("Finish the current transaction before repairing references")
    keys, targets = set(), set()
    for reference, selected in choices:
        ranked = candidates(sketch, reference)
        fresh = next(
            (
                c
                for c in ranked
                if (c.source, c.subelement) == (selected.source, selected.subelement)
            ),
            None,
        )
        if fresh is None:
            raise RepairError(
                "Candidate is unavailable or has an incompatible subelement type; refresh the list"
            )
        if not fresh.compatible:
            raise RepairError(fresh.reason)
        if (
            sum(
                c.compatible and abs(c.score - fresh.score) <= TOLERANCE for c in ranked
            )
            > 1
        ):
            raise RepairError(
                "Ambiguous mapping: equally compatible candidates; repair upstream geometry first"
            )
        key = (reference.kind, reference.key)
        target = (reference.kind, fresh.source, fresh.subelement)
        if key in keys or target in targets:
            raise RepairError("Ambiguous many-to-one mapping")
        keys.add(key)
        targets.add(target)
        if reference.kind == "attachment" and (
            len(sketch.AttachmentSupport) != 1
            or len(sketch.AttachmentSupport[0][1]) != 1
        ):
            raise RepairError("Only single-face attachments can be repaired safely")
    identity, geometry, placement = (
        _identity(sketch),
        _geometry(sketch),
        sketch.Placement,
    )
    doc.openTransaction("Repair sketch references")
    try:
        for reference, selected in choices:
            if reference.kind == "external":
                sketch.attachExternal(
                    reference.geo_id, selected.source, selected.subelement
                )
            else:
                sketch.AttachmentSupport = (
                    doc.getObject(selected.source),
                    [selected.subelement],
                )
        if not sketch.recompute():
            raise RepairError("Sketch failed to recompute after repair")
        if any(ref in broken_references(sketch) for ref, _ in choices):
            raise RepairError("Reference remains broken after recompute")
        if "Invalid" in sketch.State:
            raise RepairError("Sketch failed to recompute after repair")
        if _identity(sketch) != identity or not sketch.Placement.isSame(
            placement, TOLERANCE
        ):
            raise RepairError(
                "Repair would change constraints, expressions, geometry IDs or attachment placement"
            )
        current = _geometry(sketch)
        if len(current) != len(geometry) or any(
            not a.isSame(b, TOLERANCE, TOLERANCE) for a, b in zip(geometry, current)
        ):
            raise RepairError("Repair would change geometry or constraint endpoints")
        doc.commitTransaction()
    except Exception as error:
        doc.abortTransaction()
        if isinstance(error, RepairError):
            raise
        raise RepairError(f"Cannot repair reference: {error}") from error
