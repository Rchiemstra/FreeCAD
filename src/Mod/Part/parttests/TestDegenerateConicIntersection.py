# SPDX-License-Identifier: LGPL-2.1-or-later

"""2D curve intersection must not call OCCT on a zero-radius circle."""

import unittest

import FreeCAD
import Part

_ZERO = "Cannot intersect a circle or ellipse of zero radius"
_TWO_POINTS = [(-5.0, 0.0), (5.0, 0.0)]


def _circle2d(center, radius):
    constructed = 1.0 if radius == 0 else radius
    curve = Part.Geom2d.Circle2d(FreeCAD.Base.Vector2d(center[0], center[1]), constructed)
    if radius == 0:
        curve.Radius = 0
    return curve


class TestDegenerateConicIntersection(unittest.TestCase):
    def _assert_occ(self, call):
        with self.assertRaises(Part.OCCError) as caught:
            call()
        self.assertIs(type(caught.exception), Part.OCCError)
        self.assertEqual(str(caught.exception), _ZERO)

    def _assert_runtime(self, call):
        with self.assertRaises(RuntimeError) as caught:
            call()
        self.assertIs(type(caught.exception), RuntimeError)
        self.assertEqual(str(caught.exception), _ZERO)

    def _assert_two_points(self, points):
        # The y values are rounding noise whose last digits depend on libm and OCCT.
        self.assertEqual(len(points), len(_TWO_POINTS))
        for point, expected in zip(sorted(points), _TWO_POINTS):
            self.assertAlmostEqual(point[0], expected[0], places=9)
            self.assertAlmostEqual(point[1], expected[1], places=9)

    def test_intersect2d_zero_radius_circle_on_the_line(self):
        line = Part.LineSegment(FreeCAD.Vector(-10, 0, 0), FreeCAD.Vector(10, 0, 0))
        circle = Part.Circle(FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(0, 0, 1), 0)
        plane = Part.Plane(FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(0, 0, 1))
        self._assert_occ(lambda: line.intersect2d(circle, plane))

    def test_intersect2d_zero_radius_circle_off_the_line(self):
        line = Part.LineSegment(FreeCAD.Vector(-10, 0, 0), FreeCAD.Vector(10, 0, 0))
        circle = Part.Circle(FreeCAD.Vector(0, 5, 0), FreeCAD.Vector(0, 0, 1), 0)
        plane = Part.Plane(FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(0, 0, 1))
        self._assert_occ(lambda: line.intersect2d(circle, plane))

    def test_intersect2d_normal_circle_returns_two_points(self):
        line = Part.LineSegment(FreeCAD.Vector(-10, 0, 0), FreeCAD.Vector(10, 0, 0))
        circle = Part.Circle(FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(0, 0, 1), 5)
        plane = Part.Plane(FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(0, 0, 1))
        self._assert_two_points(line.intersect2d(circle, plane))

    def test_intersectCC_zero_radius_circle_on_the_line(self):
        line = Part.Geom2d.Line2dSegment(
            FreeCAD.Base.Vector2d(-10, 0), FreeCAD.Base.Vector2d(10, 0)
        )
        circle = _circle2d((0, 0), 0)
        self._assert_runtime(lambda: line.intersectCC(circle))

    def test_intersectCC_zero_radius_circle_off_the_line(self):
        line = Part.Geom2d.Line2dSegment(
            FreeCAD.Base.Vector2d(-10, 0), FreeCAD.Base.Vector2d(10, 0)
        )
        circle = _circle2d((0, 5), 0)
        self._assert_runtime(lambda: line.intersectCC(circle))

    def test_intersectCC_normal_circle_returns_two_points(self):
        line = Part.Geom2d.Line2dSegment(
            FreeCAD.Base.Vector2d(-10, 0), FreeCAD.Base.Vector2d(10, 0)
        )
        circle = _circle2d((0, 0), 5)
        points = [(point.x, point.y) for point in line.intersectCC(circle)]
        self._assert_two_points(points)
