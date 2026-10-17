# SPDX-License-Identifier: LGPL-2.1-or-later
import threading
import time
from unittest.mock import patch

import FreeCAD as App
import FreeCADGui as Gui
import Part
from PySide import QtCore, QtGui

from SketcherTests.GuiTestCase import SketcherGuiTestCase


class TestSketchReferenceRepairGui(SketcherGuiTestCase):
    def test_command_preview_cancel(self):
        Gui.activateWorkbench("SketcherWorkbench")
        import SketchReferenceRepairGui as repair_gui

        self.flush_gui()
        self.assertTrue(
            any(
                action.objectName() == "Sketcher_RepairReferences"
                for menu in Gui.getMainWindow().menuBar().findChildren(QtGui.QMenu)
                for action in menu.actions()
            )
        )
        self.doc = App.newDocument("ReferenceRepairGui")
        self.doc.UndoMode = 1
        source = self.doc.addObject("PartDesign::Feature", "Source")
        sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        wanted = Part.makeLine(App.Vector(0, 0, 0), App.Vector(10, 0, 0))
        other = Part.makeLine(App.Vector(30, 0, 0), App.Vector(40, 0, 0))
        source.Shape = Part.makeCompound([other, other.copy(), wanted])
        self.recompute_gui_document()
        sketch.addExternal(source.Name, "Edge3")
        self.recompute_gui_document()
        source.Shape = Part.makeCompound([other])
        self.recompute_gui_document()
        source.Shape = Part.makeCompound([wanted.copy(noElementMap=True), other])
        self.recompute_gui_document()
        source.Visibility = False
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(sketch)
        self.flush_gui()
        undo_count = self.doc.UndoCount
        scene = Gui.activeDocument().activeView().getSceneGraph()
        children = scene.getNumChildren()
        self.assertTrue(repair_gui.command.IsActive())
        Gui.runCommand("Sketcher_RepairReferences")
        self.flush_gui()
        dialog = repair_gui.command.dialog
        try:
            self.assertTrue(dialog.isVisible())
            self.assertEqual(dialog.references.count(), 1)
            self.assertTrue(self.wait_until(lambda: dialog.options.count() > 0, 5000))
            dialog.options.setCurrentRow(0)
            self.assertIsNotNone(dialog.preview.node)
            self.assertEqual(scene.getNumChildren(), children + 1)
            self.assertFalse(source.Visibility)
        finally:
            dialog.close()
            self.flush_gui()
        self.assertEqual(scene.getNumChildren(), children)
        self.assertEqual(self.doc.UndoCount, undo_count)
        self.assertEqual(Gui.Selection.getSelection(), [sketch])

        # Reopen through the registered command and exercise its apply path.
        Gui.runCommand("Sketcher_RepairReferences")
        dialog = repair_gui.command.dialog
        try:
            self.assertTrue(self.wait_until(lambda: dialog.options.count() > 0, 5000))
            dialog.options.setCurrentRow(0)
            self.assertTrue(dialog.apply_button.isEnabled())
            dialog.apply()
            self.assertTrue(
                self.wait_until(lambda: dialog.references.count() == 0, 10000),
                dialog.message.text(),
            )
            self.assertIsNone(dialog.preview.node)
            self.assertEqual(self.doc.UndoCount, undo_count + 1)
        finally:
            dialog.close()
            self.flush_gui()
        self.undo_gui_document()
        self.recompute_gui_document()
        from SketchReferenceRepair import broken_references

        self.assertEqual(len(broken_references(sketch)), 1)

    def make_dialog(self):
        import SketchReferenceRepairGui as module

        self.doc = App.newDocument("RepairLifecycle")
        source = self.doc.addObject("PartDesign::Feature", "Source")
        sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        wanted = Part.makeLine(App.Vector(), App.Vector(10, 0, 0))
        other = Part.makeLine(App.Vector(30, 0, 0), App.Vector(40, 0, 0))
        source.Shape = Part.makeCompound([other, other.copy(), wanted])
        self.recompute_gui_document()
        sketch.addExternal(source.Name, "Edge3")
        self.recompute_gui_document()
        source.Shape = Part.makeCompound([other])
        self.recompute_gui_document()
        source.Shape = Part.makeCompound([wanted.copy(noElementMap=True), other])
        self.recompute_gui_document()
        dialog = module.RepairDialog(sketch)
        dialog.show()
        self.assertTrue(self.wait_until(lambda: dialog.options.count() > 0, 5000))
        dialog.options.setCurrentRow(0)
        return module, sketch, dialog

    def test_sketch_deletion_closes_preview(self):
        _module, sketch, dialog = self.make_dialog()
        self.assertIsNotNone(dialog.preview.node)
        try:
            self.doc.removeObject(sketch.Name)
            self.flush_gui()
            self.assertIsNone(dialog.preview.scene)
            self.assertIsNone(dialog.preview.node)
        finally:
            if dialog.preview.scene is not None:
                dialog.close()

    def test_document_close_closes_preview(self):
        _module, _sketch, dialog = self.make_dialog()
        from Test.GuiRecompute import close_document

        try:
            close_document(self.doc)
            self.doc = None
            self.flush_gui()
            self.assertIsNone(dialog.preview.scene)
            self.assertIsNone(dialog.preview.node)
        finally:
            if dialog.preview.scene is not None:
                dialog.close()

    def test_preview_uses_sketch_document_and_command_rejects_foreign_selection(self):
        module, sketch, dialog = self.make_dialog()
        dialog.close()
        self.flush_gui()
        scene = Gui.getDocument(self.doc.Name).activeView().getSceneGraph()
        other_doc = App.newDocument("OtherRepairDocument")
        try:
            from Test.GuiRecompute import refresh_document

            refresh_document(other_doc)
            foreign_scene = Gui.getDocument(other_doc.Name).activeView().getSceneGraph()
            original_count = foreign_scene.getNumChildren()
            dialog = module.RepairDialog(sketch)
            dialog.show()
            self.assertTrue(self.wait_until(lambda: dialog.options.count() > 0, 5000))
            dialog.options.setCurrentRow(0)
            self.assertEqual(dialog.preview.scene, scene)
            self.assertEqual(foreign_scene.getNumChildren(), original_count)
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(sketch)
            self.assertFalse(module.command.IsActive())
        finally:
            dialog.close()
            from Test.GuiRecompute import close_document

            close_document(other_doc)

    def test_apply_returns_while_owner_work_is_pending(self):
        _module, _sketch, dialog = self.make_dialog()
        import SketchReferenceRepair as repair_module

        entered, release = threading.Event(), threading.Event()
        original = repair_module.broken_references
        threads = []

        def stalled(target):
            if target.Document.HasPendingTransaction:
                threads.append(QtCore.QThread.currentThread())
                entered.set()
                if not release.wait(5):
                    raise RuntimeError("test owner work timed out")
            return original(target)

        try:
            with patch.object(repair_module, "broken_references", stalled):
                QtCore.QTimer.singleShot(100, release.set)
                started = time.monotonic()
                dialog.apply()
                self.assertLess(
                    time.monotonic() - started, 0.5, "apply blocked the GUI"
                )
                # A sync call will have waited for the timer/test timeout here.
                self.assertFalse(release.is_set(), "apply blocked the GUI")
                self.assertTrue(self.wait_until(entered.is_set, 5000))
                self.assertTrue(self.wait_until(lambda: not dialog.busy, 10000))
            self.assertTrue(threads)
            self.assertTrue(
                all(t != QtCore.QCoreApplication.instance().thread() for t in threads)
            )
            self.assertEqual(dialog.references.count(), 0, dialog.message.text())
        finally:
            release.set()
            dialog.close()

    def test_ranking_yields_to_gui_timer(self):
        _module, sketch, dialog = self.make_dialog()
        from SketchReferenceRepair import candidates

        ref = dialog.refs[0]
        expected = candidates(sketch, ref)
        dialog.ranked = []
        tick = []
        QtCore.QTimer.singleShot(0, lambda: tick.append(True))
        try:
            dialog.select_reference(0)
            self.assertEqual(
                dialog.ranked, [], "ranking completed on the GUI call stack"
            )
            self.assertTrue(
                self.wait_until(lambda: len(dialog.ranked) == len(expected), 5000)
            )
            self.assertTrue(tick)
        finally:
            dialog.close()
