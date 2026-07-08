# SPDX-License-Identifier: LGPL-2.1-or-later
# /****************************************************************************
#                                                                           *
#    Copyright (c) 2026 The FreeCAD project                                *
#                                                                           *
#    This file is part of FreeCAD.                                          *
#                                                                           *
#    FreeCAD is free software: you can redistribute it and/or modify it     *
#    under the terms of the GNU Lesser General Public License as            *
#    published by the Free Software Foundation, either version 2.1 of the   *
#    License, or (at your option) any later version.                        *
#                                                                           *
#    FreeCAD is distributed in the hope that it will be useful, but         *
#    WITHOUT ANY WARRANTY; without even the implied warranty of             *
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
#    Lesser General Public License for more details.                        *
#                                                                           *
#    You should have received a copy of the GNU Lesser General Public       *
#    License along with FreeCAD. If not, see                                *
#    <https://www.gnu.org/licenses/>.                                       *
#                                                                           *
# ***************************************************************************/

"""Regression coverage for P5 (doc/mcp-feedback.md): an Assembly joint moving
a body breaks cross-body PartDesign datums attached to that body.

``AssemblyObject::setNewPlacements`` (src/Mod/Assembly/App/AssemblyObject.cpp)
writes each joint-moved body's solved Placement and then immediately calls
``purgeTouched()`` on it -- every caller (``solve()``, ``doDragStep()``,
``postDrag()``) follows this pattern, and no ``App::Document::recompute()``
call exists anywhere in that file. Only the object the user is *directly*
dragging (``ViewProviderAssembly.cpp``) keeps normal touched/dirty state.

This test does not go through the Assembly solver -- it reproduces the exact
write pattern (``Placement = ...; obj.purgeTouched()``) directly, which is
sufficient to demonstrate the staleness independent of any particular joint
type or solve path. This is a confirmed real bug (empirically verified
against a real build: the datum drifts by the full move distance and never
recomputes), not fixed in this pass because the responsible fix touches
Assembly's interactive-drag-performance-critical code path and needs a
broader design decision (e.g. should a final ``solve()`` recompute while
``doDragStep()`` keeps ``purgeTouched()`` for performance?) beyond the scope
of a minimal, isolated change.
"""

import unittest

import FreeCAD as App
import Part
import Sketcher


def _xy_plane(body):
    for feat in body.Origin.OriginFeatures:
        if feat.Label == "XY_Plane" or feat.Name == "XY_Plane":
            return feat
    raise LookupError("XY_Plane not found")


class TestCrossBodyDatumStaleness(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("AssemblyCrossBodyDatumStaleness")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    @unittest.expectedFailure
    def test_datum_stays_stale_after_purgeTouched_placement_write(self):
        body_b = self.doc.addObject("PartDesign::Body", "MovableSource")
        sketch = body_b.newObject("Sketcher::SketchObject", "Sk")
        sketch.AttachmentSupport = [(_xy_plane(body_b), "")]
        sketch.MapMode = "FlatFace"
        sketch.addGeometry(Part.Circle(App.Vector(0, 0, 0), App.Vector(0, 0, 1), 5), False)
        self.doc.recompute()
        pad = body_b.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 5
        self.doc.recompute()

        gp = pad.getGlobalPlacement()
        top_face = None
        for i, f in enumerate(pad.Shape.Faces, start=1):
            c = gp * f.CenterOfMass
            if type(f.Surface).__name__ == "Plane" and abs(c.z - 5.0) < 0.5:
                top_face = f"Face{i}"
        self.assertIsNotNone(top_face, "could not locate the pad's top face")

        body_a = self.doc.addObject("PartDesign::Body", "DatumOwner")
        datum = body_a.newObject("PartDesign::Plane", "CrossDatum")
        datum.AttachmentSupport = [(pad, top_face)]
        datum.MapMode = "FlatFace"
        self.doc.recompute()

        # Reproduce AssemblyObject::setNewPlacements's exact write pattern for
        # a body moved as a *side effect* of a joint solve (not the object
        # being directly dragged): write Placement, then purgeTouched().
        body_b.Placement = App.Placement(App.Vector(0, 0, 10), App.Rotation())
        body_b.purgeTouched()
        self.doc.recompute()

        face_centre = pad.getGlobalPlacement() * pad.Shape.Faces[int(top_face[4:]) - 1].CenterOfMass
        datum_base = datum.getGlobalPlacement().Base
        drift = (face_centre - datum_base).Length

        # Desired behaviour: the datum should track the moved face after a
        # recompute. Currently it does not (drift == the full 10 mm move).
        self.assertLess(drift, 1e-2, f"datum drifted {drift:.4f} mm from the moved source face")
