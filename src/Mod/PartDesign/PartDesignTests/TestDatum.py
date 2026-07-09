# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2011 Juergen Riegel <FreeCAD@juergen-riegel.net>        *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU Lesser General Public License (LGPL)    *
# *   as published by the Free Software Foundation; either version 2 of     *
# *   the License, or (at your option) any later version.                   *
# *   for detail see the LICENCE text file.                                 *
# *                                                                         *
# *   This program is distributed in the hope that it will be useful,       *
# *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
# *   GNU Library General Public License for more details.                  *
# *                                                                         *
# *   You should have received a copy of the GNU Library General Public     *
# *   License along with this program; if not, write to the Free Software   *
# *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
# *   USA                                                                   *
# *                                                                         *
# ***************************************************************************

import unittest

import FreeCAD
import Part

App = FreeCAD


class TestDatumPoint(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestDatumPoint")

    def testOriginDatumPoint(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.DatumPoint = self.Doc.addObject("PartDesign::Point", "DatumPoint")
        self.DatumPoint.AttachmentSupport = [(self.Doc.XY_Plane, "")]
        self.DatumPoint.MapMode = "ObjectOrigin"
        self.Body.addObject(self.DatumPoint)
        self.Doc.recompute()
        self.assertEqual(self.DatumPoint.AttachmentOffset.Base, App.Vector(0))

    def tearDown(self):
        # closing doc
        FreeCAD.closeDocument("PartDesignTestDatumPoint")
        # print ("omit closing document for debugging")


class TestDatumLine(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestDatumLine")

    def testXAxisDatumLine(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.DatumLine = self.Doc.addObject("PartDesign::Line", "DatumLine")
        self.DatumLine.AttachmentSupport = [(self.Doc.XY_Plane, "")]
        self.DatumLine.MapMode = "ObjectX"
        self.Body.addObject(self.DatumLine)
        self.Doc.recompute()
        self.assertNotIn("Invalid", self.DatumLine.State)

    def tearDown(self):
        # closing doc
        FreeCAD.closeDocument("PartDesignTestDatumLine")
        # print ("omit closing document for debugging")


class TestDatumPlane(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestDatumPlane")

    def testXYDatumPlane(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.DatumPlane = self.Doc.addObject("PartDesign::Plane", "DatumPlane")
        self.DatumPlane.AttachmentSupport = [(self.Doc.XY_Plane, "")]
        self.DatumPlane.MapMode = "FlatFace"
        self.Body.addObject(self.DatumPlane)
        self.Doc.recompute()
        self.DatumPlaneNormal = self.DatumPlane.Shape.Surface.Axis
        self.assertEqual(abs(self.DatumPlaneNormal.dot(App.Vector(0, 0, 1))), 1)

    def testFlatFaceOriginIsNotFaceCentre(self):
        """Regression coverage for P3/P7 feedback (doc/mcp-feedback.md): a
        PartDesign::Plane FlatFace-attached to an offset face lands its origin at
        the *support object's own placement origin* projected onto the face's
        plane, not at the face's centre of mass.

        This is confirmed, deliberate Attacher behaviour (AttachEngine3D::
        _calculateAttachedPlacement, mmFlatFace case, src/Mod/Part/App/
        Attacher.cpp) -- not a bug to be fixed here. It is kept as a *hazard*
        regression test, not silently accepted as "fine": it is undocumented,
        unintuitive, and in the originating session it read as a spurious
        ~19.4 mm position error when the plane itself was perfectly correct.
        If this test ever starts failing, the Attacher's FlatFace origin
        semantics have changed and every downstream MCP guardrail relying on
        this behaviour (placement_audit, preview_attachment) needs revisiting.
        """
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.Sketch = self.Body.newObject("Sketcher::SketchObject", "Sketch")
        self.Sketch.AttachmentSupport = [(self.Doc.XY_Plane, "")]
        self.Sketch.MapMode = "FlatFace"
        # Offset the source sketch/pad well away from the global origin so the
        # face centre and the global origin projection clearly disagree.
        self.Sketch.AttachmentOffset = App.Placement(App.Vector(-10, 0, 0), App.Rotation())
        self.Sketch.addGeometry(Part.Circle(App.Vector(0, 0, 0), App.Vector(0, 0, 1), 5), False)
        self.Doc.recompute()
        self.Pad = self.Body.newObject("PartDesign::Pad", "Pad")
        self.Pad.Profile = self.Sketch
        self.Pad.Length = 5
        self.Doc.recompute()

        gp = self.Pad.getGlobalPlacement()
        top_face = None
        for i, f in enumerate(self.Pad.Shape.Faces, start=1):
            c = gp * f.CenterOfMass
            if type(f.Surface).__name__ == "Plane" and abs(c.z - 5.0) < 0.5:
                top_face = f"Face{i}"
        self.assertIsNotNone(top_face, "could not locate the pad's top face")
        face_centre = gp * self.Pad.Shape.Faces[int(top_face[4:]) - 1].CenterOfMass

        self.DatumOwner = self.Doc.addObject("PartDesign::Body", "DatumOwner")
        self.CrossDatum = self.DatumOwner.newObject("PartDesign::Plane", "CrossDatum")
        self.CrossDatum.AttachmentSupport = [(self.Pad, top_face)]
        self.CrossDatum.MapMode = "FlatFace"
        self.Doc.recompute()

        datum_gp = self.CrossDatum.getGlobalPlacement()
        # The plane itself is still geometrically correct: zero distance and
        # zero angle error to the referenced face.
        normal = datum_gp.Rotation * App.Vector(0, 0, 1)
        plane_distance = (face_centre - datum_gp.Base).dot(normal)
        self.assertAlmostEqual(plane_distance, 0.0, places=6)
        # But the origin point is NOT the face centre -- it is the projection
        # of the support's own placement origin (here, the global origin,
        # since Pad/Body carry identity placement) onto that plane.
        origin_error = (datum_gp.Base - face_centre).Length
        self.assertGreater(
            origin_error,
            1.0,
            "datum origin unexpectedly close to the face centre -- if the Attacher "
            "was changed to centre on the face, update this hazard test and the "
            "P7 status in doc/freecad_issues_status_check.md",
        )

    def tearDown(self):
        # closing doc
        FreeCAD.closeDocument("PartDesignTestDatumPlane")
        # print ("omit closing document for debugging")


class TestDeactivatedPlacement(unittest.TestCase):
    """Regression coverage for P3 (doc/mcp-feedback.md): `MapMode = Deactivated`
    was reported to silently ignore a manually-set Placement rotation. Three
    distinct code paths were probed empirically against a real build
    (src/Mod/Part/App/AttachExtension.cpp, positionBySupport): a raw
    ``Placement`` write while Deactivated, an ``AttachmentOffset`` write while
    Deactivated, and a manual rotation under ``MapMode = 'Translate'``. Only
    the ``AttachmentOffset``-while-Deactivated case actually drops anything.
    """

    def setUp(self):
        self.Doc = FreeCAD.newDocument("PartDesignTestDeactivatedPlacement")

    def testRawPlacementRotationHonoured(self):
        """A raw Placement assignment while MapMode='Deactivated' is NOT
        touched by positionBySupport() (it early-returns before reading or
        writing Placement at all), so both translation and rotation apply."""
        body = self.Doc.addObject("PartDesign::Body", "CylBody")
        cyl = body.newObject("PartDesign::AdditiveCylinder", "Cyl")
        cyl.Radius = 2
        cyl.Height = 1
        cyl.Angle = 360
        cyl.MapMode = "Deactivated"
        # Rotate the default Z axis onto +X (90 deg about Y).
        cyl.Placement = App.Placement(App.Vector(0, 0, 0), App.Rotation(App.Vector(0, 1, 0), 90))
        self.Doc.recompute()

        bbox = cyl.Shape.BoundBox
        self.assertAlmostEqual(bbox.XMin, 0.0, places=3)
        self.assertAlmostEqual(bbox.XMax, 1.0, places=3)
        self.assertAlmostEqual(bbox.YMin, -2.0, places=3)
        self.assertAlmostEqual(bbox.YMax, 2.0, places=3)

    @unittest.expectedFailure
    def testAttachmentOffsetDroppedWhileDeactivated(self):
        """CONFIRMED BUG (not fixed in this pass -- see P3 in
        doc/freecad_issues_status_check.md): editing AttachmentOffset while
        MapMode='Deactivated' hits the same early return in
        positionBySupport() as a raw Placement edit, but unlike the raw-
        Placement case, AttachmentOffset is never applied to Placement at all
        under any mode change afterwards -- both translation and rotation of
        the offset are silently dropped. This test encodes the *desired*
        behaviour (the offset should apply) and is expected to fail until a
        deliberate design decision is made about what AttachmentOffset should
        mean when there is no active attachment to offset from."""
        body = self.Doc.addObject("PartDesign::Body", "SkBody")
        sketch = body.newObject("Sketcher::SketchObject", "Sk")
        sketch.AttachmentSupport = [(self.Doc.XY_Plane, "")]
        sketch.MapMode = "Deactivated"
        sketch.AttachmentOffset = App.Placement(
            App.Vector(5, 0, 0), App.Rotation(App.Vector(0, 1, 0), 90)
        )
        self.Doc.recompute()

        self.assertTrue(sketch.Placement.Base.isEqual(App.Vector(5, 0, 0), 1e-6))
        self.assertGreater(abs(sketch.Placement.Rotation.Angle), 1e-3)

    def testManualRotationHonouredUnderTranslateMode(self):
        """A manual Placement rotation set after attaching with
        MapMode='Translate' is preserved: Translate mode only ever writes the
        translation component via the attacher, leaving rotation open for the
        user (Placement is not forced read-only for this mode), and a direct
        Placement write is not one of the properties that re-triggers the
        attacher."""
        anchor = self.Doc.addObject("Part::Box", "Anchor")
        anchor.Length = anchor.Width = anchor.Height = 1
        self.Doc.recompute()

        body = self.Doc.addObject("PartDesign::Body", "TranslateBody")
        cyl = body.newObject("PartDesign::AdditiveCylinder", "Cyl")
        cyl.Radius = 2
        cyl.Height = 1
        cyl.Angle = 360
        cyl.AttachmentSupport = [(anchor, "Vertex1")]
        cyl.MapMode = "Translate"
        self.Doc.recompute()

        cyl.Placement = App.Placement(cyl.Placement.Base, App.Rotation(App.Vector(0, 1, 0), 90))
        self.Doc.recompute()

        bbox = cyl.Shape.BoundBox
        self.assertAlmostEqual(bbox.XMax - bbox.XMin, 1.0, places=3)
        self.assertAlmostEqual(bbox.YMax - bbox.YMin, 4.0, places=3)

    def tearDown(self):
        FreeCAD.closeDocument("PartDesignTestDeactivatedPlacement")
