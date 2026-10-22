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

    def testEdgeOnSplineKeepsDiagonalProjection(self):
        # #29677(a): averaging Y must happen before undoing the alignment rotation.
        placements = (
            App.Placement(),
            App.Placement(App.Vector(7, -3, 11), App.Rotation(23, 41, 17)),
        )
        for slope in (1, -1):
            for placement in placements:
                with self.subTest(slope=slope, placement=placement):
                    edge = self.spline(
                        [(3, slope * 4, 2), (6, slope * 8, 6),
                         (9, slope * 12, -2), (12, slope * 16, 2)]
                    ).toShape()
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
        # Rebuild d0b98703c0's case: a nominally horizontal edge-on spline whose
        # bounding box has a tiny Y extent from rounding. Keep both ends at one Y.
        for x1, x2, x3, maximum in ((3, 8, 11, 11), (21, 21, 1, 16)):
            with self.subTest(maximum=maximum):
                # The second curve doubles back: PR 19700's bounding-box workaround
                # must include its interior extremum, even though its ends coincide.
                edge = self.spline(
                    [(1, 4, 0), (x1, 4 + 1e-14, 6), (x2, 4 - 1e-14, -2), (x3, 4, 0)]
                ).toShape()
                geos = self.importShape(edge, "Edge1")
                self.assertEqual(len(geos), 1)
                self.assertIsInstance(geos[0], Part.LineSegment)
                self.assertSegment(geos[0], (1, 4, 0), (maximum, 4, 0))
                self.assertEqual(geos[0].StartPoint.y, geos[0].EndPoint.y)
