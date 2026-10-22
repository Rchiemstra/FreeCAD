# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise Sketcher clipboard expressions through the real edit-mode commands."""

from unittest.mock import patch

import FreeCAD as App
import FreeCADGui as Gui
import Part
import Sketcher
from PySide import QtGui

from SketcherTests.GuiTestCase import SketcherGuiTestCase


class TestSketchClipboardGui(SketcherGuiTestCase):
    def setUp(self):
        super().setUp()
        Gui.activateWorkbench("SketcherWorkbench")
        self.doc = App.newDocument("SketchClipboard")
        self.doc.UndoMode = 1
        clipboard = QtGui.QApplication.clipboard()
        previous = clipboard.text()
        self.addCleanup(clipboard.setText, previous)
        self.sketch = self.make_sketch()

    def make_sketch(self, name="Sketch", document=None):
        sketch = (document or self.doc).addObject("Sketcher::SketchObject", name)
        sketch.addGeometry([
            Part.LineSegment(App.Vector(), App.Vector(20, 0, 0)),
            Part.LineSegment(App.Vector(0, 10, 0), App.Vector(12, 10, 0)),
        ], False)
        sketch.addConstraint(Sketcher.Constraint("Horizontal", 0))
        sketch.addConstraint(Sketcher.Constraint("Distance", 1, 12.0))
        return sketch

    def copy(self, edges=("Edge2",)):
        self.recompute_gui_document()
        self.assertTrue(Gui.activeDocument().setEdit(self.sketch.Name))
        self.flush_gui()
        Gui.Selection.clearSelection()
        for edge in edges:
            Gui.Selection.addSelection(self.sketch, edge)
        Gui.runCommand("Sketcher_CopyClipboard")
        text = QtGui.QApplication.clipboard().text()
        self.assertTrue(text.startswith("# Copied from sketcher."))
        return text

    def paste(self, target=None):
        target = target or self.sketch
        count, geometries = target.ConstraintCount, target.GeometryCount
        undo_count = target.Document.UndoCount
        Gui.runCommand("Sketcher_Paste")
        self.flush_gui()
        self.assertEqual(target.GeometryCount, geometries + 1)
        self.assertEqual(target.ConstraintCount, count + 1)
        self.assertEqual(target.Document.UndoCount, undo_count + 1)
        self.assertAlmostEqual(target.getDatum(count).Value, 12.0)
        return count

    def assert_expression(self, target, index, expression):
        expressions = {path.lstrip("."): value for path, value in target.ExpressionEngine}
        self.assertEqual(expressions.get(f"Constraints[{index}]"), expression)

    def test_spreadsheet_alias_and_undo(self):
        sheet = self.doc.addObject("Spreadsheet::Sheet", "Spreadsheet")
        sheet.set("A1", "12 mm")
        sheet.setAlias("A1", "width")
        self.recompute_gui_document()
        self.sketch.renameConstraint(1, "Width")
        self.sketch.setExpression("Constraints.Width", "Spreadsheet.width")
        self.copy()
        before = self.sketch.ExpressionEngine
        count = self.paste()
        self.assert_expression(self.sketch, count, "Spreadsheet.width")
        self.undo_gui_document()
        self.assertEqual(self.sketch.GeometryCount, 2)
        self.assertEqual(self.sketch.ConstraintCount, 2)
        self.assertEqual(self.sketch.ExpressionEngine, before)

    def test_varset_and_repeated_paste_offset(self):
        variables = self.doc.addObject("App::VarSet", "Variables")
        variables.addProperty("App::PropertyLength", "width")
        variables.width = 12
        self.sketch.setExpression("Constraints[1]", "Variables.width")
        self.copy()
        for _ in range(2):
            count = self.paste()
            self.assert_expression(self.sketch, count, "Variables.width")

    def test_same_sketch_named_constraint(self):
        driver = self.sketch.addConstraint(Sketcher.Constraint("Distance", 0, 12.0))
        self.sketch.renameConstraint(driver, "Driver")
        self.sketch.setExpression("Constraints[1]", ".Constraints.Driver")
        self.copy()
        count = self.paste()
        self.assert_expression(self.sketch, count, ".Constraints.Driver")

    def test_copied_names_are_not_remapped(self):
        driver = self.sketch.addConstraint(Sketcher.Constraint("Distance", 0, 12.0))
        self.sketch.renameConstraint(driver, "Driver")
        self.sketch.setExpression("Constraints[1]", ".Constraints.Driver")
        self.copy(("Edge1", "Edge2"))
        Gui.runCommand("Sketcher_Paste")
        self.flush_gui()
        self.assertEqual(self.sketch.ConstraintCount, 6)
        self.assertEqual(self.sketch.Constraints[5].Name, "")
        self.assert_expression(self.sketch, 4, ".Constraints.Driver")
        self.assertEqual(self.sketch.Constraints[2].Name, "Driver")

    def test_expression_with_quoted_unicode_label(self):
        variables = self.doc.addObject("App::VarSet", "Variables")
        variables.Label = "Dimensions % ' \u00b5"
        variables.addProperty("App::PropertyLength", "width")
        variables.width = 12
        expression = "<<Dimensions % ' \u00b5>>.width"
        self.sketch.setExpression("Constraints[1]", expression)
        expression = dict(self.sketch.ExpressionEngine)["Constraints[1]"]
        self.copy()
        count = self.paste()
        self.assert_expression(self.sketch, count, expression)

    def test_constraint_without_expression(self):
        self.copy()
        self.paste()
        self.assertEqual(self.sketch.ExpressionEngine, [])

    def test_legacy_clipboard(self):
        self.sketch.setExpression("Constraints[1]", "12 mm")
        text = self.copy()
        # Everything through addConstraint is the pre-expression clipboard format.
        text = text[:text.index("# Restore constraint expressions")] if (
            "# Restore constraint expressions" in text
        ) else text
        text = text.replace("lastConstraintId = objectStr.ConstraintCount\n", "")
        QtGui.QApplication.clipboard().setText(text)
        before = self.sketch.ExpressionEngine
        self.paste()
        self.assertEqual(self.sketch.ExpressionEngine, before)

    def test_unknown_object_warns_once_and_keeps_value(self):
        variables = self.doc.addObject("App::VarSet", "Variables")
        variables.addProperty("App::PropertyLength", "width")
        variables.width = 12
        self.sketch.renameConstraint(1, "Width")
        self.sketch.setExpression("Constraints.Width", "Variables.width")
        self.copy()
        Gui.activeDocument().resetEdit()
        other = App.newDocument("ClipboardTarget")
        self.addCleanup(self.cleanup_gui_document, other)
        target = self.make_sketch(document=other)
        self.recompute_gui_document(other)
        self.assertTrue(Gui.activeDocument().setEdit(target.Name))
        self.flush_gui()
        with patch.object(App.Console, "PrintWarning") as warning:
            self.paste(target)
        warning.assert_called_once()
        self.assertIn("Width", warning.call_args.args[0])
        self.assertEqual(target.ExpressionEngine, [])
        self.undo_gui_document(other)
        self.assertEqual(target.ConstraintCount, 2)
        self.assertEqual(target.GeometryCount, 2)

    def test_cycle_warns_once_and_keeps_value(self):
        driver = self.sketch.addConstraint(Sketcher.Constraint("Distance", 0, 12.0))
        self.sketch.setExpression("Constraints[1]", f"Constraints[{driver}]")
        self.copy()
        Gui.activeDocument().resetEdit()
        target = self.make_sketch("Target")
        self.recompute_gui_document()
        self.assertTrue(Gui.activeDocument().setEdit(target.Name))
        self.flush_gui()
        with patch.object(App.Console, "PrintWarning") as warning:
            count = self.paste(target)
        warning.assert_called_once()
        self.assertIn(f"Constraints[{count}]", warning.call_args.args[0])
        self.assertEqual(target.ExpressionEngine, [])
