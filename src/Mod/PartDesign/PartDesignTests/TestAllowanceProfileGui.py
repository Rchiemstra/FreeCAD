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
        self.pocket_dialog_fitting_tolerance(accept_immediately=False)

    def testPocketDialogAcceptsPendingFittingTolerance(self):
        self.pocket_dialog_fitting_tolerance(accept_immediately=True)

    def testPocketDialogRetriesBusyPreview(self):
        self.pocket_dialog_fitting_tolerance(accept_immediately=False, busy_lane=True)

    def testPocketDialogRetriesBusyAccept(self):
        self.pocket_dialog_fitting_tolerance(accept_immediately=True, busy_lane=True)

    def pocket_dialog_fitting_tolerance(self, accept_immediately, busy_lane=False):
        from PySide import QtCore, QtGui
        from Test.GuiRecompute import _left_commit, _pump_gui_until, _retry_while_lane_busy

        def settle_recompute():
            handle = _retry_while_lane_busy(self.doc.recomputeAsync, 30)
            _pump_gui_until(lambda: handle.done() and _left_commit(self.doc), 30, handle.status)

        def wait_for_preview(feature):
            # Preview recomputes the feature; its Body stays dirty until accept.
            # The task's transaction also deliberately stays open while editing.
            _pump_gui_until(
                lambda: _left_commit(self.doc) and "Touched" not in feature.State, 30,
                lambda: {"readiness": self.doc.getMutationReadiness(),
                         "feature": feature.State, "status": feature.getStatusString()},
            )

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
        self.assertTrue(Gui.Control.activeDialog())
        pocket = self.doc.getObject("Pocket")
        self.assertIsNotNone(pocket)
        wait_for_preview(pocket)
        field = Gui.getMainWindow().findChild(QtGui.QWidget, "fittingToleranceEdit")
        self.assertIsNotNone(field)
        self.assertIn("10.4", field.toolTip())
        update_view = Gui.getMainWindow().findChild(QtGui.QCheckBox, "checkBoxUpdateView")
        self.assertIsNotNone(update_view)
        update_view.setChecked(not busy_lane)
        editor = field.findChild(QtGui.QLineEdit)
        self.assertIsNotNone(editor)
        editor.setFocus()
        # Finish the previous editor's focus-out recompute before the next edit.
        settle_recompute()
        editor.setText("0.4 mm")
        QtGui.QApplication.sendEvent(field, QtGui.QFocusEvent(QtCore.QEvent.FocusOut))
        if busy_lane:
            # Hold the owner lane across both requests so the Busy path is
            # deterministic, independent of machine speed and OCC timing.
            import threading

            entered, release = threading.Event(), threading.Event()

            class BlockingProxy:
                def execute(self, obj):
                    entered.set()
                    if not release.wait(10):
                        raise RuntimeError("test did not release execution lane")

            blocker = self.doc.addObject("App::FeaturePython", "LaneBlocker")
            blocker.Proxy = BlockingProxy()
            handle = _retry_while_lane_busy(lambda: self.doc.recomputeAsync([blocker]), 30)
            try:
                _pump_gui_until(entered.is_set, 5)
                update_view.setChecked(True)
                self.assertIn("Touched", pocket.State)
                if accept_immediately:
                    Gui.Control.activeTaskDialog().accept()
                    self.assertTrue(Gui.Control.activeDialog())
            finally:
                release.set()
                _pump_gui_until(lambda: handle.done() and _left_commit(self.doc), 30,
                                handle.status)
        if not accept_immediately:
            wait_for_preview(pocket)
            settle_recompute()
        if not (busy_lane and accept_immediately):
            Gui.Control.activeTaskDialog().accept()
        # TaskDialogPy.accept() clicks OK and returns None. Completion may be
        # deferred while a focus-out preview owns the execution lane.
        _pump_gui_until(lambda: not Gui.Control.activeDialog(), 30,
                        self.doc.getMutationReadiness)
        self.assertFalse(self.doc.HasPendingTransaction)
        recompute_document(self.doc)
        self.assertFalse(Gui.Control.activeDialog())
        self.assertAlmostEqual(pocket.FittingTolerance.Value, 0.4)
        self.assertTrue(pocket.Shape.isValid())
        # GUI tessellation makes bounding boxes approximate; measure the rims.
        rims = [edge.Curve for edge in pocket.Shape.Edges if isinstance(edge.Curve, Part.Circle)]
        self.assertEqual(len(rims), 2)
        for rim in rims:
            self.assertAlmostEqual(2 * rim.Radius, 10.4)
        self.assertAlmostEqual(self.sketch.Geometry[0].Radius, 5)
