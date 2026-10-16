# SPDX-License-Identifier: LGPL-2.1-or-later

"""Failed sketch edits must raise Python exceptions, never escape as C++ ones.

Trimming the second of two crossing lines at a point far off the curve makes
OpenCASCADE throw ``StdFail_NotDone``. The binding let it escape through the
Python interpreter; debug builds terminated FreeCAD ("terminate called after
throwing an instance of 'StdFail_NotDone'") and release builds only reported
"Unknown C++ exception". A live MCP stress run lost the whole GUI this way.
"""

import unittest

import FreeCAD
import Part
import Sketcher


class TestSketchEditFailures(unittest.TestCase):
    def setUp(self):
        self.doc = FreeCAD.newDocument("SketchEditFailures")
        self.sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        v = FreeCAD.Vector
        self.sketch.addGeometry(Part.LineSegment(v(-50, 0, 0), v(50, 0, 0)))
        self.sketch.addGeometry(Part.LineSegment(v(0, -40, 0), v(0, 40, 0)))
        self.doc.recompute()

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)

    def test_trim_far_from_the_curve_raises_a_python_error(self):
        self.sketch.trim(0, FreeCAD.Vector(25, 0, 0))

        with self.assertRaises(Exception) as raised:
            self.sketch.trim(1, FreeCAD.Vector(900, 900, 0))

        self.assertNotIn("Unknown C++ exception", str(raised.exception))

    def test_sketch_stays_usable_after_a_failed_trim(self):
        self.sketch.trim(0, FreeCAD.Vector(25, 0, 0))
        with self.assertRaises(Exception):
            self.sketch.trim(1, FreeCAD.Vector(900, 900, 0))

        self.sketch.trim(1, FreeCAD.Vector(0, 20, 0))

        self.assertTrue(self.doc.recompute() >= 0)
        self.assertEqual(self.sketch.solve(), 0)

    def test_unsolvable_datum_is_not_reported_as_a_bad_index(self):
        """setDatum mapped every solver failure to "Invalid constraint index"."""

        width = self.sketch.addConstraint(Sketcher.Constraint("Distance", 0, 100))
        self.sketch.renameConstraint(width, "Width")
        self.doc.recompute()

        with self.assertRaises(ValueError) as raised:
            self.sketch.setDatum("Width", FreeCAD.Units.Quantity("-45 mm"))

        self.assertNotIn("Invalid constraint index", str(raised.exception))
        self.assertIn("Negative datum", str(raised.exception))
        self.assertAlmostEqual(self.sketch.Constraints[width].Value, 100)
        # Rejected before solving: the solver never collapsed the line.
        line = self.sketch.Geometry[0]
        self.assertAlmostEqual(line.length(), 100)

    def test_out_of_range_constraint_index_is_reported_as_such(self):
        with self.assertRaises(ValueError) as raised:
            self.sketch.setDatum(99, FreeCAD.Units.Quantity("5 mm"))

        self.assertIn("Invalid constraint index", str(raised.exception))
