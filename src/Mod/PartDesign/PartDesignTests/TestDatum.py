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

import os
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile

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

    def _originPlane(self, body, label):
        for feat in body.Origin.OriginFeatures:
            if feat.Label == label or feat.Name == label:
                return feat
        self.fail(f"{label} not found on {body.Name}")

    def testXYDatumPlane(self):
        self.Body = self.Doc.addObject("PartDesign::Body", "Body")
        self.DatumPlane = self.Doc.addObject("PartDesign::Plane", "DatumPlane")
        self.DatumPlane.AttachmentSupport = [(self.Doc.XY_Plane, "")]
        self.DatumPlane.MapMode = "FlatFace"
        self.Body.addObject(self.DatumPlane)
        self.Doc.recompute()
        self.DatumPlaneNormal = self.DatumPlane.Shape.Surface.Axis
        self.assertEqual(abs(self.DatumPlaneNormal.dot(App.Vector(0, 0, 1))), 1)

    def testCrossBodyFlatFaceKeepsSourceBodyPlacement(self):
        """A datum attached across bodies must use the source body's placement.

        The attacher computes the support pose in global coordinates. When the
        attached object lives inside a body, its stored Placement is body-local;
        writing the global pose directly used to drop or double-apply body
        placements for cross-body FlatFace attachments.
        """
        source = self.Doc.addObject("PartDesign::Body", "SourceBody")
        source.Placement.Base = App.Vector(0, 0, 10)

        sketch = source.newObject("Sketcher::SketchObject", "CircleSketch")
        sketch.AttachmentSupport = [(self._originPlane(source, "XY_Plane"), "")]
        sketch.MapMode = "FlatFace"
        sketch.addGeometry(Part.Circle(App.Vector(0, 0, 0), App.Vector(0, 0, 1), 5), False)
        self.Doc.recompute()

        pad = source.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 5
        self.Doc.recompute()

        pad_global = pad.getGlobalPlacement()
        top_face = None
        for i, face in enumerate(pad.Shape.Faces, start=1):
            centre = pad_global * face.CenterOfMass
            if type(face.Surface).__name__ == "Plane" and abs(centre.z - 15.0) < 0.5:
                top_face = f"Face{i}"
                break
        self.assertIsNotNone(top_face, "could not locate the moved pad's global top face")

        target = self.Doc.addObject("PartDesign::Body", "DatumBody")
        datum = target.newObject("PartDesign::Plane", "CrossDatum")
        datum.AttachmentSupport = [(pad, top_face)]
        datum.MapMode = "FlatFace"
        datum.AttachmentOffset = App.Placement()
        self.Doc.recompute()

        face = pad.Shape.Faces[int(top_face[4:]) - 1]
        face_centre = pad_global * face.CenterOfMass
        face_normal = pad_global.Rotation * App.Vector(
            face.Surface.Axis.x,
            face.Surface.Axis.y,
            face.Surface.Axis.z,
        )
        datum_global = datum.getGlobalPlacement()
        datum_normal = datum_global.Rotation * App.Vector(0, 0, 1)

        self.assertAlmostEqual(abs(face_normal.dot(datum_normal)), 1.0, places=4)
        self.assertLess(abs((face_centre - datum_global.Base).dot(datum_normal)), 1e-2)

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


class TestTranslateAttachment(unittest.TestCase):
    """Translate keeps the editable Placement orientation and applies offset rotation (#19571)."""

    def setUp(self):
        self.Doc = App.newDocument("TranslateAttachment")
        self.part = self.Doc.addObject("App::Part", "Part")
        self.lcs = self.part.newObject("Part::LocalCoordinateSystem", "LCS")
        self.origin = self.part.Origin
        self.vertex = self.origin.OriginFeatures[-1].Name + "."
        self.xy_plane = self.origin.OriginFeatures[3].Name + "."
        self.offset_position = App.Vector(100, 7, -3)

    def tearDown(self):
        App.closeDocument(self.Doc.Name)

    def attach(self, obj=None):
        obj = self.lcs if obj is None else obj
        obj.AttachmentSupport = [(self.origin, self.vertex)]
        obj.MapMode = "Translate"

    def set_offset(self, angle, obj=None):
        obj = self.lcs if obj is None else obj
        obj.AttachmentOffset = App.Placement(
            self.offset_position, App.Rotation(App.Vector(0, 0, 1), angle)
        )

    def assert_placement(self, rotation, position=None, obj=None):
        obj = self.lcs if obj is None else obj
        position = self.offset_position if position is None else position
        self.assertNotIn("Invalid", obj.State)
        self.assertTrue(obj.Placement.Base.isEqual(position, 1e-7), str(obj.Placement))
        self.assertTrue(obj.Placement.Rotation.isSame(rotation, 1e-7), str(obj.Placement))

    def recompute_and_check(self, rotation, position=None, obj=None):
        obj = self.lcs if obj is None else obj
        # Force execution: an untouched document's recompute can otherwise be a no-op.
        for _ in range(4):
            obj.touch()
            self.Doc.recompute()
            self.assert_placement(rotation, position, obj)

    def testTranslateOffsetRotation(self):
        self.attach()
        self.set_offset(90)
        self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), 90))

    def testTranslateChangingOffsetRotation(self):
        self.attach()
        for angle in (90, 45, 0):
            with self.subTest(angle=angle):
                self.set_offset(angle)
                self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), angle))

    def testTranslateOffsetBeforeAttaching(self):
        self.set_offset(90)
        self.attach()
        self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), 90))

    def testTranslatePreservesOriginalOrientation(self):
        # Different axes also catch an incorrect order of rotation composition.
        original = App.Rotation(App.Vector(1, 0, 0), 30)
        self.lcs.Placement = App.Placement(App.Vector(), original)
        self.attach()
        for angle in (90, 45, 0):
            with self.subTest(angle=angle):
                self.set_offset(angle)
                self.recompute_and_check(original * App.Rotation(App.Vector(0, 0, 1), angle))

    def testTranslatePositionIsVertexPlusOffset(self):
        anchor = self.part.newObject("Part::Feature", "Anchor")
        point = App.Vector(12, -9, 5)
        anchor.Shape = Part.Vertex(point)
        self.lcs.AttachmentSupport = [(anchor, "Vertex1")]
        self.lcs.MapMode = "Translate"
        self.set_offset(90)
        self.recompute_and_check(
            App.Rotation(App.Vector(0, 0, 1), 90), point + self.offset_position
        )

    def testTranslatePlaneEngine(self):
        sketch = self.part.newObject("Sketcher::SketchObject", "Sketch")
        self.attach(sketch)
        for angle in (90, 45, 0):
            with self.subTest(angle=angle):
                self.set_offset(angle, sketch)
                self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), angle), obj=sketch)

    def testTranslateSaveReopen(self):
        original = App.Rotation(App.Vector(1, 0, 0), 30)
        self.lcs.Placement = App.Placement(App.Vector(), original)
        self.attach()
        self.set_offset(90)
        self.Doc.recompute()
        saved = self.lcs.Placement
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "Translate.FCStd")
            self.Doc.saveAs(path)
            App.closeDocument(self.Doc.Name)
            self.Doc = App.openDocument(path)
            self.lcs = self.Doc.getObject("LCS")
            self.assertTrue(self.lcs.Placement.isSame(saved, 1e-7))
            self.recompute_and_check(original * App.Rotation(App.Vector(0, 0, 1), 90))
            self.set_offset(45)
            self.recompute_and_check(original * App.Rotation(App.Vector(0, 0, 1), 45))

    def testTranslateModeRoundTrip(self):
        self.attach()
        self.set_offset(90)
        self.Doc.recompute()
        direct = self.lcs.Placement
        self.lcs.MapMode = "Deactivated"
        self.lcs.AttachmentSupport = [(self.origin, self.xy_plane)]
        self.lcs.MapMode = "FlatFace"
        self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), 90))
        self.lcs.MapMode = "Deactivated"
        self.attach()
        self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), 90))
        self.assertTrue(self.lcs.Placement.isSame(direct, 1e-7))

    def testTranslateNoOffsetRotationDoesNotMove(self):
        original = App.Rotation(App.Vector(1, 2, 3), 37)
        self.lcs.Placement = App.Placement(App.Vector(), original)
        self.attach()
        self.set_offset(0)
        self.Doc.recompute()
        saved = self.lcs.Placement
        self.recompute_and_check(original)
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "TranslateIdentityOffset.FCStd")
            self.Doc.saveAs(path)
            App.closeDocument(self.Doc.Name)
            self.Doc = App.openDocument(path)
            self.lcs = self.Doc.getObject("LCS")
            self.recompute_and_check(original)
            self.assertTrue(self.lcs.Placement.isSame(saved, 1e-7))

    def testTranslateManualOrientationRemainsEditable(self):
        self.attach()
        self.set_offset(90)
        manual = App.Rotation(App.Vector(0, 1, 0), 23)
        self.lcs.Placement = App.Placement(self.lcs.Placement.Base, manual)
        self.recompute_and_check(manual)
        self.set_offset(45)
        self.recompute_and_check(manual * App.Rotation(App.Vector(0, 0, 1), -45))

    def testTranslateUnchangedOffsetDoesNotChangeOrientation(self):
        self.attach()
        self.set_offset(90)
        expected = App.Rotation(App.Vector(0, 0, 1), 90)
        self.recompute_and_check(expected)
        self.offset_position = App.Vector(-2, 11, 8)
        self.set_offset(90)
        self.recompute_and_check(expected)

    def testTranslateInRotatedPart(self):
        self.part.Placement = App.Placement(
            App.Vector(20, 30, 40), App.Rotation(App.Vector(0, 1, 0), 25)
        )
        original = App.Rotation(App.Vector(1, 0, 0), 30)
        self.lcs.Placement = App.Placement(App.Vector(), original)
        self.attach()
        # Translate offsets are added in global coordinates, then converted to the parent frame.
        position = self.part.Placement.Rotation.inverted() * self.offset_position
        for angle in (0, 90, 45, 0):
            with self.subTest(angle=angle):
                self.set_offset(angle)
                self.recompute_and_check(
                    original * App.Rotation(App.Vector(0, 0, 1), angle), position
                )

    def testTranslateUndoRedoOffset(self):
        self.Doc.UndoMode = 1
        self.attach()
        self.set_offset(90)
        self.Doc.recompute()
        self.Doc.openTransaction("Change attachment offset rotation")
        self.set_offset(45)
        self.Doc.recompute()
        self.Doc.commitTransaction()
        self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), 45))
        self.Doc.undo()
        self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), 90))
        self.Doc.redo()
        self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), 45))
        self.Doc.undo()
        self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), 90))

    def testTranslateActivationUndoRedoUndo(self):
        self.Doc.UndoMode = 1
        self.set_offset(90)
        self.lcs.AttachmentSupport = [(self.origin, self.vertex)]
        self.Doc.recompute()
        self.Doc.openTransaction("Activate Translate")
        self.lcs.MapMode = "Translate"
        self.Doc.recompute()
        self.Doc.commitTransaction()
        self.assert_placement(App.Rotation(App.Vector(0, 0, 1), 90))
        self.Doc.undo()
        self.Doc.redo()
        self.Doc.undo()
        self.assertEqual(self.lcs.MapMode, "Deactivated")
        self.assertTrue(self.lcs.Placement.Rotation.isSame(App.Rotation(), 1e-7))
        self.lcs.MapMode = "Translate"
        self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), 90))

    def testTranslateDeactivatedSaveReopen(self):
        self.set_offset(90)
        self.lcs.AttachmentSupport = [(self.origin, self.vertex)]
        self.Doc.recompute()
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "Deactivated.FCStd")
            self.Doc.saveAs(path)
            App.closeDocument(self.Doc.Name)
            self.Doc = App.openDocument(path)
            self.lcs = self.Doc.getObject("LCS")
            self.assertTrue(self.lcs.Placement.Rotation.isSame(App.Rotation(), 1e-7))
            self.lcs.MapMode = "Translate"
            self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), 90))

    def testTranslateLegacySaveReopen(self):
        self.attach()
        self.set_offset(90)
        original = App.Rotation(App.Vector(1, 0, 0), 30)
        # Simulate a legacy Placement that never received the offset rotation.
        self.lcs.Placement = App.Placement(self.offset_position, original)
        self.Doc.recompute()
        saved = self.lcs.Placement
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "Translate.FCStd")
            legacy_path = os.path.join(directory, "Legacy.FCStd")
            self.Doc.saveAs(path)
            App.closeDocument(self.Doc.Name)
            removed = 0
            with zipfile.ZipFile(path) as source, zipfile.ZipFile(legacy_path, "w") as target:
                for entry in source.infolist():
                    data = source.read(entry.filename)
                    if entry.filename == "Document.xml":
                        root = ET.fromstring(data)
                        for properties in root.iter("Properties"):
                            for prop in list(properties):
                                if prop.get("name") == "AttachmentOffsetApplied":
                                    properties.remove(prop)
                                    removed += 1
                                    properties.set("Count", str(int(properties.get("Count")) - 1))
                        data = ET.tostring(root, encoding="utf-8", xml_declaration=True)
                    target.writestr(entry, data)
            self.assertGreater(removed, 0)
            self.Doc = App.openDocument(legacy_path)
            self.lcs = self.Doc.getObject("LCS")
            self.assertTrue(self.lcs.Placement.isSame(saved, 1e-7))
            self.recompute_and_check(original)
            self.set_offset(45)
            self.recompute_and_check(original * App.Rotation(App.Vector(0, 0, 1), -45))

    def testTranslateCopyObject(self):
        original = App.Rotation(App.Vector(1, 0, 0), 30)
        self.lcs.Placement = App.Placement(App.Vector(), original)
        self.attach()
        self.set_offset(90)
        self.Doc.recompute()
        copied = self.Doc.copyObject(self.lcs)
        self.part.addObject(copied)
        self.recompute_and_check(original * App.Rotation(App.Vector(0, 0, 1), 90), obj=copied)
        self.set_offset(45, copied)
        self.recompute_and_check(original * App.Rotation(App.Vector(0, 0, 1), 45), obj=copied)
        self.assert_placement(original * App.Rotation(App.Vector(0, 0, 1), 90))

    def testTranslateExpressionOffsetUndo(self):
        self.Doc.UndoMode = 1
        driver = self.Doc.addObject("App::FeaturePython", "Driver")
        driver.addProperty("App::PropertyAngle", "Angle")
        driver.Angle = 90
        self.attach()
        self.set_offset(0)
        self.lcs.setExpression("AttachmentOffset.Rotation.Angle", "Driver.Angle")
        self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), 90))
        self.Doc.openTransaction("Change expression input")
        driver.Angle = 45
        self.Doc.recompute()
        self.Doc.commitTransaction()
        self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), 45))
        for replay, angle in ((self.Doc.undo, 90), (self.Doc.redo, 45), (self.Doc.undo, 90)):
            replay()
            self.recompute_and_check(App.Rotation(App.Vector(0, 0, 1), angle))

    def testTranslateZeroOffsetDoesNotWriteAppliedRotation(self):
        self.attach()
        self.set_offset(0)
        self.Doc.recompute()
        changes = []

        class Observer:
            def slotChangedObject(observer, obj, prop):
                if obj == self.lcs and prop == "AttachmentOffsetApplied":
                    changes.append(prop)

        observer = Observer()
        App.addDocumentObserver(observer)
        try:
            self.recompute_and_check(App.Rotation())
        finally:
            App.removeDocumentObserver(observer)
        self.assertEqual(changes, [])
        self.assertTrue(self.lcs.AttachmentOffsetApplied.isSame(App.Rotation(), 1e-7))
