# SPDX-License-Identifier: LGPL-2.1-or-later
"""Pad/Pocket fitting tolerance is a size correction, half per boundary."""

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Part


class TestFittingTolerance(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TestFittingTolerance")
        self.body = self.doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

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
        self.assertAlmostEqual(feature.AddSubShape.BoundBox.XLength, diameter, places=6)
        self.assertAlmostEqual(feature.Profile[0].Shape.BoundBox.XLength, 10, places=6)

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
                    App.closeDocument(self.doc.Name)
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
                App.closeDocument(self.doc.Name)
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
                    App.closeDocument(self.doc.Name)
                    self.doc = App.openDocument(legacy)
                    self.body = self.doc.getObject("Body")
                    feature = self.doc.getObject(name)
                    self.assertEqual(feature.FittingTolerance.Value, 0)
                    self.doc.recompute(None, True, True)
                    self.assertAlmostEqual(feature.Shape.Volume, volume)
                App.closeDocument(self.doc.Name)
                self.setUp()
