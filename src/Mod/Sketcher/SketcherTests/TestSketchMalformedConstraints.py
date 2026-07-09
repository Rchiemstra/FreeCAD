# SPDX-License-Identifier: LGPL-2.1-or-later

# **************************************************************************
#   Copyright (c) 2026 The FreeCAD project                                 *
#                                                                          *
#   This file is part of the FreeCAD CAx development system.               *
#                                                                          *
#   FreeCAD is free software: you can redistribute it and/or modify it     *
#   under the terms of the GNU Lesser General Public License as            *
#   published by the Free Software Foundation, either version 2.1 of the   *
#   License, or (at your option) any later version.                        *
#                                                                          *
#   FreeCAD is distributed in the hope that it will be useful, but         *
#   WITHOUT ANY WARRANTY; without even the implied warranty of             *
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
#   Lesser General Public License for more details.                        *
#                                                                          *
#   You should have received a copy of the GNU Lesser General Public       *
#   License along with FreeCAD. If not, see                                *
#   <https://www.gnu.org/licenses/>.                                       *
# **************************************************************************

"""Regression coverage for P11 (doc/mcp-feedback.md): the 1-point DistanceX
constraint form ``Sketcher.Constraint('DistanceX', geoId, value)`` is valid
for Line geometry (PointPos::none is the documented encoding for "length of a
line", see Sketch.cpp) but semantically meaningless for a Point geometry.
Empirically confirmed against a real build: applying it to a Point used to be
accepted silently by ``addConstraint`` -- the sketch only became Invalid on
the *next* recompute, with an opaque "Sketch with malformed constraints"
message that never explained why. ``SketchObjectPyImp::addConstraint`` now
checks the immediate post-add solve for malformed constraints and raises
right away, naming the offending constraint.
"""

import unittest

import FreeCAD
import Part
import Sketcher

App = FreeCAD


class TestSketchMalformedConstraints(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("SketchMalformedConstraintsTest")

    def testOnePointDistanceXOnLineIsValid(self):
        """The 1-point DistanceX form on a Line geometry is the documented
        "horizontal length of a line" encoding and must keep working -- this
        is NOT the P11 bug (an earlier xfail-style repro incorrectly targeted
        this exact scenario; it was never actually broken)."""
        sketch = self.Doc.addObject("Sketcher::SketchObject", "Sketch")
        sketch.addGeometry(Part.LineSegment(App.Vector(0, 0, 0), App.Vector(10, 0, 0)), False)
        self.Doc.recompute()

        idx = sketch.addConstraint(Sketcher.Constraint("DistanceX", 0, 5.0))
        self.Doc.recompute()

        self.assertGreaterEqual(idx, 0)
        self.assertNotIn("Invalid", sketch.State)
        names = [getattr(con, "Name", "") for con in sketch.Constraints]
        self.assertEqual(len(names), 1)

    def testOnePointDistanceXOnPointRaisesImmediately(self):
        """The real P11 papercut: 1-point DistanceX applied to a Point
        geometry is structurally malformed (Sketch::addDistanceXConstraint
        requires a Line for the PointPos::none form) and can never become
        valid by adding more constraints. addConstraint() must now reject it
        immediately instead of silently accepting it and deferring the
        failure to the next document recompute."""
        sketch = self.Doc.addObject("Sketcher::SketchObject", "Sketch")
        sketch.addGeometry(Part.Point(App.Vector(3, 4, 0)), False)
        self.Doc.recompute()

        with self.assertRaises(ValueError):
            sketch.addConstraint(Sketcher.Constraint("DistanceX", 0, 5.0))

    def testRadiusAndCoincidentFormsStillWork(self):
        """Other documented multi-argument constructor forms (unaffected by
        this fix) keep working: Radius (geoId, value) and Coincident
        (geoId1, pos1, geoId2, pos2)."""
        sketch = self.Doc.addObject("Sketcher::SketchObject", "Sketch")
        sketch.addGeometry(Part.Circle(App.Vector(0, 0, 0), App.Vector(0, 0, 1), 2), False)
        sketch.addGeometry(Part.LineSegment(App.Vector(0, 0, 0), App.Vector(10, 0, 0)), False)
        self.Doc.recompute()

        sketch.addConstraint(Sketcher.Constraint("Radius", 0, 2.0))
        sketch.addConstraint(Sketcher.Constraint("Coincident", 1, 1, 0, 3))
        self.Doc.recompute()

        types = {con.Type for con in sketch.Constraints}
        self.assertNotIn("Invalid", sketch.State)
        self.assertEqual(types, {"Radius", "Coincident"})

    def tearDown(self):
        FreeCAD.closeDocument("SketchMalformedConstraintsTest")
