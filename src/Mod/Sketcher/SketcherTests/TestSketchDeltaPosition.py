# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2021 Werner Mayer <werner.wm.mayer@gmx.de>              *
# *                                                                         *
# *   This file is part of the FreeCAD CAx development system.              *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU Lesser General Public License (LGPL)    *
# *   as published by the Free Software Foundation; either version 2 of     *
# *   the License, or (at your option) any later version.                   *
# *   for detail see the LICENCE text file.                                 *
# *                                                                         *
# *   FreeCAD is distributed in the hope that it will be useful,            *
# *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
# *   GNU Library General Public License for more details.                  *
# *                                                                         *
# *   You should have received a copy of the GNU Library General Public     *
# *   License along with FreeCAD; if not, write to the Free Software        *
# *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
# *   USA                                                                   *
# ***************************************************************************/


import unittest
import FreeCAD
import Part
import Sketcher

App = FreeCAD


class TestSketchDeltaPosition(unittest.TestCase):
    def setUp(self):
        self.Doc = FreeCAD.newDocument("TestSketchDeltaPosition")

    def testDeltaPositionConstraintCreatesSignedDistancePair(self):
        sketch = self.Doc.addObject("Sketcher::SketchObject", "Sketch")
        reference = sketch.addGeometry(Part.Point(App.Vector(1, 2, 0)), False)
        target = sketch.addGeometry(Part.Point(App.Vector(3, 4, 0)), False)

        sketch.addConstraint(
            [
                Sketcher.Constraint("DistanceX", reference, 1, 1),
                Sketcher.Constraint("DistanceY", reference, 1, 2),
            ]
        )

        delta_x, delta_y = sketch.addDeltaPositionConstraint(reference, 1, target, 1, 10, -5)
        self.assertEqual((delta_x, delta_y), sketch.getDeltaPositionConstraintPair(delta_x))
        self.assertEqual((delta_x, delta_y), sketch.getDeltaPositionConstraintPair(delta_y))
        self.assertEqual(((delta_x, delta_y),), sketch.getDeltaPositionConstraintPairs())

        self.assertEqual("DistanceX", sketch.Constraints[delta_x].Type)
        self.assertEqual("DistanceY", sketch.Constraints[delta_y].Type)
        self.assertEqual(reference, sketch.Constraints[delta_x].First)
        self.assertEqual(target, sketch.Constraints[delta_x].Second)
        self.assertEqual(reference, sketch.Constraints[delta_y].First)
        self.assertEqual(target, sketch.Constraints[delta_y].Second)

        self.assertEqual(0, sketch.solve())
        reference_point = sketch.getPoint(reference, 1)
        target_point = sketch.getPoint(target, 1)
        self.assertAlmostEqual(10, target_point.x - reference_point.x)
        self.assertAlmostEqual(-5, target_point.y - reference_point.y)

    def testDeltaPositionConstraintAcceptsExpressions(self):
        sketch = self.Doc.addObject("Sketcher::SketchObject", "Sketch")
        reference = sketch.addGeometry(Part.Point(App.Vector(0, 0, 0)), False)
        target = sketch.addGeometry(Part.Point(App.Vector(1, 1, 0)), False)

        sketch.addConstraint(
            [
                Sketcher.Constraint("DistanceX", reference, 1, 0),
                Sketcher.Constraint("DistanceY", reference, 1, 0),
            ]
        )

        delta_x, delta_y = sketch.addDeltaPositionConstraint(reference, 1, target, 1, 0, 0)
        sketch.renameConstraint(delta_x, "Width")
        sketch.setExpression("Constraints[{}]".format(delta_x), "10 mm")
        sketch.setExpression("Constraints[{}]".format(delta_y), ".Constraints.Width / 2")

        self.assertEqual(0, sketch.solve())
        reference_point = sketch.getPoint(reference, 1)
        target_point = sketch.getPoint(target, 1)
        self.assertAlmostEqual(10, target_point.x - reference_point.x)
        self.assertAlmostEqual(5, target_point.y - reference_point.y)

    def tearDown(self):
        # comment out to omit closing document for debugging
        FreeCAD.closeDocument(self.Doc.Name)
