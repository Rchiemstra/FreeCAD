# SPDX-License-Identifier: LGPL-2.1-or-later
"""GUI command and cached expression diagnostics in the property editor."""

import unittest

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets
from TestExpressionDiagnostics import close_document, recompute_document, save_document_as
import os
import tempfile


class ExpressionDiagnosticsGuiTests(unittest.TestCase):
    def test_command_and_selection(self):
        from freecad import expression_diagnostics_gui as gui

        self.assertIn("Std_ExpressionDiagnostics", Gui.listCommands())
        doc = App.newDocument("ExpressionDiagnosticsGuiTest")
        try:
            obj = doc.addObject("App::FeaturePython", "Broken")
            obj.addProperty("App::PropertyFloat", "Value")
            obj.setExpression("Value", "Absent.Value")
            with tempfile.TemporaryDirectory() as tmp:
                save_document_as(doc, os.path.join(tmp, "diagnostics.FCStd"))
                before = (doc.UndoCount, doc.RedoCount, doc.isTouched(),
                          list(obj.State), Gui.getDocument(doc.Name).Modified)
                Gui.runCommand("Std_ExpressionDiagnostics")
                dialog = Gui.getMainWindow().findChild(QtWidgets.QDialog, "ExpressionDiagnostics")
                self.assertIsNotNone(dialog)
                self.assertEqual(dialog.table.rowCount(), 1)
                dialog.table.selectRow(0)
                self.assertEqual(Gui.Selection.getSelection(), [obj])
                self.assertEqual(before, (doc.UndoCount, doc.RedoCount, doc.isTouched(),
                                          list(obj.State), Gui.getDocument(doc.Name).Modified))
                dialog.close()
                Gui.Selection.clearSelection()
        finally:
            close_document(doc)

    def test_report_defers_all_model_reads_and_selection_while_busy(self):
        from unittest.mock import patch
        from freecad import expression_diagnostics as diagnostics
        from freecad.expression_diagnostics_gui import DiagnosticsDialog

        doc = App.newDocument("DiagnosticsBusy")
        dialog = None
        try:
            obj = doc.addObject("App::FeaturePython", "Values")
            obj.addProperty("App::PropertyFloat", "Value")
            obj.setExpression("Value", "Missing.Value")
            with patch.object(Gui, "tryRunPresentationCallback", return_value=False), \
                    patch.object(diagnostics, "diagnose", wraps=diagnostics.diagnose) as reads, \
                    patch.object(App, "listDocuments", wraps=App.listDocuments) as documents:
                dialog = DiagnosticsDialog(doc)
                self.assertEqual(reads.call_count, 0)
                self.assertEqual(documents.call_count, 0)
                self.assertTrue(dialog.timer.isActive())
            dialog.refresh()
            self.assertEqual(dialog.table.rowCount(), 1)
            Gui.Selection.clearSelection()
            tabs = Gui.getMainWindow().findChild(QtWidgets.QTabWidget, "propertyTab")
            tabs.setCurrentIndex(0)
            with patch.object(Gui, "tryRunPresentationCallback", return_value=False), \
                    patch.object(App, "listDocuments", wraps=App.listDocuments) as documents:
                dialog.table.selectRow(0)
                self.assertEqual(documents.call_count, 0)
                self.assertEqual(Gui.Selection.getSelection(), [])
            dialog.refresh()
            self.assertEqual(Gui.Selection.getSelection(), [obj])
            view = Gui.getMainWindow().findChild(QtWidgets.QTreeView, "propertyEditorData")
            import time
            deadline = time.monotonic() + 5
            while view.currentIndex().data() != "Value" and time.monotonic() < deadline:
                QtWidgets.QApplication.processEvents()
                QtCore.QThread.msleep(20)
            self.assertEqual(view.currentIndex().data(), "Value")
            self.assertEqual(tabs.currentWidget(), view)
        finally:
            if dialog is not None:
                dialog.close()
            Gui.Selection.clearSelection()
            close_document(doc)

    def test_report_selects_nested_property(self):
        from freecad.expression_diagnostics_gui import DiagnosticsDialog
        import time

        doc = App.newDocument("DiagnosticsNested")
        dialog = None
        try:
            obj = doc.addObject("App::FeaturePython", "Values")
            obj.addProperty("App::PropertyPlacement", "Placement")
            obj.setExpression("Placement.Base.x", "Missing.Value")
            dialog = DiagnosticsDialog(doc)
            dialog.table.selectRow(0)
            view = Gui.getMainWindow().findChild(QtWidgets.QTreeView, "propertyEditorData")
            key = obj.FullName + ".Placement.Base.x"
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                QtWidgets.QApplication.processEvents()
                if key in (view.currentIndex().data(QtCore.Qt.UserRole + 26497) or []):
                    break
                QtCore.QThread.msleep(20)
            self.assertIn(key, view.currentIndex().data(QtCore.Qt.UserRole + 26497) or [])
            self.assertEqual(view.currentIndex().data(), "x")
        finally:
            if dialog is not None:
                dialog.close()
            Gui.Selection.clearSelection()
            close_document(doc)

    def test_report_reserves_external_documents_refreshes_and_closes(self):
        from unittest.mock import patch
        from freecad.expression_diagnostics_gui import DiagnosticsDialog

        doc = App.newDocument("DiagnosticsConsumer")
        source = App.newDocument("DiagnosticsSource")
        dialog = None
        try:
            value = source.addObject("App::FeaturePython", "Input")
            value.addProperty("App::PropertyFloat", "Value")
            value.setExpression("Value", "Missing.Value")
            obj = doc.addObject("App::FeaturePython", "Values")
            obj.addProperty("App::PropertyFloat", "Value")
            temporary = tempfile.TemporaryDirectory()
            self.addCleanup(temporary.cleanup)
            save_document_as(source, os.path.join(temporary.name, "source.FCStd"))
            save_document_as(doc, os.path.join(temporary.name, "consumer.FCStd"))
            obj.setExpression("Value", source.Name + "#Input.Value")
            original = Gui.tryRunPresentationCallback
            scopes = []
            def admitted(callback, document=None):
                if document is not None:
                    scopes.append(document.Name)
                return original(callback, document) if document is not None else original(callback)
            with patch.object(Gui, "tryRunPresentationCallback", admitted):
                dialog = DiagnosticsDialog(doc)
            self.assertIn(doc.Name, scopes)
            self.assertIn(source.Name, scopes)
            from freecad import expression_diagnostics as diagnostics
            def source_busy(callback, document=None):
                if document is source:
                    return False
                return admitted(callback, document)
            with patch.object(Gui, "tryRunPresentationCallback", source_busy), \
                    patch.object(diagnostics, "diagnose", wraps=diagnostics.diagnose) as reads:
                dialog.refresh()
                self.assertEqual(reads.call_count, 0)
                self.assertFalse(dialog.table.isEnabled())
                self.assertTrue(dialog.timer.isActive())
            dialog.refresh()
            self.assertGreater(dialog.table.rowCount(), 0)
            value.setExpression("Value", "0")
            # Refresh from queued observer notifications, without reopening.
            import time
            deadline = time.monotonic() + 5
            while dialog.table.rowCount() and time.monotonic() < deadline:
                QtWidgets.QApplication.processEvents()
                QtCore.QThread.msleep(20)
            self.assertEqual(dialog.table.rowCount(), 0)
            dialog.setAttribute(QtCore.Qt.WA_DeleteOnClose, False)
            dialog.show()
            close_document(doc)
            doc = None
            QtWidgets.QApplication.processEvents()
            dialog.refresh()
            self.assertTrue(dialog.stopped)
            self.assertFalse(dialog.isVisible())
        finally:
            if dialog is not None and not getattr(dialog, "stopped", False):
                dialog.close()
            if doc is not None:
                close_document(doc)
            close_document(source)

    def test_report_uses_real_french_catalogue(self):
        from freecad.expression_diagnostics_gui import DiagnosticsDialog
        from freecad.expression_diagnostics_text import role_text, kind_text, reason_text

        catalogue = QtCore.QTranslator()
        # Built with FreeCAD and embedded in its normal resource collection.
        self.assertTrue(catalogue.load(":/translations/FreeCAD_fr.qm"))
        app = QtWidgets.QApplication.instance()
        app.installTranslator(catalogue)
        doc = App.newDocument("DiagnosticsFrench")
        dialog = None
        try:
            obj = doc.addObject("App::FeaturePython", "Values")
            obj.addProperty("App::PropertyFloat", "Value")
            obj.setExpression("Value", "Missing.Value")
            dialog = DiagnosticsDialog(doc)
            self.assertEqual(dialog.table.horizontalHeaderItem(0).text(), "Objet")
            self.assertEqual(dialog.table.horizontalHeaderItem(5).text(), "Cause")
            self.assertEqual(role_text("root"), "Cause première")
            self.assertEqual(kind_text("unresolved_reference"), "Référence introuvable")
            self.assertIn("Détails", dialog.table.item(0, 5).text())
            for kind in ("stale_value", "not_evaluated", "dependency_cycle"):
                row = dict(role="root", kind=kind, message="unused", chain=[
                    dict(object="Doc#Object", property="Value")])
                self.assertNotEqual(reason_text(row), "unused")
                self.assertNotEqual(kind_text(kind), kind)
            self.assertEqual(reason_text(dict(role="affected", kind="dependency", chain=[
                dict(object="Doc#Object", property="Value")])), "Dépend de Doc#Object.Value.")
        finally:
            if dialog is not None:
                dialog.close()
            app.removeTranslator(catalogue)
            close_document(doc)

    def test_menu_in_multiple_workbenches(self):
        original = Gui.activeWorkbench().name()
        try:
            for workbench in ("PartWorkbench", "SketcherWorkbench"):
                Gui.activateWorkbench(workbench)
                actions = Gui.getMainWindow().findChildren(QtGui.QAction)
                self.assertTrue(any(a.objectName() == "Std_ExpressionDiagnostics" for a in actions))
        finally:
            Gui.activateWorkbench(original)


class ExpressionDiagnosticsDataTabTests(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("ExpressionDiagnosticsDataTab")
        self.obj = self.doc.addObject("App::FeaturePython", "Values")
        for name in ("Good", "Broken", "Plain"):
            self.obj.addProperty("App::PropertyFloat", name)
        self.obj.Good = 2
        self.obj.setExpression("Good", "2")
        self.obj.setExpression("Broken", "Absent.Value")
        self.view = Gui.getMainWindow().findChild(QtWidgets.QTreeView, "propertyEditorData")
        self.assertIsNotNone(self.view)
        self.params = App.ParamGet("User parameter:BaseApp/Preferences/PropertyView")
        self.old_color = self.params.GetUnsigned("ExpressionErrorColor", 0xD35400FF)
        self.params.SetUnsigned("ExpressionErrorColor", 0xD35400FF)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.obj)
        self.wait_for(lambda: self.index("Broken").isValid())

    def tearDown(self):
        self.params.SetUnsigned("ExpressionErrorColor", self.old_color)
        Gui.Selection.clearSelection()
        if self.doc is not None:
            close_document(self.doc)

    def wait_for(self, predicate):
        import time
        deadline = time.monotonic() + 10
        while time.monotonic() < deadline:
            QtWidgets.QApplication.processEvents()
            if predicate():
                return
            QtCore.QThread.msleep(20)
        self.fail("Data tab did not update")

    def index(self, name, parent=QtCore.QModelIndex()):
        model = self.view.model()
        for row in range(model.rowCount(parent)):
            index = model.index(row, 0, parent)
            if index.data() == name:
                return index.sibling(row, 1)
            found = self.index(name, index)
            if found.isValid():
                return found
        return QtCore.QModelIndex()

    def color(self, name):
        color = self.index(name).data(QtCore.Qt.ForegroundRole)
        return color.color() if isinstance(color, QtGui.QBrush) else color

    def assert_broken(self, name, reason):
        self.wait_for(lambda: self.color(name) == QtGui.QColor(0xD3, 0x54, 0)
                      and reason in (self.index(name).data(QtCore.Qt.ToolTipRole) or ""))
        self.assertIn(reason, self.index(name).data(QtCore.Qt.ToolTipRole))

    def assert_good(self, name):
        self.wait_for(lambda: self.color(name) == QtWidgets.QApplication.palette().color(
            QtGui.QPalette.Link))
        self.assertNotIn("Expression error", self.index(name).data(QtCore.Qt.ToolTipRole) or "")

    def test_roles_and_repair(self):
        recompute_document(self.doc)
        self.assert_broken("Broken", "Absent")
        self.assert_good("Good")
        self.assertIsNone(self.color("Plain"))
        self.obj.setExpression("Broken", "3")
        recompute_document(self.doc)
        self.assert_good("Broken")
        self.obj.setExpression("Broken", "1 / 0")
        recompute_document(self.doc)
        self.assert_broken("Broken", "zero")
        self.obj.setExpression("Broken", None)
        self.wait_for(lambda: self.color("Broken") is None)
        self.assertNotIn("Expression error", self.index("Broken").data(QtCore.Qt.ToolTipRole) or "")

    def test_reference_creation_rename_delete_and_property_removal(self):
        self.assert_broken("Broken", "Absent")
        source = self.doc.addObject("App::FeaturePython", "Absent")
        source.addProperty("App::PropertyFloat", "Value")
        source.Value = 7
        recompute_document(self.doc)
        self.assert_good("Broken")
        source.Label = "Input values"
        self.obj.setExpression("Broken", "<<Input values>>.Value")
        source.Label = "Renamed values"
        recompute_document(self.doc)
        self.assert_good("Broken")
        source.removeProperty("Value")
        recompute_document(self.doc)
        self.assert_broken("Broken", "Value")
        source.addProperty("App::PropertyFloat", "Value")
        recompute_document(self.doc)
        self.assert_good("Broken")
        self.doc.removeObject(source.Name)
        recompute_document(self.doc)
        self.assert_broken("Broken", "Value")

    def test_error_color_parameter(self):
        self.assert_broken("Broken", "Absent")
        self.params.SetUnsigned("ExpressionErrorColor", 0xC02030FF)
        self.wait_for(lambda: self.color("Broken") == QtGui.QColor(0xC0, 0x20, 0x30))
        self.assert_good("Good")

    def test_role_reads_use_cached_diagnostics(self):
        from unittest.mock import patch
        from freecad import expression_diagnostics

        self.assert_broken("Broken", "Absent")
        # A large tab can request these roles thousands of times per repaint.
        with patch.object(expression_diagnostics, "diagnose",
                          wraps=expression_diagnostics.diagnose) as diagnose:
            for _ in range(1000):
                for name in ("Good", "Broken", "Plain"):
                    index = self.index(name)
                    index.data(QtCore.Qt.ForegroundRole)
                    index.data(QtCore.Qt.ToolTipRole)
            self.assertEqual(diagnose.call_count, 0)
            self.obj.setExpression("Broken", "Absent.Other")
            self.wait_for(lambda: "Absent.Other" in (
                self.index("Broken").data(QtCore.Qt.ToolTipRole) or ""))
            self.assertGreater(diagnose.call_count, 0)

    def test_compound_expression_roles(self):
        self.obj.addProperty("App::PropertyVector", "Vector")
        self.obj.setExpression("Vector.x", "Absent.Value")
        self.obj.setExpression("Vector.y", "2")
        recompute_document(self.doc)
        self.wait_for(lambda: self.index("Vector").isValid())
        self.assert_broken("Vector", "Vector.x")
        vector = self.index("Vector")
        parent = vector.sibling(vector.row(), 0)
        self.wait_for(lambda: self.view.model().rowCount(parent) == 3)
        x = self.view.model().index(0, 1, parent)
        y = self.view.model().index(1, 1, parent)
        self.assertEqual(x.data(QtCore.Qt.ForegroundRole), QtGui.QColor(0xD3, 0x54, 0))
        self.assertIn("Absent", x.data(QtCore.Qt.ToolTipRole))
        self.assertEqual(y.data(QtCore.Qt.ForegroundRole),
                         QtWidgets.QApplication.palette().color(QtGui.QPalette.Link))


    def test_named_sketch_constraint_roles(self):
        import Part
        import Sketcher

        sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        for index, length in enumerate((10, 20)):
            sketch.addGeometry(Part.LineSegment(
                App.Vector(0, index * 5, 0), App.Vector(length, index * 5, 0)))
            constraint = sketch.addConstraint(Sketcher.Constraint("Distance", index, float(length)))
            sketch.renameConstraint(constraint, ("Width", "Height")[index])
        sketch.setExpression("Constraints.Width", "Absent.Value")
        sketch.setExpression("Constraints.Height", "20 mm")
        recompute_document(self.doc)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(sketch)
        self.wait_for(lambda: self.index("Width").isValid())
        self.assert_broken("Width", "Constraints.Width")
        self.assert_broken("Constraints", "Constraints.Width")
        self.assert_good("Height")


    def test_multi_selection_roles(self):
        other = self.doc.addObject("App::FeaturePython", "OtherValues")
        for name in ("Good", "Broken", "Plain"):
            other.addProperty("App::PropertyFloat", name)
        other.Broken = 3
        other.setExpression("Broken", "3")
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(other)
        self.assert_good("Broken")
        Gui.Selection.addSelection(self.obj)
        self.assert_broken("Broken", "Absent")


    def test_valid_dependent_expression_keeps_existing_color(self):
        self.obj.setExpression("Good", "Broken + 2")
        self.obj.setExpression("Broken", "Missing.Value")
        self.assert_broken("Broken", "Missing")
        self.assert_good("Good")


    def test_boolean_error_is_painted(self):
        self.obj.addProperty("App::PropertyBool", "Flag")
        self.obj.setExpression("Flag", "Absent.Value")
        self.wait_for(lambda: self.index("Flag").isValid())
        self.assert_broken("Flag", "Absent")
        image = QtGui.QImage(180, 32, QtGui.QImage.Format_ARGB32)
        image.fill(QtCore.Qt.transparent)
        option = QtWidgets.QStyleOptionViewItem()
        option.initFrom(self.view)
        option.rect = QtCore.QRect(0, 0, 180, 32)
        painter = QtGui.QPainter(image)
        try:
            self.view.itemDelegate().paint(painter, option, self.index("Flag"))
        finally:
            painter.end()
        self.assertTrue(any(
            color.alpha() and color.red() > 150 and color.green() < 140 and color.blue() < 80
            for y in range(image.height()) for x in range(image.width())
            for color in (image.pixelColor(x, y),)),
            "The checkbox label did not use the expression error colour")

    def test_cache_refresh_preserves_uncommitted_text(self):
        from unittest.mock import patch
        from freecad import expression_diagnostics

        self.obj.addProperty("App::PropertyString", "Text")
        self.obj.Text = "stored"
        other = self.doc.addObject("App::FeaturePython", "Other")
        other.addProperty("App::PropertyFloat", "Value")
        other.setExpression("Value", "Absent.Value")
        self.wait_for(lambda: self.index("Text").isValid())
        self.view.edit(self.index("Text"))
        editor = self.view.findChild(QtWidgets.QLineEdit)
        self.assertIsNotNone(editor)
        editor.blockSignals(True)
        editor.setText("draft")
        editor.setModified(True)
        editor.blockSignals(False)
        self.assertEqual(self.obj.Text, "stored")
        try:
            with patch.object(expression_diagnostics, "diagnose",
                              wraps=expression_diagnostics.diagnose) as diagnose:
                other.setExpression("Value", "AnotherMissing.Value")
                self.wait_for(lambda: diagnose.call_count > 0)
                self.assertEqual(editor.text(), "draft")
        finally:
            QtCore.QCoreApplication.sendEvent(editor, QtGui.QKeyEvent(
                QtCore.QEvent.KeyPress, QtCore.Qt.Key_Escape, QtCore.Qt.NoModifier))


    def test_document_discovery_requires_presentation_admission(self):
        from unittest.mock import patch
        from freecad.expression_diagnostics_property_editor import _cache

        self.assert_broken("Broken", "Absent")
        _cache.timer.stop()
        _cache.dirty = True
        with patch.object(Gui, "tryRunPresentationCallback", return_value=False), \
                patch.object(App, "listDocuments", wraps=App.listDocuments) as documents:
            _cache.refresh()
            self.assertEqual(documents.call_count, 0,
                             "Document discovery ran without a presentation reservation")
        self.assertTrue(_cache.timer.isActive())
        _cache.refresh()
        self.assert_broken("Broken", "Absent")

    def placement_leaf(self):
        placement = self.index("Placement")
        position = self.index("Position", placement.sibling(placement.row(), 0))
        return self.index("x", position.sibling(position.row(), 0))

    def test_readonly_placement_leaf_identity(self):
        Gui.Selection.clearSelection()
        self.wait_for(lambda: not self.index("Broken").isValid())
        self.obj.addProperty("App::PropertyPlacement", "Placement", "Test", "", 1)
        self.obj.setEditorMode("Placement", 1)
        self.obj.setExpression("Placement.Base.x", "Absent.Value")
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.obj)
        self.assert_broken("Placement", "Placement.Base.x")
        self.wait_for(lambda: self.placement_leaf().data(QtCore.Qt.ForegroundRole)
                      == QtGui.QColor(0xD3, 0x54, 0))
        self.assertIn("Absent", self.placement_leaf().data(QtCore.Qt.ToolTipRole))
        self.assertFalse(self.placement_leaf().flags() & QtCore.Qt.ItemIsEditable)

    def test_merged_placement_leaf_all_owners(self):
        other = self.doc.addObject("App::FeaturePython", "OtherPlacement")
        for obj in (other, self.obj):
            obj.addProperty("App::PropertyPlacement", "Placement")
        self.obj.setExpression("Placement.Base.x", "Absent.Value")
        other.setExpression("Placement.Base.x", "AnotherMissing.Value")
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(other)
        Gui.Selection.addSelection(self.obj)
        self.assert_broken("Placement", "Absent")
        self.wait_for(lambda: self.placement_leaf().data(QtCore.Qt.ForegroundRole)
                      == QtGui.QColor(0xD3, 0x54, 0))
        tooltip = self.placement_leaf().data(QtCore.Qt.ToolTipRole)
        self.assertIn("Absent", tooltip)
        self.assertIn("AnotherMissing", tooltip)
        y = self.placement_leaf().sibling(1, 1)
        self.assertNotIn("Expression error", y.data(QtCore.Qt.ToolTipRole) or "")

    def test_list_and_link_diagnostics(self):
        self.obj.addProperty("App::PropertyFloatList", "Samples")
        self.obj.Samples = [1, 2]
        self.obj.setExpression("Samples[1]", "Absent.Value")
        self.obj.addProperty("App::PropertyLink", "Target")
        self.obj.setExpression("Target", "Absent.Value")
        self.assert_broken("Samples", "Samples[1]")
        self.assert_broken("Target", "Absent")

    def test_xlink_tooltip_preserves_path_and_appends_reason(self):
        external = App.newDocument("DiagnosticsExternal")
        try:
            target = external.addObject("App::FeaturePython", "Target")
            with tempfile.TemporaryDirectory() as tmp:
                path = os.path.join(tmp, "external.FCStd")
                save_document_as(external, path)
                save_document_as(self.doc, os.path.join(tmp, "owner.FCStd"))
                self.obj.addProperty("App::PropertyXLink", "External")
                self.obj.External = target
                self.obj.setExpression("External", "Absent.Value")
                Gui.Selection.clearSelection()
                Gui.Selection.addSelection(self.obj)
                self.assert_broken("External", "Absent")
                self.assertIn("external.FCStd", self.index("External").data(QtCore.Qt.ToolTipRole))
        finally:
            close_document(external)

    def test_failed_recompute_skips_execute_and_refreshes(self):
        class ExecutionProbe:
            def __init__(self):
                self.calls = 0

            def execute(self, obj):
                self.calls += 1

        probe = ExecutionProbe()
        self.obj.Proxy = probe
        recompute_document(self.doc)
        self.assertEqual(probe.calls, 0)
        self.assert_broken("Broken", "Absent")
        self.obj.setExpression("Broken", "3")
        recompute_document(self.doc)
        self.assertGreater(probe.calls, 0)
        self.assert_good("Broken")

    def test_undo_redo_and_close_clear_diagnostics(self):
        from freecad.expression_diagnostics_property_editor import _cache
        from Test.GuiRecompute import undo_document, redo_document

        self.assert_broken("Broken", "Absent")
        self.doc.UndoMode = 1
        self.doc.openTransaction("Repair expression")
        self.obj.setExpression("Broken", "3")
        self.doc.commitTransaction()
        self.assert_good("Broken")
        undo_document(self.doc)
        self.assert_broken("Broken", "Absent")
        redo_document(self.doc)
        self.assert_good("Broken")
        undo_document(self.doc)
        self.assert_broken("Broken", "Absent")
        name = self.doc.Name
        key = self.obj.FullName + ".Broken"
        Gui.Selection.clearSelection()
        close_document(self.doc)
        self.doc = None
        self.wait_for(lambda: name not in _cache.reports and key not in (_cache.published or {}))
        self.assertFalse(self.index("Broken").isValid())
