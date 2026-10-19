# SPDX-License-Identifier: LGPL-2.1-or-later
import unittest
import FreeCAD as App
from SketcherTests.ExpressionDiagnosticsFixture import make_box


class TestSketchExpressionDiagnostics(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("SketchExpressionDiagnostics")
        self.sketch = make_box(self.doc)
        self.doc.recompute()

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def findings(self):
        from freecad.sketcher_expression_diagnostics import constraint_errors
        return constraint_errors(self.sketch)

    def test_healthy_expressions_are_not_errors(self):
        self.assertEqual(self.findings(), {})

    def test_deleted_varset_identifies_both_named_constraints(self):
        self.doc.removeObject("Dimensions")
        errors = self.findings()
        self.assertEqual(set(errors), {8, 9})
        for reason in errors.values():
            self.assertIn("Dimensions", reason)

    def test_unnamed_constraint_and_unit_error(self):
        self.sketch.renameConstraint(8, "")
        self.sketch.setExpression("Constraints[8]", "1 mm + 1 s")
        self.assertEqual(set(self.findings()), {8})

    def test_healthy_conditional_with_missing_inactive_branch(self):
        self.sketch.setExpression("Constraints.Width", "0 ? Missing.Width : 20 mm")
        self.assertEqual(self.findings(), {})

    def test_stored_value_pending_recompute_is_not_a_broken_expression(self):
        self.doc.Dimensions.Width = 25
        self.assertEqual(self.findings(), {})

    def test_upstream_failure_marks_affected_constraint(self):
        self.doc.Dimensions.setExpression("Width", "Missing.Value")
        self.assertEqual(set(self.findings()), {8})
        self.assertIn("Missing", self.findings()[8])

    def test_multiple_upstream_causes_do_not_duplicate_the_constraint(self):
        self.doc.Dimensions.setExpression("Width", "MissingWidth.Value")
        self.doc.Dimensions.setExpression("Height", "MissingHeight.Value")
        self.sketch.setExpression("Constraints.Width", "Dimensions.Width + Dimensions.Height")
        errors = self.findings()
        self.assertEqual(set(errors), {8, 9})
        self.assertIn("MissingWidth", errors[8])
        self.assertIn("MissingHeight", errors[8])

    def test_failure_in_feature_output_dependency_is_retained(self):
        source = self.doc.addObject("App::FeaturePython", "Source")
        source.addProperty("App::PropertyFloat", "Value")
        source.addProperty("App::PropertyVector", "Output")
        source.Output = App.Vector(20, 0, 0)
        source.setExpression("Value", "Missing.Value")
        self.sketch.setExpression("Constraints.Width", "Source.Output.x")
        self.assertEqual(set(self.findings()), {8})
        self.assertIn("Missing", self.findings()[8])
