# SPDX-License-Identifier: LGPL-2.1-or-later
"""The global diagnostics command must be observational and select its rows."""

import unittest

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtGui, QtWidgets
from TestExpressionDiagnostics import close_document, save_document_as
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

    def test_menu_in_multiple_workbenches(self):
        original = Gui.activeWorkbench().name()
        try:
            for workbench in ("PartWorkbench", "SketcherWorkbench"):
                Gui.activateWorkbench(workbench)
                actions = Gui.getMainWindow().findChildren(QtGui.QAction)
                self.assertTrue(any(a.objectName() == "Std_ExpressionDiagnostics" for a in actions))
        finally:
            Gui.activateWorkbench(original)
