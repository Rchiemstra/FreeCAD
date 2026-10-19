# SPDX-License-Identifier: LGPL-2.1-or-later
import FreeCAD as App
import Part
from PySide import QtCore, QtWidgets
from pivy import coin
from SketcherTests.GuiTestCase import FreeCADGui as Gui, SketcherGuiTestCase
from SketcherTests.ExpressionDiagnosticsFixture import make_box


class TestSketchExpressionDiagnosticsGui(SketcherGuiTestCase):
    def setUp(self):
        super().setUp()
        Gui.activateWorkbench("SketcherWorkbench")
        self.doc = App.newDocument("SketchExpressionDiagnosticsGui")
        self.sketch = make_box(self.doc)
        self.recompute_gui_document()
        self.doc.openTransaction("Delete dimensions")
        self.doc.removeObject("Dimensions")
        self.doc.commitTransaction()
        self.recompute_gui_document()
        Gui.activeDocument().setEdit(self.sketch.Name)
        self.sketch.solve()
        self.flush_gui(100)

    def constraint_list(self):
        for widget in Gui.getMainWindow().findChildren(QtWidgets.QListWidget):
            if widget.count() == self.sketch.ConstraintCount and widget.item(8).data(QtCore.Qt.UserRole) == 8:
                return widget
        self.fail("Sketch constraint list was not found")

    def message_label(self):
        return Gui.getMainWindow().findChild(QtWidgets.QLabel, "brokenExpressionConstraints")

    def test_list_marks_broken_constraints_with_reason(self):
        items = self.constraint_list()
        for index in (8, 9):
            self.assertIsNotNone(items.item(index).data(QtCore.Qt.ForegroundRole))
            self.assertIn("Broken expression", items.item(index).toolTip())
            self.assertIn("Dimensions", items.item(index).toolTip())
        self.assertIsNone(items.item(0).data(QtCore.Qt.ForegroundRole))

    def test_solver_message_counts_and_selects_broken_constraints(self):
        label = self.message_label()
        self.assertIsNotNone(label)
        self.assertIn("2 constraints with broken expressions", label.text())
        label.linkActivated.emit("#broken-expressions")
        self.flush_gui()
        self.assertEqual(set(Gui.Selection.getSelectionEx()[0].SubElementNames),
                         {"Constraint9", "Constraint10"})

    def test_scene_uses_configured_broken_expression_colour(self):
        params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Sketcher/View")
        old = params.GetUnsigned("BrokenExpressionConstrDimColor", 0xD020D0FF)
        params.SetUnsigned("BrokenExpressionConstrDimColor", 0xA15020FF)
        try:
            self.sketch.solve()
            self.flush_gui()
            search = coin.SoSearchAction()
            search.setType(coin.SoType.fromName("SoDatumLabel"))
            search.setInterest(coin.SoSearchAction.ALL)
            search.setSearchingAll(True)
            search.apply(Gui.activeDocument().activeView().getViewer().getSoRenderManager().getSceneGraph())
            paths = search.getPaths()
            colors = [tuple(paths[i].getTail().getField("textColor").getValue().getValue())
                      for i in range(paths.getLength())]
            expected = (0xA1 / 255, 0x50 / 255, 0x20 / 255)
            self.assertGreaterEqual(sum(all(abs(a-b) < 0.005 for a, b in zip(c, expected))
                                        for c in colors), 2)
        finally:
            params.SetUnsigned("BrokenExpressionConstrDimColor", old)

    def test_solver_conflict_message_remains_visible(self):
        import Sketcher
        self.sketch.addConstraint(Sketcher.Constraint("Distance", 0, 21))
        self.sketch.solve()
        self.flush_gui()
        self.assertIn("2 constraints with broken expressions", self.message_label().text())
        status = Gui.getMainWindow().findChild(QtWidgets.QLabel, "labelStatus")
        self.assertIn("Over-constrained", status.text())

    def test_repair_clears_list_and_solver_message(self):
        for name, value in (("Width", "20 mm"), ("Height", "10 mm")):
            self.sketch.setExpression("Constraints." + name, value)
        self.sketch.solve()
        self.flush_gui()
        self.assertIsNone(self.constraint_list().item(8).data(QtCore.Qt.ForegroundRole))
        self.assertFalse(self.message_label().isVisible())

    def test_undo_deletion_clears_diagnostics(self):
        self.undo_gui_document()
        self.sketch.solve()
        self.flush_gui()
        self.assertIsNone(self.constraint_list().item(8).data(QtCore.Qt.ForegroundRole))
        self.assertFalse(self.message_label().isVisible())

    def test_rejected_radius_names_expression_cause(self):
        params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Sketcher")
        notifications = App.ParamGet("User parameter:BaseApp/Preferences/NotificationArea")
        saved = params.GetBool("ShowDialogOnDistanceConstraint", True)
        saved_notifications = notifications.GetBool("NonIntrusiveNotificationsEnabled", True)
        params.SetBool("ShowDialogOnDistanceConstraint", True)
        notifications.SetBool("NonIntrusiveNotificationsEnabled", False)
        self.sketch.addGeometry(Part.Circle(App.Vector(35, 5, 0), App.Vector(0, 0, 1), 3), False)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.sketch, "Edge5")
        messages = []
        timer = QtCore.QTimer()
        def accept_dialog():
            widget = QtWidgets.QApplication.activeModalWidget()
            if isinstance(widget, QtWidgets.QMessageBox):
                messages.append(widget.text())
                QtCore.QTimer.singleShot(0, widget.accept)
            elif isinstance(widget, QtWidgets.QDialog):
                QtCore.QTimer.singleShot(0, widget.accept)
        timer.timeout.connect(accept_dialog)
        timer.start(30)
        try:
            Gui.runCommand("Sketcher_ConstrainRadius")
            self.flush_gui(100)
        finally:
            timer.stop()
            params.SetBool("ShowDialogOnDistanceConstraint", saved)
            notifications.SetBool("NonIntrusiveNotificationsEnabled", saved_notifications)
        self.assertTrue(messages, "Rejected radius must explain why it failed")
        text = "\n".join(messages)
        self.assertIn("broken expressions", text.lower(),
                      f"Radius reproduction: {self.sketch.ConstraintCount} constraints; {messages!r}")
        self.assertIn("Width", text)
        self.assertIn("Dimensions", text)
