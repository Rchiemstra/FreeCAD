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
            sketch.addGeometry(
                Part.LineSegment(App.Vector(a[0], a[1], 0), App.Vector(b[0], b[1], 0)), False
            )

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
                self.assertAlmostEqual(
                    self.area(p), (20 + 2 * a) * (16 + 2 * a) - math.pi * (3 - a) ** 2
                )

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
        # Equivalent location chains can have different BRep serializations.
        nominal = Part.makeFace(s.Shape.Wires, "Part::FaceMakerBullseye")
        actual = Part.makeFace(p.Shape.Wires, "Part::FaceMakerBullseye")
        self.assertAlmostEqual(nominal.Area + actual.Area - 2 * nominal.common(actual).Area, 0)
        self.assertEqual(len(p.Shape.Wires), len(s.Shape.Wires))
        self.assertEqual(len(p.Shape.Edges), len(s.Shape.Edges))
        self.assertAlmostEqual(p.Shape.Length, s.Shape.Length)
        self.assertTrue(p.Placement.isSame(s.Placement, 1e-7))

    def testRecomputeSketchAndExpression(self):
        s = self.circle()
        p = self.profile(s, 0.2)
        p.setExpression("Allowance", "Nominal.Constraints[0] / 25")
        s.setDatum(0, App.Units.Quantity("10 mm"))
        self.doc.recompute()
        self.assertAlmostEqual(p.Allowance.Value, 0.4)
        self.assertAlmostEqual(p.Shape.Edges[0].Curve.Radius, 10.4)

    def testAllowanceWithinToleranceIsZero(self):
        s = self.circle()
        p = self.profile(s, 0)
        for allowance in (1e-9, -1e-9, 1e-7, -1e-7):
            with self.subTest(allowance=allowance):
                p.Allowance = allowance
                p.Proxy.execute(p)
                self.assertEqual(p.Allowance.Value, allowance)
                self.assertEqual(len(p.Shape.Edges), len(s.Shape.Edges))
                self.assertAlmostEqual(p.Shape.Edges[0].Curve.Radius, 5)
                self.assertAlmostEqual(p.Shape.Length, s.Shape.Length)

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
            s.addGeometry(
                Part.LineSegment(App.Vector(a[0], a[1], 0), App.Vector(b[0], b[1], 0)), False
            )
        self.assertInvalid(self.profile(s, 0.1))

    def testOpenSource(self):
        s = self.body.newObject("Sketcher::SketchObject", "Nominal")
        s.addGeometry(Part.LineSegment(App.Vector(), App.Vector(10, 0, 0)), False)
        self.assertInvalid(self.profile(s, 0.1))

    def testNarrowBridgeCollapse(self):
        s = self.body.newObject("Sketcher::SketchObject", "Nominal")
        points = [
            (0, 0),
            (4, 0),
            (4, 1.5),
            (6, 1.5),
            (6, 0),
            (10, 0),
            (10, 4),
            (6, 4),
            (6, 2.5),
            (4, 2.5),
            (4, 4),
            (0, 4),
        ]
        for a, b in zip(points, points[1:] + points[:1]):
            s.addGeometry(
                Part.LineSegment(App.Vector(a[0], a[1], 0), App.Vector(b[0], b[1], 0)), False
            )
        self.assertInvalid(self.profile(s, -0.6))

    def testNarrowAppendageCollapse(self):
        s = self.body.newObject("Sketcher::SketchObject", "Nominal")
        points = [(0, 0), (4, 0), (4, 1.5), (8, 1.5), (8, 2.5), (4, 2.5), (4, 4), (0, 4)]
        for a, b in zip(points, points[1:] + points[:1]):
            s.addGeometry(
                Part.LineSegment(App.Vector(a[0], a[1], 0), App.Vector(b[0], b[1], 0)), False
            )
        self.assertInvalid(self.profile(s, -0.6))

    def testNestedIslandAndReversedWinding(self):
        s = self.body.newObject("Sketcher::SketchObject", "Nominal")
        # Rectangle construction has identical winding for the first two loops.
        self.rectangle(s, -10, -10, 10, 10)
        self.rectangle(s, -6, -6, 6, 6)
        self.rectangle(s, 2, -2, -2, 2)
        p = self.profile(s, 0.2)
        self.assertAlmostEqual(self.area(p), 20.4**2 - 11.6**2 + 4.4**2)

    def testValidConcaveProfile(self):
        s = self.body.newObject("Sketcher::SketchObject", "Nominal")
        points = [(0, 0), (5, 0), (5, 2), (2, 2), (2, 5), (0, 5)]
        for a, b in zip(points, points[1:] + points[:1]):
            s.addGeometry(
                Part.LineSegment(App.Vector(a[0], a[1], 0), App.Vector(b[0], b[1], 0)), False
            )
        for allowance in (0.1, -0.1):
            with self.subTest(allowance=allowance):
                p = self.profile(s, allowance)
                self.assertNotIn("Invalid", p.State)
                self.assertAlmostEqual(self.area(p), 16 + 20 * allowance + 4 * allowance**2)

    def testNarrowRecessCollapse(self):
        s = self.body.newObject("Sketcher::SketchObject", "Nominal")
        points = [(0, 0), (10, 0), (10, 10), (5.5, 10), (5.5, 5), (4.5, 5), (4.5, 10), (0, 10)]
        for a, b in zip(points, points[1:] + points[:1]):
            s.addGeometry(
                Part.LineSegment(App.Vector(a[0], a[1], 0), App.Vector(b[0], b[1], 0)), False
            )
        self.assertInvalid(self.profile(s, 0.6))

    def testNarrowAppendageOnLargeProfile(self):
        s = self.body.newObject("Sketcher::SketchObject", "Nominal")
        points = [
            (0, 0),
            (1000, 0),
            (1000, 499.95),
            (1000.5, 499.95),
            (1000.5, 500.05),
            (1000, 500.05),
            (1000, 1000),
            (0, 1000),
        ]
        for a, b in zip(points, points[1:] + points[:1]):
            s.addGeometry(
                Part.LineSegment(App.Vector(a[0], a[1], 0), App.Vector(b[0], b[1], 0)), False
            )
        self.assertInvalid(self.profile(s, -0.06))

    def testTinyDetailOnLargeProfileCollapses(self):
        from PartDesign.AllowanceProfile import _offset_face

        # Each lost detail has area 1e-4 mm2, below the perimeter-scaled
        # area tolerance of a 1000 mm square (about 4e-4 mm2).
        for depth, allowance in ((0.01, -0.006), (-0.01, 0.006)):
            with self.subTest(depth=depth, allowance=allowance):
                points = [
                    (0, 0), (1000, 0), (1000, 499.995),
                    (1000 + depth, 499.995), (1000 + depth, 500.005),
                    (1000, 500.005), (1000, 1000), (0, 1000),
                ]
                vertices = [App.Vector(x, y, 0) for x, y in points]
                face = Part.Face(Part.makePolygon(vertices + vertices[:1]))
                self.assertTrue(face.isValid())
                with self.assertRaisesRegex(ValueError, "collapses"):
                    _offset_face(face, allowance)

    def testOffsetErrorsAreTranslated(self):
        from unittest.mock import patch
        from PartDesign.AllowanceProfile import offsetProfile

        shape = Part.Wire([Part.makeCircle(5)])
        with patch.object(App.Qt, "translate", side_effect=lambda context, text: "TRANSLATED " + text) as translate:
            with self.assertRaisesRegex(ValueError, "TRANSLATED .*loop.*\\."):
                offsetProfile(shape, -6, App.Placement())
        self.assertTrue(translate.called)
        for call in translate.call_args_list:
            self.assertEqual(call.args[0], "PartDesign_AllowanceProfile")
            self.assertTrue(call.args[1][0].isupper())
            self.assertTrue(call.args[1].endswith("."))
