# SPDX-License-Identifier: LGPL-2.1-or-later
import FreeCAD as App
import FreeCADGui as Gui
import Part
from PySide import QtGui

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
            dialog.options.setCurrentRow(0)
            self.assertTrue(dialog.apply_button.isEnabled())
            dialog.apply()
            self.assertEqual(dialog.references.count(), 0, dialog.message.text())
            self.assertIsNone(dialog.preview.node)
            self.assertEqual(self.doc.UndoCount, undo_count + 1)
        finally:
            dialog.close()
            self.flush_gui()
        self.undo_gui_document()
        self.recompute_gui_document()
        from SketchReferenceRepair import broken_references

        self.assertEqual(len(broken_references(sketch)), 1)
