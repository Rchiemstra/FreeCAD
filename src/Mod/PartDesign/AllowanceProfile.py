# SPDX-License-Identifier: LGPL-2.1-or-later
"""A nominal sketch's manufacturing profile, usable by ordinary Pad and Pocket.

Allowance is a signed boundary distance, not a diameter correction. Positive
values enlarge the filled profile (outer boundaries expand, holes contract).
A Pad therefore gains material and a Pocket removes more material. Corners use
intersections of the offset edges. Topology changes are deliberately rejected:
a disappearing hole or narrow region must not silently become a different part.
"""

import FreeCAD as App
import Part

_TOLERANCE = 1e-7
_ALLOWANCE_DOC = (
    "Signed boundary distance (not diameter): positive expands outer loops and "
    "shrinks holes; negative shrinks outer loops and expands holes. "
    "Positive adds material to a Pad and removes more material with a Pocket. "
    "For a 10 mm circular Pocket, +0.2 mm gives a 10.4 mm diameter."
)


def makeAllowanceProfile(source, allowance=0):
    """Create a profile in the source sketch's Body, without changing the sketch."""
    if not source or not source.isDerivedFrom("Sketcher::SketchObject"):
        raise ValueError("Allowance profile requires a source sketch")
    body = source.getParentGeoFeatureGroup()
    if body is None or not body.isDerivedFrom("PartDesign::Body"):
        raise ValueError("Allowance profile source sketch must belong to a Body")
    obj = body.newObject("Part::Part2DObjectPython", "AllowanceProfile")
    AllowanceProfile(obj)
    obj.Source = source
    obj.Allowance = allowance
    if App.GuiUp:
        from PartDesign.AllowanceProfileGui import ViewProviderAllowanceProfile

        ViewProviderAllowanceProfile(obj.ViewObject)
    return obj


def _offset_wire(wire, distance):
    # OCC can return a reversed top-level wire after an outward circle offset.
    # Rebuilding from its oriented edges makes subsequent signed offsets stable.
    wire = Part.Wire(wire.Edges)
    return wire.makeOffset2D(distance, join=2)


def _loop_faces(wires):
    """Validate simple disjoint boundaries and return their containment depths."""
    if not wires or any(not wire.isClosed() or not wire.isValid() for wire in wires):
        raise ValueError("source must contain closed, valid wires")
    faces = [Part.Face(wire) for wire in wires]
    if any(not face.isValid() or face.Area <= _TOLERANCE**2 for face in faces):
        raise ValueError("a loop is collapsed or self-intersecting")
    depths = [0] * len(wires)
    for i, wire in enumerate(wires):
        for j in range(i):
            if wire.distToShape(wires[j])[0] <= _TOLERANCE:
                raise ValueError("profile loops touch or intersect")
        point = wire.Vertexes[0].Point
        depths[i] = sum(
            face.isInside(point, _TOLERANCE, False) for j, face in enumerate(faces) if i != j
        )
    return faces, depths


def _offset_face(face, distance):
    bounds = face.BoundBox
    if distance < 0 and min(bounds.XLength, bounds.YLength) <= -2 * distance + _TOLERANCE:
        raise ValueError("offset collapses a loop (allowance exceeds its width)")
    result = face.makeOffset2D(distance, join=2)
    if result.isNull() or len(result.Wires) != 1:
        raise ValueError("offset collapses or splits a narrow region")
    wire = result.Wires[0]
    if not wire.isClosed() or not wire.isValid():
        raise ValueError("offset boundary is open or self-intersecting")
    result_face = Part.Face(wire)
    if not result_face.isValid() or result_face.Area <= _TOLERANCE**2:
        raise ValueError("offset boundary is collapsed or self-intersecting")
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
        raise ValueError("offset reverses or collapses a boundary")
    # OCC may return a valid, single wire after erasing an appendage or recess.
    # Recovering the nominal region catches that loss even without a split.
    restored = _offset_wire(wire, -distance)
    if restored.isNull() or len(restored.Wires) != 1:
        raise ValueError("offset collapses a narrow region")
    restored_face = Part.Face(restored.Wires[0])
    difference = face.Area + restored_face.Area - 2 * face.common(restored_face).Area
    if abs(difference) > area_tolerance:
        raise ValueError("offset collapses a narrow region (boundary cannot be recovered)")
    return wire


class AllowanceProfile:
    """Persistent Python proxy; Source and Allowance are the only model inputs."""

    def __init__(self, obj):
        obj.addProperty(
            "App::PropertyLink", "Source", "Profile", "Nominal source sketch (unchanged)"
        )
        # PropertyDistance is a signed length; PropertyLength clamps at zero.
        obj.addProperty("App::PropertyDistance", "Allowance", "Profile", _ALLOWANCE_DOC)
        obj.setEditorMode("Placement", 1)
        obj.Proxy = self

    def execute(self, obj):
        try:
            source = obj.Source
            if source is None or not source.isDerivedFrom("Sketcher::SketchObject"):
                raise ValueError("select a source sketch")
            if source.getParentGeoFeatureGroup() != obj.getParentGeoFeatureGroup():
                raise ValueError("source sketch must be in the same Body")
            obj.Placement = source.Placement
            shape = source.Shape.copy()
            shape.Placement = App.Placement()
            wires = shape.Wires
            if sum(len(w.Edges) for w in wires) != len(shape.Edges):
                raise ValueError("source contains edges outside closed wires")
            faces, depths = _loop_faces(wires)
            allowance = obj.Allowance.Value
            if allowance == 0:
                obj.Shape = source.Shape
                return
            result = [
                _offset_face(face, allowance if depth % 2 == 0 else -allowance)
                for face, depth in zip(faces, depths)
            ]
            _, result_depths = _loop_faces(result)
            if result_depths != depths:
                raise ValueError("offset changes loop nesting or collapses the intervening region")
            output = Part.makeCompound(result)
            filled = Part.makeFace(output.Wires, "Part::FaceMakerBullseye")
            if not filled.isValid():
                raise ValueError("offset produces a self-intersecting profile")
            output.Placement = source.Placement
            obj.Shape = output
        except Exception as exc:
            obj.Shape = Part.Shape()
            raise ValueError("Invalid allowance profile: " + str(exc)) from exc

    def dumps(self):
        return None

    def loads(self, state):
        pass
