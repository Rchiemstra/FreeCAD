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

"""Regression coverage for cross-body PartDesign datums after Assembly-style
body movement.

P1 (doc/mcp-feedback.md) covers the creation-time case: a PartDesign datum
created on one body and attached to a face on another body must include the
source body's current non-identity placement. This is the exact PartDesign
datum + Assembly movement repro left open in doc/freecad_issues_status_check.md.

P5 covers the later staleness case: after a valid cross-body datum exists, an
Assembly joint can move the source body and then suppress the touched state, so
the datum does not recompute.

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

    def makePaddedSourceBody(self, name):
        body_b = self.doc.addObject("PartDesign::Body", name)
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
        return body_b, pad, top_face

    def makeCrossBodyDatum(self, support, subelement):
        body_a = self.doc.addObject("PartDesign::Body", "DatumOwner")
        datum = body_a.newObject("PartDesign::Plane", "CrossDatum")
        datum.AttachmentSupport = [(support, subelement)]
        datum.MapMode = "FlatFace"
        self.doc.recompute()
        return datum

    def assertDatumContainsSourceFace(self, pad, top_face, datum):
        face_centre = pad.getGlobalPlacement() * pad.Shape.Faces[int(top_face[4:]) - 1].CenterOfMass
        datum_base = datum.getGlobalPlacement().Base
        drift = (face_centre - datum_base).Length
        self.assertLess(drift, 1e-2, f"datum drifted {drift:.4f} mm from the moved source face")

    def test_datum_created_after_body_move_uses_current_source_placement(self):
        body_b, pad, top_face = self.makePaddedSourceBody("MovedBeforeDatumCreation")

        # Reproduce the P1 creation-time boundary left open in the feedback
        # status doc: Assembly has already moved the source body, then a
        # PartDesign datum is created cross-body on that moved source.
        body_b.Placement = App.Placement(App.Vector(0, 0, 10), App.Rotation())
        self.doc.recompute()

        datum = self.makeCrossBodyDatum(pad, top_face)
        self.assertDatumContainsSourceFace(pad, top_face, datum)

    @unittest.expectedFailure
    def test_datum_stays_stale_after_purgeTouched_placement_write(self):
        body_b, pad, top_face = self.makePaddedSourceBody("MovedAfterDatumCreation")
        datum = self.makeCrossBodyDatum(pad, top_face)

        # Reproduce AssemblyObject::setNewPlacements's exact write pattern for
        # a body moved as a *side effect* of a joint solve (not the object
        # being directly dragged): write Placement, then purgeTouched().
        body_b.Placement = App.Placement(App.Vector(0, 0, 10), App.Rotation())
        body_b.purgeTouched()
        self.doc.recompute()

        # Desired behaviour: the datum should track the moved face after a
        # recompute. Currently it does not (drift == the full 10 mm move).
        self.assertDatumContainsSourceFace(pad, top_face, datum)
