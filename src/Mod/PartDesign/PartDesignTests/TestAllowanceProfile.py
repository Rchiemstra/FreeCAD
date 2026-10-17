# SPDX-License-Identifier: LGPL-2.1-or-later
"""Manufacturing profiles must leave the nominal sketch and solid history intact."""

import math
import os
import tempfile
import unittest

import FreeCAD as App
import Part
import Sketcher


class TestAllowanceProfile(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TestAllowanceProfile")
        self.body = self.doc.addObject("PartDesign::Body", "Body")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def circle(self, radius=5):
        sketch = self.body.newObject("Sketcher::SketchObject", "Nominal")
        sketch.addGeometry(Part.Circle(App.Vector(), App.Vector(0, 0, 1), radius), False)
        sketch.addConstraint(Sketcher.Constraint("Radius", 0, radius))
        self.doc.recompute()
        return sketch

    def rectangle(self, sketch, x0, y0, x1, y1):
        points = [(x0, y0), (x1, y0), (x1, y1), (x0, y1)]
        for a, b in zip(points, points[1:] + points[:1]):
            sketch.addGeometry(Part.LineSegment(App.Vector(a[0], a[1], 0), App.Vector(b[0], b[1], 0)), False)

    def profile(self, sketch, allowance):
        from PartDesign.AllowanceProfile import makeAllowanceProfile

        obj = makeAllowanceProfile(sketch, allowance)
        self.doc.recompute()
        return obj

    def area(self, obj):
        return Part.makeFace(obj.Shape.Wires, "Part::FaceMakerBullseye").Area

    def assertInvalid(self, obj):
        self.assertIn("Invalid", obj.State)
        self.assertTrue(obj.Shape.isNull(), "Failed recompute must clear stale geometry")
        self.assertIn("allowance", obj.getStatusString().lower())

    def testPart2DCompatibility(self):
        """Guard against needing any changes to Body or the C++ profile machinery."""
        p = self.body.newObject("Part::Part2DObjectPython", "Profile")
        p.Shape = Part.Wire([Part.makeCircle(5)])
        pad = self.body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = p
        pad.Length = 2
        self.doc.recompute()
        self.assertIn(p, self.body.Group)
        self.assertIsNone(self.body.BaseFeature)
        self.assertIsNone(pad.BaseFeature)
        self.assertAlmostEqual(pad.Shape.Volume, math.pi * 50)
        q = self.body.newObject("Part::Part2DObjectPython", "CutProfile")
        q.Shape = Part.Wire([Part.makeCircle(2)])
        q.Placement.Base.z = 2
        pocket = self.body.newObject("PartDesign::Pocket", "Pocket")
        pocket.Profile = q
        pocket.Length = 1
        self.doc.recompute()
        self.assertEqual(pocket.BaseFeature, pad)
        self.assertAlmostEqual(pocket.Shape.Volume, math.pi * 46)

    def testCirclePositive(self):
        p = self.profile(self.circle(), 0.2)
        self.assertAlmostEqual(p.Shape.Edges[0].Curve.Radius, 5.2)

    def testCircleNegative(self):
        p = self.profile(self.circle(), -0.1)
        self.assertAlmostEqual(p.Allowance.Value, -0.1)
        self.assertAlmostEqual(p.Shape.Edges[0].Curve.Radius, 4.9)

    def testRectangleWithHole(self):
        s = self.body.newObject("Sketcher::SketchObject", "Nominal")
        self.rectangle(s, -10, -8, 10, 8)
        s.addGeometry(Part.Circle(App.Vector(), App.Vector(0, 0, 1), 3), False)
        for a in (0.2, -0.2):
            with self.subTest(allowance=a):
                p = self.profile(s, a)
                self.assertEqual(len(p.Shape.Wires), 2)
                self.assertAlmostEqual(self.area(p), (20 + 2*a)*(16 + 2*a) - math.pi*(3-a)**2)

    def testPadVolume(self):
        p = self.profile(self.circle(), -0.1)
        pad = self.body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = p
        pad.Length = 3
        self.doc.recompute()
        self.assertIsNone(pad.BaseFeature)
        self.assertAlmostEqual(pad.Shape.Volume, math.pi * 4.9**2 * 3)

    def testPocketVolume(self):
        box = self.body.newObject("PartDesign::AdditiveBox", "Box")
        box.Length = box.Width = 20
        box.Height = 5
        s = self.circle()
        s.Placement.Base = App.Vector(10, 10, 5)
        p = self.profile(s, 0.2)
        pocket = self.body.newObject("PartDesign::Pocket", "Pocket")
        pocket.Profile = p
        pocket.Length = 2
        self.doc.recompute()
        self.assertEqual(pocket.BaseFeature, box)
        self.assertAlmostEqual(pocket.Shape.Volume, 2000 - math.pi * 5.2**2 * 2)

    def testZeroAllowance(self):
        s = self.circle()
        p = self.profile(s, 0)
        self.assertEqual(p.Shape.exportBrepToString(), s.Shape.exportBrepToString())

    def testRecomputeSketchAndExpression(self):
        s = self.circle()
        p = self.profile(s, 0.2)
        p.setExpression("Allowance", "Nominal.Constraints[0] / 25")
        s.setDatum(0, App.Units.Quantity("10 mm"))
        self.doc.recompute()
        self.assertAlmostEqual(p.Allowance.Value, 0.4)
        self.assertAlmostEqual(p.Shape.Edges[0].Curve.Radius, 10.4)

    def testNominalConstraintsUntouched(self):
        s = self.circle()
        constraints = [str(c) for c in s.Constraints]
        geometry = s.Geometry[0].Radius
        p = self.profile(s, 0.2)
        p.Allowance = -0.3
        self.doc.recompute()
        self.assertEqual([str(c) for c in s.Constraints], constraints)
        self.assertEqual(s.Geometry[0].Radius, geometry)
        self.assertEqual(s.getDatum(0).Value, 5)

    def testSaveReopen(self):
        s = self.circle()
        p = self.profile(s, -0.1)
        p.setExpression("Allowance", "-Nominal.Constraints[0] / 50")
        name = p.Name
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "allowance.FCStd")
            self.doc.recompute()
            self.doc.saveAs(path)
            App.closeDocument(self.doc.Name)
            self.doc = App.openDocument(path)
            p = self.doc.getObject(name)
            p.Source.setDatum(0, App.Units.Quantity("10 mm"))
            self.doc.recompute()
            self.assertAlmostEqual(p.Shape.Edges[0].Curve.Radius, 9.8)
            self.assertEqual(p.Source.Name, "Nominal")

    def testPlacementAndRecompute(self):
        s = self.circle()
        s.Placement = App.Placement(App.Vector(1, 2, 3), App.Rotation(App.Vector(1, 0, 0), 90))
        p = self.profile(s, 0.2)
        self.assertTrue(p.Placement.isSame(s.Placement, 1e-7))
        self.assertAlmostEqual(p.Shape.BoundBox.YMin, 2)
        self.assertAlmostEqual(p.Shape.BoundBox.ZMax, 8.2)
        s.Placement.Base = App.Vector(4, 5, 6)
        self.doc.recompute()
        self.assertAlmostEqual(p.Shape.BoundBox.ZMax, 11.2)

    def testCollapsedCircleClearsShape(self):
        p = self.profile(self.circle(), 0.2)
        p.Allowance = -6
        self.doc.recompute()
        self.assertInvalid(p)
        p.Allowance = 0.1
        self.doc.recompute()
        self.assertNotIn("Invalid", p.State)
        self.assertAlmostEqual(p.Shape.Edges[0].Curve.Radius, 5.1)

    def testCollapsedRectangle(self):
        s = self.body.newObject("Sketcher::SketchObject", "Nominal")
        self.rectangle(s, 0, 0, 10, 1)
        p = self.profile(s, -0.6)
        self.assertInvalid(p)

    def testCollapsedHole(self):
        s = self.body.newObject("Sketcher::SketchObject", "Nominal")
        self.rectangle(s, -10, -10, 10, 10)
        s.addGeometry(Part.Circle(App.Vector(), App.Vector(0, 0, 1), 1), False)
        p = self.profile(s, 1.1)
        self.assertInvalid(p)

    def testIntersectingLoops(self):
        s = self.body.newObject("Sketcher::SketchObject", "Nominal")
        self.rectangle(s, -5, -5, 5, 5)
        self.rectangle(s, -4, -4, 4, 4)
        p = self.profile(s, -0.6)
        self.assertInvalid(p)

    def testSelfIntersectingSource(self):
        s = self.body.newObject("Sketcher::SketchObject", "Nominal")
        points = [(0, 0), (10, 10), (0, 10), (10, 0)]
        for a, b in zip(points, points[1:] + points[:1]):
            s.addGeometry(Part.LineSegment(App.Vector(a[0], a[1], 0), App.Vector(b[0], b[1], 0)), False)
        self.assertInvalid(self.profile(s, 0.1))

    def testOpenSource(self):
        s = self.body.newObject("Sketcher::SketchObject", "Nominal")
        s.addGeometry(Part.LineSegment(App.Vector(), App.Vector(10, 0, 0)), False)
        self.assertInvalid(self.profile(s, 0.1))
