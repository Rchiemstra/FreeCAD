# SPDX-License-Identifier: LGPL-2.1-or-later
"""Command creation, cancellation and document undo for allowance profiles."""

import unittest

import FreeCAD as App
import FreeCADGui as Gui
import Part
from Test.GuiRecompute import close_document, recompute_document, undo_document, redo_document


class TestAllowanceProfileGui(unittest.TestCase):
    def setUp(self):
        Gui.activateWorkbench("PartDesignWorkbench")
        self.doc = App.newDocument("TestAllowanceProfileGui")
        self.doc.UndoMode = 1
        self.body = self.doc.addObject("PartDesign::Body", "Body")
        Gui.activeDocument().activeView().setActiveObject("pdbody", self.body)
        self.sketch = self.body.newObject("Sketcher::SketchObject", "Nominal")
        self.sketch.addGeometry(Part.Circle(App.Vector(), App.Vector(0, 0, 1), 5), False)
        recompute_document(self.doc)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.sketch)

    def tearDown(self):
        if Gui.Control.activeDialog():
            Gui.activeDocument().resetEdit()
            if self.doc.HasPendingTransaction:
                self.doc.abortTransaction()
        Gui.Selection.clearSelection()
        close_document(self.doc)

    def create(self):
        Gui.runCommand("PartDesign_AllowanceProfile")
        p = self.doc.getObject("AllowanceProfile")
        self.assertIsNotNone(p)
        self.assertIn(p, self.body.Group)
        self.assertIsNotNone(Gui.Control.activeDialog())
        self.assertFalse(self.sketch.Visibility)
        return p, p.ViewObject.Proxy.panel

    def testCommandAndUndo(self):
        p, panel = self.create()
        panel.input.setText("0.2 mm")
        self.assertTrue(panel.accept())
        recompute_document(self.doc)
        self.assertAlmostEqual(p.Shape.Edges[0].Curve.Radius, 5.2)
        undo_document(self.doc)
        recompute_document(self.doc)
        self.assertIsNone(self.doc.getObject("AllowanceProfile"))
        self.assertTrue(self.sketch.Visibility)
        redo_document(self.doc)
        recompute_document(self.doc)
        self.assertAlmostEqual(self.doc.AllowanceProfile.Shape.Edges[0].Curve.Radius, 5.2)

    def testWorkbenchMenuAndReactivation(self):
        from PySide import QtGui

        for workbench in ("PartDesignWorkbench", "PartWorkbench", "PartDesignWorkbench"):
            Gui.activateWorkbench(workbench)
        menu = Gui.getMainWindow().findChild(QtGui.QMenu, "&Part Design")
        self.assertIsNotNone(menu)
        actions = [a for a in menu.actions() if a.objectName() == "PartDesign_AllowanceProfile"]
        self.assertEqual(len(actions), 1)

    def testCancelCreation(self):
        p, panel = self.create()
        panel.input.setText("-0.1 mm")
        self.assertTrue(panel.reject())
        self.assertIsNone(self.doc.getObject("AllowanceProfile"))
        self.assertTrue(self.sketch.Visibility)

    def testCancelEditing(self):
        p, panel = self.create()
        panel.input.setText("0.2 mm")
        self.assertTrue(panel.accept())
        Gui.activeDocument().setEdit(p.Name)
        panel = p.ViewObject.Proxy.panel
        panel.input.setText("0.4 mm")
        self.assertTrue(panel.reject())
        self.assertAlmostEqual(p.Allowance.Value, 0.2)

    def testCommandRequiresSketchInActiveBody(self):
        from PartDesign.AllowanceProfileGui import CommandAllowanceProfile

        command = CommandAllowanceProfile()
        other = self.doc.addObject("PartDesign::Body", "OtherBody")
        Gui.activeDocument().activeView().setActiveObject("pdbody", other)
        self.assertFalse(command.IsActive())
        Gui.activeDocument().activeView().setActiveObject("pdbody", self.body)
        self.assertTrue(command.IsActive())

    def testInvalidAllowanceKeepsTaskOpen(self):
        p, panel = self.create()
        panel.input.setText("-6 mm")
        self.assertFalse(panel.accept())
        self.assertIsNotNone(Gui.Control.activeDialog())
        self.assertIn("allowance", panel.error.text().lower())
        panel.input.setText("-0.1 mm")
        self.assertTrue(panel.accept())
        recompute_document(self.doc)
        self.assertAlmostEqual(p.Shape.Edges[0].Curve.Radius, 4.9)

    def testEditPreservesExpression(self):
        p, panel = self.create()
        self.assertTrue(panel.accept())
        recompute_document(self.doc)
        p.setExpression("Allowance", "0.2 mm")
        recompute_document(self.doc)
        Gui.activeDocument().setEdit(p.Name)
        self.assertTrue(p.ViewObject.Proxy.panel.accept())
        recompute_document(self.doc)
        self.assertEqual(dict(p.ExpressionEngine)["Allowance"], "0.2 mm")
        self.assertAlmostEqual(p.Shape.Edges[0].Curve.Radius, 5.2)

    def testIncompleteInputKeepsTaskOpen(self):
        p, panel = self.create()
        panel.input.setText("0.2 mm")
        panel.input.setText("-")
        self.assertFalse(panel.accept())
        self.assertIsNotNone(Gui.Control.activeDialog())
        self.assertTrue(self.doc.HasPendingTransaction)
        self.assertTrue(panel.error.text())
        panel.input.setText("-0.1 mm")
        self.assertTrue(panel.accept())
        recompute_document(self.doc)
        self.assertAlmostEqual(p.Shape.Edges[0].Curve.Radius, 4.9)

    def testIncompleteInputWithBoundExpression(self):
        p, panel = self.create()
        self.assertTrue(panel.accept())
        recompute_document(self.doc)
        p.setExpression("Allowance", "0.2 mm")
        recompute_document(self.doc)
        Gui.activeDocument().setEdit(p.Name)
        panel = p.ViewObject.Proxy.panel
        transaction_pending = self.doc.HasPendingTransaction
        panel.input.setText("-")
        self.assertFalse(panel.accept())
        self.assertIsNotNone(Gui.Control.activeDialog())
        self.assertEqual(self.doc.HasPendingTransaction, transaction_pending)
        self.assertTrue(panel.error.text())
        self.assertEqual(dict(p.ExpressionEngine)["Allowance"], "0.2 mm")
        panel.input.setText("0.3 mm + 0.1 mm")
        current_expression = dict(p.ExpressionEngine)["Allowance"]
        self.assertTrue(panel.accept())
        recompute_document(self.doc)
        self.assertEqual(dict(p.ExpressionEngine)["Allowance"], current_expression)
        self.assertAlmostEqual(p.Shape.Edges[0].Curve.Radius, 5.4)

    def testPocketDialogFittingTolerance(self):
        from PySide import QtCore, QtGui
        from Test.GuiRecompute import _left_commit, _pump_gui_until

        def wait_for_preview():
            # The task's transaction deliberately stays open while editing.
            _pump_gui_until(lambda: _left_commit(self.doc) and not self.doc.mustExecute(), 30)

        base = self.body.newObject("PartDesign::AdditiveBox", "Base")
        base.Length = base.Width = 20
        base.Height = 4
        base.Placement.Base = App.Vector(-10, -10, -4)
        # Put the nominal sketch after the solid in the Body history.
        self.body.Group = [base, self.sketch]
        self.body.Tip = base
        recompute_document(self.doc)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.sketch)
        Gui.runCommand("PartDesign_Pocket")
        wait_for_preview()
        self.assertTrue(Gui.Control.activeDialog())
        pocket = self.doc.getObject("Pocket")
        self.assertIsNotNone(pocket)
        field = Gui.getMainWindow().findChild(QtGui.QWidget, "fittingToleranceEdit")
        self.assertIsNotNone(field)
        self.assertIn("10.4", field.toolTip())
        update_view = Gui.getMainWindow().findChild(QtGui.QCheckBox, "checkBoxUpdateView")
        self.assertIsNotNone(update_view)
        update_view.setChecked(True)
        editor = field.findChild(QtGui.QLineEdit)
        self.assertIsNotNone(editor)
        editor.setFocus()
        editor.setText("0.4 mm")
        QtGui.QApplication.sendEvent(field, QtGui.QFocusEvent(QtCore.QEvent.FocusOut))
        wait_for_preview()
        Gui.Control.activeTaskDialog().accept()
        recompute_document(self.doc)
        self.assertFalse(Gui.Control.activeDialog())
        self.assertAlmostEqual(pocket.FittingTolerance.Value, 0.4)
        self.assertAlmostEqual(pocket.AddSubShape.BoundBox.XLength, 10.4)
        self.assertAlmostEqual(self.sketch.Shape.BoundBox.XLength, 10)
