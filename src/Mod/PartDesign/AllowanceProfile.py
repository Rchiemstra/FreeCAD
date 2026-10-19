# SPDX-License-Identifier: LGPL-2.1-or-later
"""A nominal sketch's manufacturing profile, usable by ordinary Pad and Pocket.

Allowance is a signed boundary distance, not a diameter correction. Positive
values enlarge the filled profile (outer boundaries expand, holes contract).
A Pad therefore gains material and a Pocket removes more material. Corners use
intersections of the offset edges. Topology changes are deliberately rejected:
a disappearing hole or narrow region must not silently become a different part.

Native Pad/Pocket FittingTolerance uses this same offset algorithm but measures
full size change, with clearance signs: +0.4 mm makes a 10 mm Pocket 10.4 mm
wide and a 10 mm circular Pad 9.6 mm wide (0.2 mm per boundary). It composes with an
AllowanceProfile when one is used as the feature's Profile. Zero bypasses the
native offset path, including when restoring files predating FittingTolerance.
"""

import FreeCAD as App
import Part
from PySide.QtCore import QT_TRANSLATE_NOOP

_TOLERANCE = 1e-7
_ALLOWANCE_DOC = QT_TRANSLATE_NOOP(
    "App::Property",
    "The allowance is a signed boundary distance. Positive values expand outer loops and "
    "shrink holes; negative values shrink outer loops and expand holes. "
    "Positive values add material to a Pad and remove more material with a Pocket. "
    "For a 10 mm circular Pocket, an allowance of +0.2 mm gives a 10.4 mm diameter.",
)


def makeAllowanceProfile(source, allowance=0):
    """Create a profile in the source sketch's Body, without changing the sketch."""
    if not source or not source.isDerivedFrom("Sketcher::SketchObject"):
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "An allowance profile requires a source sketch.",
            )
        )
    body = source.getParentGeoFeatureGroup()
    if body is None or not body.isDerivedFrom("PartDesign::Body"):
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "The source sketch of an allowance profile must belong to a Body.",
            )
        )
    obj = body.newObject("Part::Part2DObjectPython", "AllowanceProfile")
    AllowanceProfile(obj)
    obj.Source = source
    obj.Allowance = allowance
    if App.GuiUp:
        from PartDesign.AllowanceProfileGui import ViewProviderAllowanceProfile

        ViewProviderAllowanceProfile(obj.ViewObject)
    return obj


def _planar_face(wire):
    # Keep containment and recovery checks on the same local support plane;
    # OCC can infer either plane orientation from a verified extrusion face.
    return Part.Face(Part.Plane(), wire)


def _offset_wire(wire, distance):
    # OCC can reverse both the wire and its closed edge after an offset.
    # Normalize both before recovery, including when the inferred support
    # plane points opposite to a circle's geometric axis.
    edges = wire.Edges
    if len(edges) == 1 and edges[0].Orientation == "Reversed":
        edges[0].reverse()
    wire = Part.Wire(edges)
    return _planar_face(wire).makeOffset2D(distance, join=2)


def _loop_faces(wires):
    """Validate simple disjoint boundaries and return their containment depths."""
    if not wires or any(not wire.isClosed() or not wire.isValid() for wire in wires):
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "The source must contain closed, valid wires.",
            )
        )
    faces = [_planar_face(wire) for wire in wires]
    if any(not face.isValid() or face.Area <= _TOLERANCE**2 for face in faces):
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "A loop is collapsed or self-intersecting.",
            )
        )
    depths = [0] * len(wires)
    for i, wire in enumerate(wires):
        for j in range(i):
            if wire.distToShape(wires[j])[0] <= _TOLERANCE:
                raise ValueError(
                    App.Qt.translate(
                        "PartDesign_AllowanceProfile",
                        "The profile loops touch or intersect.",
                    )
                )
        point = wire.Vertexes[0].Point
        depths[i] = sum(
            face.isInside(point, _TOLERANCE, False) for j, face in enumerate(faces) if i != j
        )
    return faces, depths


def _offset_face(face, distance):
    bounds = face.BoundBox
    if distance < 0 and min(bounds.XLength, bounds.YLength) <= -2 * distance + _TOLERANCE:
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "The offset collapses a loop because the allowance exceeds its width.",
            )
        )
    result = face.makeOffset2D(distance, join=2)
    if result.isNull() or len(result.Wires) != 1:
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "The offset collapses or splits a narrow region.",
            )
        )
    wire = result.Wires[0]
    if not wire.isClosed() or not wire.isValid():
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "The offset boundary is open or self-intersecting.",
            )
        )
    result_face = _planar_face(wire)
    if not result_face.isValid() or result_face.Area <= _TOLERANCE**2:
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "The offset boundary is collapsed or self-intersecting.",
            )
        )
    # Boundary position error accumulates with perimeter, not filled area.
    # Scaling with area could silently discard small details on large parts.
    area_tolerance = _TOLERANCE * max(face.Length, result_face.Length, 1)
    overlap = result_face.common(face).Area
    if distance > 0:
        contained = abs(overlap - face.Area) <= area_tolerance
        grows = result_face.Area > face.Area
    else:
        contained = abs(overlap - result_face.Area) <= area_tolerance
        grows = result_face.Area < face.Area
    if not contained or not grows:
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "The offset reverses or collapses a boundary.",
            )
        )
    # OCC may return a valid, single wire after erasing an appendage or recess.
    # Recovering the nominal region catches that loss even without a split.
    restored = _offset_wire(wire, -distance)
    if restored.isNull() or len(restored.Wires) != 1:
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "The offset collapses a narrow region.",
            )
        )
    restored_face = _planar_face(restored.Wires[0])
    difference = face.Area + restored_face.Area - 2 * face.common(restored_face).Area
    # A tiny lost detail can fit within the global area tolerance on a large
    # loop. Check nominal edge endpoints and interior samples locally as well.
    boundary_recovered = all(
        Part.Vertex(point).distToShape(restored.Wires[0])[0] <= _TOLERANCE
        for edge in face.Wires[0].Edges
        for point in edge.discretize(Number=5)
    )
    if abs(difference) > area_tolerance or not boundary_recovered:
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "The offset collapses a narrow region because its boundary cannot be recovered.",
            )
        )
    return wire


def offsetProfile(source_shape, allowance, placement=None):
    """Offset closed planar loops in their support plane, preserving nesting.

    Shared by the standalone profile and native Pad/Pocket fitting tolerance.
    The input is in document coordinates; allowance is a boundary distance.
    """
    if placement is None:
        plane = source_shape.findPlane()
        if plane is None:
            raise ValueError(
                App.Qt.translate(
                    "PartDesign_AllowanceProfile",
                    "The profile must lie in a single plane.",
                )
            )
        placement = App.Placement(plane.Position, App.Rotation(App.Vector(0, 0, 1), plane.Axis))
    shape = source_shape.copy()
    # The single-edge offset wrapper resets edge locations. Apply the frame
    # transform to copied geometry instead of only changing its Placement.
    shape.transformShape(placement.inverse().toMatrix(), True)
    wires = shape.Wires
    if sum(len(w.Edges) for w in wires) != len(shape.Edges):
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "The source contains edges outside closed wires.",
            )
        )
    faces, depths = _loop_faces(wires)
    if abs(allowance) <= _TOLERANCE:
        return source_shape
    result = [
        _offset_face(face, allowance if depth % 2 == 0 else -allowance)
        for face, depth in zip(faces, depths)
    ]
    _, result_depths = _loop_faces(result)
    if result_depths != depths:
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "The offset changes loop nesting or collapses the intervening region.",
            )
        )
    output = Part.makeCompound(result)
    filled = Part.makeFace(output.Wires, "Part::FaceMakerBullseye")
    if not filled.isValid():
        raise ValueError(
            App.Qt.translate(
                "PartDesign_AllowanceProfile",
                "The offset produces a self-intersecting profile.",
            )
        )
    output.Placement = placement
    return output


def fittingProfile(shape, tolerance, additive):
    """Convert a signed size correction to a boundary offset and return faces."""
    distance = (-0.5 if additive else 0.5) * tolerance
    wires = offsetProfile(shape, distance)
    result = Part.makeFace(wires.Wires, "Part::FaceMakerBullseye")
    if shape.Faces and result.Faces[0].normalAt(0, 0).dot(shape.Faces[0].normalAt(0, 0)) < 0:
        result.reverse()
    return result


class AllowanceProfile:
    """Persistent Python proxy; Source and Allowance are the only model inputs."""

    def __init__(self, obj):
        obj.addProperty(
            "App::PropertyLink", "Source", "Profile",
            QT_TRANSLATE_NOOP("App::Property", "The nominal source sketch remains unchanged.")
        )
        # PropertyDistance is a signed length; PropertyLength clamps at zero.
        obj.addProperty("App::PropertyDistance", "Allowance", "Profile", _ALLOWANCE_DOC)
        obj.setEditorMode("Placement", 1)
        obj.Proxy = self

    def execute(self, obj):
        try:
            source = obj.Source
            if source is None or not source.isDerivedFrom("Sketcher::SketchObject"):
                raise ValueError(App.Qt.translate("PartDesign_AllowanceProfile", "Select a source sketch."))
            if source.getParentGeoFeatureGroup() != obj.getParentGeoFeatureGroup():
                raise ValueError(
                    App.Qt.translate(
                        "PartDesign_AllowanceProfile",
                        "The source sketch must be in the same Body.",
                    )
                )
            obj.Placement = source.Placement
            obj.Shape = offsetProfile(source.Shape, obj.Allowance.Value, source.Placement)
        except Exception as exc:
            obj.Shape = Part.Shape()
            raise ValueError(
                App.Qt.translate("PartDesign_AllowanceProfile", "Cannot create the allowance profile.")
                + "\n" + str(exc)
            ) from exc

    def dumps(self):
        return None

    def loads(self, state):
        pass
