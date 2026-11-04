# SPDX-License-Identifier: LGPL-2.1-or-later

"""Trim beside a radius-0 arc must return and leave a definite geometry.

OCCT 8 does not return from the 2D conic intersector on this input, and OCCT
7.6 throws. These tests lock the geometry FreeCAD keeps after the guard.
"""

import math
import unittest

import FreeCAD
import Part

_ARC_U0 = 1.57079632679
_ARC_U1 = 4.71238898038


def _assert_xyz(test, point, expected):
    # Computed coordinates carry rounding noise that depends on libm and OCCT.
    for actual, wanted in zip((point.x, point.y, point.z), expected):
        test.assertAlmostEqual(actual, wanted, places=9)


def _live_sketch(doc):
    sketch = doc.addObject("Sketcher::SketchObject", "Sketch")
    sketch.addGeometry(Part.LineSegment(FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(20, 0, 0)), False)
    sketch.addGeometry(Part.LineSegment(FreeCAD.Vector(20, 0, 0), FreeCAD.Vector(0, 0, 0)), False)
    sketch.addGeometry(
        Part.ArcOfCircle(
            Part.Circle(FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(0, 0, 1), 0),
            _ARC_U0,
            _ARC_U1,
        ),
        False,
    )
    return sketch


def _assert_line(test, geo, start, end):
    test.assertIsInstance(geo, Part.LineSegment)
    _assert_xyz(test, geo.StartPoint, start)
    _assert_xyz(test, geo.EndPoint, end)


def _assert_zero_arc(test, geo):
    test.assertIsInstance(geo, Part.ArcOfCircle)
    _assert_xyz(test, geo.Center, (0.0, 0.0, 0.0))
    test.assertEqual(geo.Radius, 0.0)
    _assert_xyz(test, geo.Axis, (0.0, 0.0, 1.0))
    test.assertAlmostEqual(geo.FirstParameter, _ARC_U0, places=9)
    test.assertAlmostEqual(geo.LastParameter, _ARC_U1, places=9)
    _assert_xyz(test, geo.StartPoint, (0.0, 0.0, 0.0))
    _assert_xyz(test, geo.EndPoint, (0.0, 0.0, 0.0))


class TestSketchTrimDegenerate(unittest.TestCase):
    def setUp(self):
        self.doc = FreeCAD.newDocument("TrimDegenerate")

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)

    def test_trim_line_beside_zero_radius_arc(self):
        sketch = _live_sketch(self.doc)
        self.assertEqual(len(sketch.Geometry), 3)

        self.assertIsNone(sketch.trim(0, FreeCAD.Vector(5, 5, 0)))

        self.assertEqual(len(sketch.Geometry), 2)
        _assert_line(self, sketch.Geometry[0], (20.0, 0.0, 0.0), (0.0, 0.0, 0.0))
        _assert_zero_arc(self, sketch.Geometry[1])

    def test_trim_the_zero_radius_arc(self):
        sketch = _live_sketch(self.doc)

        self.assertIsNone(sketch.trim(2, FreeCAD.Vector(0, 0, 0)))

        self.assertEqual(len(sketch.Geometry), 2)
        _assert_line(self, sketch.Geometry[0], (0.0, 0.0, 0.0), (20.0, 0.0, 0.0))
        _assert_line(self, sketch.Geometry[1], (20.0, 0.0, 0.0), (0.0, 0.0, 0.0))

    def test_trim_line_crossing_a_real_arc(self):
        sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        sketch.addGeometry(
            Part.LineSegment(FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(20, 0, 0)), False
        )
        sketch.addGeometry(
            Part.ArcOfCircle(
                Part.Circle(FreeCAD.Vector(10, 0, 0), FreeCAD.Vector(0, 0, 1), 6),
                0,
                math.pi,
            ),
            False,
        )

        self.assertIsNone(sketch.trim(0, FreeCAD.Vector(10, 5, 0)))

        self.assertEqual(len(sketch.Geometry), 3)
        _assert_line(self, sketch.Geometry[0], (0.0, 0.0, 0.0), (4.0, 0.0, 0.0))
        arc = sketch.Geometry[1]
        self.assertIsInstance(arc, Part.ArcOfCircle)
        _assert_xyz(self, arc.Center, (10.0, 0.0, 0.0))
        self.assertAlmostEqual(arc.Radius, 6.0, places=9)
        _assert_xyz(self, arc.Axis, (0.0, 0.0, 1.0))
        self.assertAlmostEqual(arc.FirstParameter, 0.0, places=9)
        self.assertAlmostEqual(arc.LastParameter, math.pi, places=9)
        _assert_xyz(self, arc.StartPoint, (16.0, 0.0, 0.0))
        _assert_xyz(self, arc.EndPoint, (4.0, 0.0, 0.0))
        _assert_line(self, sketch.Geometry[2], (16.0, 0.0, 0.0), (20.0, 0.0, 0.0))
