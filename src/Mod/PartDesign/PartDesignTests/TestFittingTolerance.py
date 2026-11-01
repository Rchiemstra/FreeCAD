# SPDX-License-Identifier: LGPL-2.1-or-later
"""Pad/Pocket fitting tolerance is a size correction, half per boundary."""

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Part
from Test.GuiRecompute import close_document


class TestFittingTolerance(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TestFittingTolerance")
        self.body = self.doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        close_document(self.doc)

    def circle(self, radius=5):
        sketch = self.body.newObject("Sketcher::SketchObject", "Nominal")
        sketch.addGeometry(Part.Circle(App.Vector(), App.Vector(0, 0, 1), radius), False)
        return sketch

    def pad(self):
        sketch = self.circle()
        pad = self.body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 2
        self.doc.recompute()
        return pad

    def pocket(self):
        base = self.body.newObject("PartDesign::AdditiveBox", "Base")
        base.Length = base.Width = 20
        base.Height = 4
        base.Placement.Base = App.Vector(-10, -10, -4)
        sketch = self.circle()
        pocket = self.body.newObject("PartDesign::Pocket", "Pocket")
        pocket.Profile = sketch
        pocket.Length = 4
        self.doc.recompute()
        return pocket

    def assertSize(self, feature, diameter):
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertTrue(feature.Shape.isValid())
        # GUI tessellation makes bounding boxes approximate. Measure the
        # analytic circular rims while keeping the same dimensional precision.
        radii = [edge.Curve.Radius for edge in feature.AddSubShape.Edges
                 if isinstance(edge.Curve, Part.Circle)]
        self.assertTrue(radii)
        self.assertAlmostEqual(2 * max(radii), diameter, places=6)
        self.assertAlmostEqual(2 * feature.Profile[0].Shape.Edges[0].Curve.Radius, 10, places=6)

    def testPadSizeAndZero(self):
        pad = self.pad()
        nominal = pad.Shape.Volume
        self.assertEqual(pad.FittingTolerance.Value, 0)
        self.assertSize(pad, 10)
        for tolerance, size in ((0.4, 9.6), (-0.4, 10.4), (0, 10)):
            pad.FittingTolerance = tolerance
            self.doc.recompute()
            self.assertSize(pad, size)
        self.assertAlmostEqual(pad.Shape.Volume, nominal)

    def testPocketDiameterAndZero(self):
        pocket = self.pocket()
        nominal = pocket.Shape.Volume
        self.assertEqual(pocket.FittingTolerance.Value, 0)
        for tolerance, size in ((0.4, 10.4), (-0.4, 9.6), (0, 10)):
            pocket.FittingTolerance = tolerance
            self.doc.recompute()
            self.assertSize(pocket, size)
            self.assertAlmostEqual(pocket.Shape.Volume, 1600 - math.pi * size**2)
        self.assertAlmostEqual(pocket.Shape.Volume, nominal)

    def testExpressionAndReload(self):
        for factory in (self.pad, self.pocket):
            with self.subTest(feature=factory.__name__):
                feature = factory()
                name = feature.Name
                feature.setExpression("FittingTolerance", "Length / 10")
                self.doc.recompute()
                expected = feature.Shape.Volume
                with tempfile.TemporaryDirectory() as directory:
                    path = os.path.join(directory, "fit.FCStd")
                    self.doc.saveAs(path)
                    close_document(self.doc)
                    self.doc = App.openDocument(path)
                    self.body = self.doc.getObject("Body")
                    feature = self.doc.getObject(name)
                    self.doc.recompute(None, True, True)
                    self.assertEqual(dict(feature.ExpressionEngine)["FittingTolerance"], "Length / 10")
                    self.assertAlmostEqual(feature.Shape.Volume, expected)
                    feature.Length = 3
                    self.doc.recompute()
                    self.assertAlmostEqual(feature.FittingTolerance.Value, 0.3)
                    self.assertSize(feature, 9.7 if factory.__name__ == "pad" else 10.3)
                close_document(self.doc)
                self.setUp()

    def testAllowanceProfileComposition(self):
        from PartDesign.AllowanceProfile import makeAllowanceProfile

        profile = makeAllowanceProfile(self.circle(), -0.1)
        pad = self.body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = profile
        pad.Length = 2
        pad.FittingTolerance = 0.4
        self.doc.recompute()
        self.assertNotIn("Invalid", pad.State, pad.getStatusString())
        self.assertAlmostEqual(pad.Shape.BoundBox.XLength, 9.4)
        self.assertAlmostEqual(profile.Shape.BoundBox.XLength, 9.8)

    def testRectangleSize(self):
        sketch = self.body.newObject("Sketcher::SketchObject", "Rectangle")
        points = [App.Vector(x, y, 0) for x, y in ((0, 0), (10, 0), (10, 8), (0, 8))]
        for a, b in zip(points, points[1:] + points[:1]):
            sketch.addGeometry(Part.LineSegment(a, b), False)
        pad = self.body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 2
        pad.FittingTolerance = 0.4
        self.doc.recompute()
        self.assertAlmostEqual(pad.Shape.BoundBox.XLength, 9.6)
        self.assertAlmostEqual(pad.Shape.BoundBox.YLength, 7.6)

    def testCollapsedProfileFails(self):
        pad = self.pad()
        pad.FittingTolerance = 11
        self.doc.recompute()
        self.assertIn("Invalid", pad.State)
        self.assertIn("collaps", pad.getStatusString().lower())

    def testNestedBoundaries(self):
        pad = self.pad()
        pad.Profile[0].addGeometry(Part.Circle(App.Vector(), App.Vector(0, 0, 1), 2), False)
        pad.FittingTolerance = 0.4
        self.doc.recompute()
        self.assertSize(pad, 9.6)
        self.assertAlmostEqual(pad.Shape.Volume, math.pi * (4.8**2 - 2.2**2) * 2)

    def testRotatedAndTranslatedSketch(self):
        pad = self.pad()
        pad.Profile[0].Placement = App.Placement(
            App.Vector(13, 17, 23), App.Rotation(App.Vector(1, 1, 0), 63)
        )
        pad.FittingTolerance = 0.4
        self.doc.recompute()
        self.assertNotIn("Invalid", pad.State, pad.getStatusString())
        self.assertAlmostEqual(pad.Shape.Volume, math.pi * 4.8**2 * 2)
        center = pad.Profile[0].Placement.multVec(App.Vector(0, 0, 1))
        self.assertLess((pad.Shape.CenterOfMass - center).Length, 1e-6)

    def testSelectedFaceOfRotatedBody(self):
        for kind in ("Pad", "Pocket"):
            with self.subTest(feature=kind):
                self.body = self.doc.addObject("PartDesign::Body", "RotatedBody")
                self.body.Placement = App.Placement(
                    App.Vector(13, 17, 23), App.Rotation(App.Vector(1, 1, 0), 63)
                )
                base = self.body.newObject("PartDesign::AdditiveBox", "Box")
                base.Length, base.Width, base.Height = 20, 16, 8
                self.doc.recompute()
                # A YZ face is not in the Box object's local XY plane.
                index = next(i for i, f in enumerate(base.Shape.Faces, 1)
                             if f.normalAt(0, 0).x > 0.99)
                feature = self.body.newObject("PartDesign::" + kind, kind)
                feature.Profile = (base, ["Face%d" % index])
                feature.Length = 2
                self.doc.recompute()
                self.assertNotIn("Invalid", feature.State, feature.getStatusString())
                nominal = feature.Shape.copy()
                feature.FittingTolerance = 0.4
                self.doc.recompute()
                self.assertNotIn("Invalid", feature.State, feature.getStatusString())
                self.assertTrue(feature.Shape.isValid())
                width, height = (15.6, 7.6) if kind == "Pad" else (16.4, 8.4)
                self.assertAlmostEqual(feature.AddSubShape.Volume, width * height * 2)
                expected = 2560 + width * height * 2 if kind == "Pad" else 2560 - 16 * 8 * 2
                self.assertAlmostEqual(feature.Shape.Volume, expected)
                feature.FittingTolerance = 0
                self.doc.recompute()
                self.assertAlmostEqual(feature.Shape.Volume, nominal.Volume)

    def testXZSketchInRotatedBody(self):
        for factory in (self.pad, self.pocket):
            with self.subTest(feature=factory.__name__):
                feature = factory()
                frame = App.Placement(App.Vector(2, 3, 4), App.Rotation(App.Vector(1, 0, 0), 90))
                feature.Profile[0].Placement = frame
                if feature.BaseFeature:
                    feature.BaseFeature.Placement = frame.multiply(feature.BaseFeature.Placement)
                self.body.Placement = App.Placement(
                    App.Vector(13, 17, 23), App.Rotation(App.Vector(1, 1, 0), 63)
                )
                feature.FittingTolerance = 0.4
                self.doc.recompute()
                self.assertNotIn("Invalid", feature.State, feature.getStatusString())
                self.assertTrue(feature.Shape.isValid())
                expected = math.pi * 4.8**2 * 2 if factory == self.pad else 1600 - math.pi * 5.2**2 * 4
                self.assertAlmostEqual(feature.Shape.Volume, expected)
                close_document(self.doc)
                self.setUp()

    def testPocketIslandShrinks(self):
        pocket = self.pocket()
        pocket.Length = 2  # Keep the island joined to the floor.
        sketch = pocket.Profile[0]
        sketch.addGeometry(Part.Circle(App.Vector(), App.Vector(0, 0, 1), 2), False)
        pocket.FittingTolerance = 0.4
        self.doc.recompute()
        self.assertSize(pocket, 10.4)
        self.assertAlmostEqual(pocket.Shape.Volume, 1600 - math.pi * (5.2**2 - 1.8**2) * 2)
        radii = {round(e.Curve.Radius, 6) for e in pocket.Shape.Edges
                 if isinstance(e.Curve, Part.Circle)}
        self.assertEqual(radii, {1.8, 5.2})
        self.assertEqual([g.Radius for g in sketch.Geometry], [5, 2])

    def testSeveralSeparateWires(self):
        pocket = self.pocket()
        sketch = pocket.Profile[0]
        sketch.delGeometry(0)
        for x, y in ((-5, 0), (5, 0), (0, 5)):
            sketch.addGeometry(Part.Circle(App.Vector(x, y, 0), App.Vector(0, 0, 1), 2), False)
        pocket.FittingTolerance = 0.4
        self.doc.recompute()
        self.assertNotIn("Invalid", pocket.State, pocket.getStatusString())
        self.assertTrue(pocket.Shape.isValid())
        self.assertEqual(len(pocket.AddSubShape.Solids), 3)
        self.assertAlmostEqual(pocket.Shape.Volume, 1600 - 3 * math.pi * 2.2**2 * 4)
        self.assertEqual([g.Radius for g in sketch.Geometry], [2, 2, 2])

    def testTwoDimensionsAndSymmetric(self):
        for factory in (self.pad, self.pocket):
            for side in ("Two sides", "Symmetric"):
                with self.subTest(feature=factory.__name__, side=side):
                    feature = factory()
                    if factory == self.pocket:
                        feature.Profile[0].Placement.Base.z = -2
                    feature.SideType = side
                    feature.Length = 1 if side == "Two sides" else 3
                    feature.Length2 = 2
                    feature.FittingTolerance = 0.4
                    self.doc.recompute()
                    self.assertSize(feature, 9.6 if factory == self.pad else 10.4)
                    radius = 4.8 if factory == self.pad else 5.2
                    self.assertAlmostEqual(feature.AddSubShape.Volume, math.pi * radius**2 * 3)
                    expected = math.pi * radius**2 * 3
                    self.assertAlmostEqual(feature.Shape.Volume, expected if factory == self.pad else 1600 - expected)
                    close_document(self.doc)
                    self.setUp()

    def testUpToFace(self):
        for factory in (self.pad, self.pocket):
            with self.subTest(feature=factory.__name__):
                feature = factory()
                if factory == self.pad:
                    reference = self.doc.addObject("PartDesign::Feature", "Reference")
                    reference.Shape = Part.makePlane(40, 40, App.Vector(-20, -20, 3))
                    face = "Face1"
                    depth, radius = 3, 4.8
                else:
                    reference = feature.BaseFeature
                    index = next(i for i, f in enumerate(reference.Shape.Faces, 1)
                                 if f.normalAt(0, 0).z < -0.99)
                    face = "Face%d" % index
                    depth, radius = 4, 5.2
                feature.UpToFace = (reference, [face])
                feature.Type = "UpToFace"
                feature.FittingTolerance = 0.4
                self.doc.recompute()
                self.assertSize(feature, 2 * radius)
                self.assertAlmostEqual(feature.AddSubShape.Volume, math.pi * radius**2 * depth)
                expected = math.pi * radius**2 * depth
                self.assertAlmostEqual(feature.Shape.Volume, expected if factory == self.pad else 1600 - expected)
                close_document(self.doc)
                self.setUp()

    def testUndoRedoToleranceChange(self):
        from Test.GuiRecompute import recompute_document, undo_document, redo_document

        for factory in (self.pad, self.pocket):
            with self.subTest(feature=factory.__name__):
                feature = factory()
                name = feature.Name
                self.doc.UndoMode = 1
                self.doc.openTransaction("Change fitting tolerance")
                feature.FittingTolerance = 0.4
                self.doc.commitTransaction()
                # GUI mutation readiness excludes an open transaction.
                recompute_document(self.doc)
                expected = 9.6 if factory == self.pad else 10.4
                self.assertSize(feature, expected)
                undo_document(self.doc)
                recompute_document(self.doc)
                feature = self.doc.getObject(name)
                self.assertEqual(feature.FittingTolerance.Value, 0)
                self.assertSize(feature, 10)
                redo_document(self.doc)
                recompute_document(self.doc)
                feature = self.doc.getObject(name)
                self.assertEqual(feature.FittingTolerance.Value, 0.4)
                self.assertSize(feature, expected)
                close_document(self.doc)
                self.setUp()

    def testLegacyDocumentWithoutProperty(self):
        import xml.etree.ElementTree as ET
        import zipfile

        for factory in (self.pad, self.pocket):
            with self.subTest(feature=factory.__name__):
                feature = factory()
                name = feature.Name
                volume = feature.Shape.Volume
                with tempfile.TemporaryDirectory() as directory:
                    path = os.path.join(directory, "current.FCStd")
                    legacy = os.path.join(directory, "legacy.FCStd")
                    self.doc.saveAs(path)
                    with zipfile.ZipFile(path) as source, zipfile.ZipFile(legacy, "w") as target:
                        removed = 0
                        for member in source.infolist():
                            data = source.read(member.filename)
                            if member.filename == "Document.xml":
                                root = ET.fromstring(data)
                                for properties in root.iter("Properties"):
                                    for prop in list(properties):
                                        if prop.get("name") == "FittingTolerance":
                                            properties.remove(prop)
                                            properties.set("Count", str(len(properties)))
                                            removed += 1
                                data = ET.tostring(root, encoding="utf-8", xml_declaration=True)
                            target.writestr(member, data)
                        self.assertGreater(removed, 0)
                    close_document(self.doc)
                    self.doc = App.openDocument(legacy)
                    self.body = self.doc.getObject("Body")
                    feature = self.doc.getObject(name)
                    self.assertEqual(feature.FittingTolerance.Value, 0)
                    self.doc.recompute(None, True, True)
                    self.assertAlmostEqual(feature.Shape.Volume, volume)
                close_document(self.doc)
                self.setUp()

    def testSharedSketchMatingParts(self):
        # Both features consume exactly the same nominal sketch. Opposite
        # signed corrections give a line-to-line fit; positive Pad tolerance
        # additionally provides radial clearance.
        pocket = self.pocket()
        sketch = pocket.Profile[0]
        shaft_body = self.doc.addObject("PartDesign::Body", "ShaftBody")
        shaft = shaft_body.newObject("PartDesign::Pad", "Shaft")
        shaft.Profile = sketch
        shaft.Reversed = True
        shaft.Length = 4
        pocket.FittingTolerance = 0.4
        shaft.FittingTolerance = -0.4
        self.doc.recompute()
        self.assertSize(pocket, 10.4)
        self.assertSize(shaft, 10.4)
        self.assertAlmostEqual(pocket.Shape.common(shaft.Shape).Volume, 0, places=6)
        self.assertAlmostEqual(pocket.Shape.Volume + shaft.Shape.Volume, 1600)
        shaft.FittingTolerance = 0.4
        self.doc.recompute()
        self.assertSize(shaft, 9.6)
        self.assertAlmostEqual(pocket.Shape.common(shaft.Shape).Volume, 0, places=6)
        self.assertAlmostEqual(sketch.Geometry[0].Radius, 5)

    def extrusion_mode(self, factory, mode):
        if mode in ("UpToFirst", "UpToLast") and factory == self.pad:
            base = self.body.newObject("PartDesign::Feature", "Target")
            base.Shape = Part.makeBox(20, 20, 4, App.Vector(-10, -10, 2))
        feature = factory()
        sketch = feature.Profile[0]
        if mode in ("ThroughAll", "UpToFirst", "UpToLast"):
            feature.Type = mode
        elif mode == "UpToShape":
            target = self.doc.addObject("Part::Feature", "TargetPlane")
            z = 3 if factory == self.pad else -3
            target.Shape = Part.makePlane(40, 40, App.Vector(-20, -20, z))
            feature.UpToShape = [(target, ["Face1"])]
            feature.Type = mode
        elif mode == "Taper":
            feature.Length = 2
            feature.TaperAngle = 5
        elif mode == "Reversed":
            feature.Reversed = True
            if factory == self.pocket:
                sketch.Placement.Base.z = -4
        elif mode == "CustomDirection":
            feature.UseCustomVector = True
            feature.Direction = App.Vector(0.1, 0, 1 if factory == self.pad else -1)
            feature.Length = 2
        elif mode in ("Offset", "Reference"):
            feature.Length = 1
            feature.StartType = mode
            feature.StartOffset = 0.5
            if mode == "Reference":
                target = self.doc.addObject("Part::Feature", "StartPlane")
                target.Shape = Part.makePlane(40, 40, App.Vector(-20, -20, 0))
                feature.StartReference = (target, ["Face1"])
        feature.FittingTolerance = 0.4
        self.doc.recompute()
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        actual = feature.Shape.copy()
        tool = feature.AddSubShape.copy()
        self.assertTrue(actual.isValid())
        self.assertGreater(actual.Volume, 0)
        self.assertAlmostEqual(sketch.Geometry[0].Radius, 5)
        # Independent geometric oracle: an explicitly resized sketch and zero
        # tolerance must produce the complete same solid for this mode.
        radius = 4.8 if factory == self.pad else 5.2
        sketch.delGeometry(0)
        sketch.addGeometry(Part.Circle(App.Vector(), App.Vector(0, 0, 1), radius), False)
        feature.FittingTolerance = 0
        self.doc.recompute()
        self.assertNotIn("Invalid", feature.State, feature.getStatusString())
        self.assertAlmostEqual(actual.Volume, feature.Shape.Volume, places=5)
        self.assertAlmostEqual(tool.Volume, feature.AddSubShape.Volume, places=5)
        self.assertAlmostEqual(actual.cut(feature.Shape).Volume, 0, places=5)
        self.assertAlmostEqual(feature.Shape.cut(actual).Volume, 0, places=5)
        close_document(self.doc)
        self.setUp()

    def testThroughAllWithTolerance(self):
        self.extrusion_mode(self.pocket, "ThroughAll")

    def testUpToFirstLastWithTolerance(self):
        for factory, mode in ((self.pad, "UpToFirst"), (self.pad, "UpToLast"),
                              (self.pocket, "UpToFirst")):
            with self.subTest(feature=factory.__name__, mode=mode):
                self.extrusion_mode(factory, mode)

    def testUpToShapeWithTolerance(self):
        for factory in (self.pad, self.pocket):
            with self.subTest(feature=factory.__name__):
                self.extrusion_mode(factory, "UpToShape")

    def testTaperReversedCustomDirectionWithTolerance(self):
        for factory in (self.pad, self.pocket):
            for mode in ("Taper", "Reversed", "CustomDirection"):
                with self.subTest(feature=factory.__name__, mode=mode):
                    self.extrusion_mode(factory, mode)

    def testOffsetStartModesWithTolerance(self):
        for factory in (self.pad, self.pocket):
            for mode in ("Offset", "Reference"):
                with self.subTest(feature=factory.__name__, mode=mode):
                    self.extrusion_mode(factory, mode)
