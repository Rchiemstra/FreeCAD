# SPDX-License-Identifier: LGPL-2.1-or-later
import FreeCAD as App
import Part
from PySide import QtCore, QtGui, QtWidgets
from pivy import coin
from SketcherTests.GuiTestCase import FreeCADGui as Gui, SketcherGuiTestCase
from SketcherTests.ExpressionDiagnosticsFixture import make_box


class TestSketchExpressionDiagnosticsGui(SketcherGuiTestCase):
    def setUp(self):
        super().setUp()
        self.preferences = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Sketcher")
        self.auto_update = self.preferences.GetBool("AutoRecompute", False)
        self.preferences.SetBool("AutoRecompute", True)
        Gui.activateWorkbench("SketcherWorkbench")
        self.doc = App.newDocument("SketchExpressionDiagnosticsGui")
        self.sketch = make_box(self.doc)
        self.recompute_gui_document()
        self.doc.openTransaction("Delete dimensions")
        self.doc.removeObject("Dimensions")
        self.doc.commitTransaction()
        self.recompute_gui_document()
        Gui.activeDocument().setEdit(self.sketch.Name)
        self.flush_gui(100)

    def tearDown(self):
        try:
            super().tearDown()
        finally:
            self.preferences.SetBool("AutoRecompute", self.auto_update)

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
        params = App.ParamGet("User parameter:BaseApp/Preferences/View")
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

    def test_expression_changes_and_failed_recompute_refresh_without_solve(self):
        # Repair the deleted source, then exercise auto-update while still editing.
        for name, value in (("Width", "20 mm"), ("Height", "10 mm")):
            self.sketch.setExpression("Constraints." + name, value)
        self.recompute_gui_document()
        self.flush_gui(150)
        self.assertFalse(self.message_label().isVisible())
        self.sketch.setExpression("Constraints.Width", "Missing.Value")
        self.recompute_gui_document()
        self.flush_gui(150)
        self.assertIsNotNone(self.constraint_list().item(8).data(QtCore.Qt.ForegroundRole))
        self.assertIn("Missing", self.constraint_list().item(8).toolTip())
        self.assertIn("1 constraint with a broken expression", self.message_label().text())
        self.sketch.setExpression("Constraints.Width", "20 mm")
        self.recompute_gui_document()
        self.flush_gui(150)
        self.assertFalse(self.message_label().isVisible())
        self.assertIsNone(self.constraint_list().item(8).data(QtCore.Qt.ForegroundRole))

    def test_dependency_change_refreshes_without_solver_notification(self):
        variables = self.doc.addObject("App::VarSet", "Dimensions")
        for name, value in (("Width", 20), ("Height", 10)):
            variables.addProperty("App::PropertyLength", name)
            setattr(variables, name, value)
        self.recompute_gui_document()
        self.flush_gui(150)
        self.assertFalse(self.message_label().isVisible())
        variables.setExpression("Width", "Missing.Value")
        self.recompute_gui_document()
        self.flush_gui(150)
        self.assertIn("1 constraint with a broken expression", self.message_label().text())
        self.assertIn("Missing", self.constraint_list().item(8).toolTip())

    def test_visibility_and_solver_notifications_reuse_evaluation(self):
        from unittest.mock import patch
        from freecad import sketcher_expression_diagnostics as diagnostics
        self.flush_gui(150)
        with patch.object(diagnostics, "diagnose", wraps=diagnostics.diagnose) as evaluate:
            for visible in (False, True, False, True):
                self.sketch.setVisibility(8, visible)
                self.flush_gui(100)
            self.sketch.solve()
            self.flush_gui(100)
            self.assertEqual(evaluate.call_count, 0)
            self.sketch.setExpression("Constraints.Width", "20 mm")
            self.recompute_gui_document()
            self.flush_gui(150)
            self.assertGreater(evaluate.call_count, 0)

    def test_paint_uses_snapshot_while_owner_changes_constraints(self):
        import threading
        from Test.GuiRecompute import _pump_gui_until

        items = self.constraint_list()
        before = [[items.item(i).data(role) for role in
                   (QtCore.Qt.DisplayRole, QtCore.Qt.ForegroundRole, QtCore.Qt.ToolTipRole)]
                  for i in (0, 8, 9)]
        entered, release = threading.Event(), threading.Event()
        outcome = []
        def mutate():
            self.sketch.Constraints = []
            entered.set()
            if not release.wait(15):
                raise RuntimeError("paint test timed out")
        def worker():
            result = self.doc.commitCompatibilityMutationAsync(
                mutate, object_name=self.sketch.Name, label="Paint busy regression",
                recompute=False, postcondition=lambda: False)
            outcome.append(result["wait"]() if callable(result.get("wait")) else result)
        thread = threading.Thread(target=worker)
        thread.start()
        try:
            self.assertTrue(self.wait_until(entered.is_set, 5000))
            self.assertFalse(Gui.tryRunPresentationCallback(lambda: None, self.doc))
            image = QtGui.QImage(items.size(), QtGui.QImage.Format_ARGB32)
            for _ in range(20):
                items.render(image)
                actual = [[items.item(i).data(role) for role in
                           (QtCore.Qt.DisplayRole, QtCore.Qt.ForegroundRole, QtCore.Qt.ToolTipRole)]
                          for i in (0, 8, 9)]
                self.assertEqual(actual, before)
        finally:
            release.set()
            _pump_gui_until(lambda: not thread.is_alive(), 20)
            thread.join()
        self.assertTrue(outcome)
        self.assertNotEqual(outcome[0].get("status"), "Committed")
        self.assertEqual(self.sketch.ConstraintCount, 10)


class TestExpressionClipboardWorkflow(SketcherGuiTestCase):
    def test_clipboard_diameter_rejection_and_expression_editor_repair(self):
        from Test.GuiRecompute import close_document
        Gui.activateWorkbench("SketcherWorkbench")
        source = App.newDocument("ExpressionClipboardSource")
        self.doc = None
        params = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Sketcher")
        notifications = App.ParamGet("User parameter:BaseApp/Preferences/NotificationArea")
        saved = params.GetBool("ShowDialogOnDistanceConstraint", True)
        saved_notify = notifications.GetBool("NonIntrusiveNotificationsEnabled", True)
        general = App.ParamGet("User parameter:BaseApp/Preferences/General")
        saved_deps = general.GetBool("ObjectSelectionAutoDeps", True)
        params.SetBool("ShowDialogOnDistanceConstraint", True)
        notifications.SetBool("NonIntrusiveNotificationsEnabled", False)
        timer = QtCore.QTimer()
        messages, copy_dialogs = [], []
        def handle_modal():
            dialog = QtWidgets.QApplication.activeModalWidget()
            if dialog is None:
                return
            tree = dialog.findChild(QtWidgets.QTreeWidget, "treeWidget")
            if tree is not None:
                auto = dialog.findChild(QtWidgets.QCheckBox, "checkBoxAutoDeps")
                auto.setChecked(False)
                for item in tree.findItems("Dimensions", QtCore.Qt.MatchContains | QtCore.Qt.MatchRecursive):
                    item.setCheckState(0, QtCore.Qt.Unchecked)
                copy_dialogs.append(True)
                QtCore.QTimer.singleShot(0, dialog.accept)
            elif isinstance(dialog, QtWidgets.QMessageBox):
                messages.append(dialog.text())
                QtCore.QTimer.singleShot(0, dialog.accept)
            elif isinstance(dialog, QtWidgets.QDialog):
                QtCore.QTimer.singleShot(0, dialog.accept)
        timer.timeout.connect(handle_modal)
        try:
            sketch = make_box(source)
            self.recompute_gui_document(source)
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(sketch)
            Gui.getMainWindow().findChild(QtWidgets.QMdiArea).activeSubWindow().setFocus()
            timer.start(30)
            Gui.runCommand("Std_Copy")
            timer.stop()
            self.assertTrue(copy_dialogs)
            self.assertTrue(any("documentobject" in fmt for fmt in
                                QtWidgets.QApplication.clipboard().mimeData().formats()))
            close_document(source)
            source = None
            self.doc = App.newDocument("ExpressionClipboardTarget")
            Gui.getMainWindow().findChild(QtWidgets.QMdiArea).activeSubWindow().setFocus()
            timer.start(30)
            Gui.runCommand("Std_Paste")
            self.recompute_gui_document()
            self.assertIsNone(self.doc.getObject("Dimensions"))
            self.sketch = self.doc.getObject("Sketch")
            self.assertIsNotNone(self.sketch)
            self.assertEqual(self.sketch.ConstraintCount, 10)
            self.assertTrue(any("Dimensions" in expression for _, expression in self.sketch.ExpressionEngine))
            Gui.activeDocument().setEdit(self.sketch.Name)
            self.flush_gui(200)
            label = Gui.getMainWindow().findChild(QtWidgets.QLabel, "brokenExpressionConstraints")
            self.assertIn("2 constraints with broken expressions", label.text())
            items = next(w for w in Gui.getMainWindow().findChildren(QtWidgets.QListWidget)
                         if w.count() == 10 and w.item(8).data(QtCore.Qt.UserRole) == 8)
            self.assertIn("Dimensions", items.item(8).toolTip())
            self.sketch.addGeometry(Part.Circle(App.Vector(35, 5, 0), App.Vector(0, 0, 1), 3), False)
            before = self.sketch.ConstraintCount
            messages.clear()
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(self.sketch, "Edge5")
            timer.start(30)
            Gui.runCommand("Sketcher_ConstrainDiameter")
            timer.stop()
            self.flush_gui(200)
            self.assertEqual(self.sketch.ConstraintCount, before, messages)
            self.assertIn("broken expressions", "\n".join(messages).lower())
            self.assertIn("Dimensions", "\n".join(messages))
            self.assertIn("Width", "\n".join(messages))
            self.assertTrue(label.isVisible())
            Gui.activeDocument().resetEdit()
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(self.sketch)
            self.flush_gui(200)
            view = Gui.getMainWindow().findChild(QtWidgets.QTreeView, "propertyEditorData")
            for name, value in (("Width", "20 mm"), ("Height", "10 mm")):
                key = self.sketch.FullName + ".Constraints." + name
                def find(parent=QtCore.QModelIndex()):
                    if not parent.isValid():
                        for row in range(view.model().rowCount()):
                            view.model().index(row, 0).data(QtCore.Qt.UserRole + 26496)
                    for row in range(view.model().rowCount(parent)):
                        index = view.model().index(row, 0, parent)
                        if key in (index.data(QtCore.Qt.UserRole + 26497) or []):
                            return index
                        child = find(index)
                        if child.isValid():
                            return child
                    return QtCore.QModelIndex()
                self.assertTrue(self.wait_until(lambda: find().isValid(), 5000), key)
                index = find()
                ancestor = index.parent()
                while ancestor.isValid():
                    view.expand(ancestor)
                    ancestor = ancestor.parent()
                value_index = index.sibling(index.row(), 1)
                view.scrollTo(value_index)
                self.flush_gui()
                self.click(view.viewport(), view.visualRect(value_index).center())
                self.assertTrue(self.wait_until(
                    lambda: view.findChild(QtWidgets.QAbstractSpinBox) is not None, 5000), name)
                spin = view.findChild(QtWidgets.QAbstractSpinBox)
                self.key_click(spin, QtCore.Qt.Key_Equal, "=")
                editor = Gui.getMainWindow().findChild(QtWidgets.QDialog, "DlgExpressionInput")
                self.assertIsNotNone(editor)
                text = editor.findChild(QtWidgets.QPlainTextEdit, "expression")
                if text is None:
                    text = editor.findChild(QtWidgets.QTextEdit, "expression")
                self.assertIsNotNone(text)
                text.setPlainText(value)
                buttons = editor.findChild(QtWidgets.QDialogButtonBox)
                ok = buttons.button(QtWidgets.QDialogButtonBox.Ok)
                self.assertTrue(self.wait_until(ok.isEnabled, 5000))
                ok.click()
                self.flush_gui(100)
                self.key_click(spin, QtCore.Qt.Key_Return)
                self.flush_gui(100)
            self.recompute_gui_document()
            self.assertEqual(self.sketch.ConstraintCount, before)
            for name in ("Width", "Height"):
                key = self.sketch.FullName + ".Constraints." + name
                def healthy():
                    index = find()
                    if not index.isValid():
                        return False
                    value_index = index.sibling(index.row(), 1)
                    color = value_index.data(QtCore.Qt.ForegroundRole)
                    if isinstance(color, QtGui.QBrush):
                        color = color.color()
                    return (color == QtWidgets.QApplication.palette().color(QtGui.QPalette.Link)
                            and "Expression error" not in (value_index.data(QtCore.Qt.ToolTipRole) or ""))
                self.assertTrue(self.wait_until(healthy, 5000), name)
            Gui.activeDocument().setEdit(self.sketch.Name)
            self.flush_gui(200)
            label = Gui.getMainWindow().findChild(QtWidgets.QLabel, "brokenExpressionConstraints")
            self.assertFalse(label.isVisible())
            items = next(w for w in Gui.getMainWindow().findChildren(QtWidgets.QListWidget)
                         if w.count() == before and w.item(8).data(QtCore.Qt.UserRole) == 8)
            for i in (8, 9):
                self.assertIsNone(items.item(i).data(QtCore.Qt.ForegroundRole))
                self.assertNotIn("Broken expression", items.item(i).toolTip())
        finally:
            timer.stop()
            params.SetBool("ShowDialogOnDistanceConstraint", saved)
            notifications.SetBool("NonIntrusiveNotificationsEnabled", saved_notify)
            general.SetBool("ObjectSelectionAutoDeps", saved_deps)
            if source is not None:
                close_document(source)
