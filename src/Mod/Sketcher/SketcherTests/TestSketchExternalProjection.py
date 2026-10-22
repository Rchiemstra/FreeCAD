# SPDX-License-Identifier: LGPL-2.1-or-later

import unittest

import FreeCAD as App
import Part
import Sketcher


class TestSketchExternalProjection(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TestSketchExternalProjection")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def spline(self, poles):
        curve = Part.BSplineCurve()
        curve.buildFromPolesMultsKnots(
            [App.Vector(*p) for p in poles], [4, 4], [0.0, 1.0], False, 3
        )
        return curve

    def importShape(self, shape, subelement, placement=None):
        source = self.doc.addObject("Part::Feature", "Source")
        source.Shape = shape
        sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        if placement is not None:
            source.Placement = placement
            sketch.Placement = placement
        sketch.addExternal(source.Name, subelement)
        self.doc.recompute()
        return sketch.ExternalGeo[2:]

    def assertSegment(self, geometry, start, end):
        start, end = App.Vector(*start), App.Vector(*end)
        a, b = geometry.StartPoint, geometry.EndPoint
        error = min(
            max((a - start).Length, (b - end).Length),
            max((a - end).Length, (b - start).Length),
        )
        self.assertLess(error, 1e-6)
        self.assertAlmostEqual(geometry.toShape().Length, (end - start).Length, places=6)

    def assertPlaneNormal(self, edge, placement, expected):
        located = edge.copy()
        located.Placement = placement
        plane = located.findPlane()
        self.assertIsNotNone(plane)
        normal = placement.Rotation.inverted().multVec(plane.Axis)
        self.assertGreater(normal.dot(App.Vector(*expected)), 1 - 1e-12)

    def testEdgeOnSplineKeepsDiagonalProjection(self):
        # #29677(a): averaging Y must happen before undoing the alignment rotation.
        # A half-turn covers the opposite normal; pole reversal alone may not.
        placements = (
            (App.Placement(), 1),
            (App.Placement(App.Vector(7, -3, 11), App.Rotation(23, 41, 17)), 1),
            (App.Placement(App.Vector(), App.Rotation(App.Vector(0, 0, 1), 180)), -1),
        )
        for slope in (1, -1):
            for placement, sign in placements:
                for reverse_poles in (False, True):
                    for reverse_edge in (False, True):
                        with self.subTest(
                            slope=slope, placement=placement, sign=sign,
                            reverse_poles=reverse_poles, reverse_edge=reverse_edge,
                        ):
                            poles = [(3, slope * 4, 2), (6, slope * 8, 6),
                                     (9, slope * 12, -2), (12, slope * 16, 2)]
                            edge = self.spline(poles[::-1] if reverse_poles else poles).toShape()
                            if reverse_edge:
                                edge.reverse()
                            self.assertPlaneNormal(
                                edge, placement, (sign * 0.8, -sign * slope * 0.6, 0)
                            )
                            geos = self.importShape(edge, "Edge1", placement)
                            self.assertEqual(len(geos), 1)
                            self.assertIsInstance(geos[0], Part.LineSegment)
                            self.assertSegment(geos[0], (3, slope * 4, 0), (12, slope * 16, 0))

    def testTrimmedNonplanarFaceKeepsBoundary(self):
        # #29677(b): HLR exposes the untrimmed supporting B-spline of a face edge.
        lower = self.spline([(0, 0, 0), (5, 0, 1), (10, 0, 2), (15, 0, 0)])
        upper = self.spline([(0, 10, 0), (5, 10, 2), (10, 10, 4), (15, 10, 0)])
        face = Part.makeRuledSurface(lower.toShape(0.2, 0.8), upper.toShape(0.2, 0.8))
        self.assertIsInstance(face.Surface, Part.BSplineSurface)
        expected = [
            ((3, 0, 0), (12, 0, 0)), ((12, 0, 0), (12, 10, 0)),
            ((3, 10, 0), (12, 10, 0)), ((3, 0, 0), (3, 10, 0)),
        ]
        geos = self.importShape(face, "Face1")
        self.assertEqual(len(geos), len(expected))
        # Match by midpoint so the test does not depend on OCC's edge order.
        for start, end in expected:
            midpoint = (App.Vector(*start) + App.Vector(*end)) / 2
            geo = min(geos, key=lambda g: ((g.StartPoint + g.EndPoint) / 2 - midpoint).Length)
            self.assertSegment(geo, start, end)
            geos.remove(geo)

    def testEdgeOnSplineRounding25720(self):
        # Keep axis-aligned projections exact despite bounding-box/rotation rounding.
        placements = (
            (App.Placement(), 1),
            (App.Placement(App.Vector(), App.Rotation(App.Vector(0, 0, 1), 180)), -1),
        )
        for x1, x2, x3, maximum in ((3, 8, 11, 11), (21, 21, 1, 16)):
            for vertical in (False, True):
                for placement, sign in placements:
                    for reverse_poles in (False, True):
                        for reverse_edge in (False, True):
                            with self.subTest(
                                maximum=maximum, vertical=vertical, sign=sign,
                                reverse_poles=reverse_poles, reverse_edge=reverse_edge,
                            ):
                                # The second curve doubles back; include its interior extremum.
                                poles = [(1, 4, 0), (x1, 4 + 1e-14, 6),
                                         (x2, 4 - 1e-14, -2), (x3, 4, 0)]
                                start, end = (1, 4, 0), (maximum, 4, 0)
                                normal = (0, sign, 0)
                                if vertical:
                                    poles = [(y, x, z) for x, y, z in poles]
                                    start, end = (4, 1, 0), (4, maximum, 0)
                                    normal = (sign, 0, 0)
                                curve = self.spline(poles[::-1] if reverse_poles else poles)
                                edge = curve.toShape()
                                if reverse_edge:
                                    edge.reverse()
                                self.assertPlaneNormal(edge, placement, normal)
                                geos = self.importShape(edge, "Edge1", placement)
                                self.assertEqual(len(geos), 1)
                                self.assertIsInstance(geos[0], Part.LineSegment)
                                self.assertSegment(geos[0], start, end)
                                if vertical:
                                    self.assertEqual(geos[0].StartPoint.x, geos[0].EndPoint.x)
                                else:
                                    self.assertEqual(geos[0].StartPoint.y, geos[0].EndPoint.y)
