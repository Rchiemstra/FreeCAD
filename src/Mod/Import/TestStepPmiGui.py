# SPDX-License-Identifier: LGPL-2.1-or-later
"""Semantic STEP PMI GUI acceptance; also discovered through TestImportGui."""
import os
import unittest

import FreeCAD as App
import ImportGui
from Test.GuiRecompute import close_document, recompute_document


class StepPmiGuiTest(unittest.TestCase):
    """GUI import, selection and the shared File > Import preferences page."""

    def setUp(self):
        self.doc = App.newDocument("GuiPmi")
        self.prefs = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Import")
        self.step_prefs = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Part/STEP")
        self.saved = self.prefs.GetBool("ImportSemanticPMI", False)
        self.had_pref = any(item[1] == "ImportSemanticPMI"
                            for item in self.prefs.GetContents() or [])
        self.dialog_saved = self.step_prefs.GetBool("VisibleImportDialog", True)
        self.dialog_had_pref = any(item[1] == "VisibleImportDialog"
                                   for item in self.step_prefs.GetContents() or [])
        self.fixture = os.path.join(os.path.dirname(__file__), "TestData", "StepPmi", "semantic.step")

    def tearDown(self):
        import FreeCADGui as Gui

        Gui.Selection.clearSelection()
        close_document(self.doc)
        if self.had_pref:
            self.prefs.SetBool("ImportSemanticPMI", self.saved)
        else:
            self.prefs.RemBool("ImportSemanticPMI")
        if self.dialog_had_pref:
            self.step_prefs.SetBool("VisibleImportDialog", self.dialog_saved)
        else:
            self.step_prefs.RemBool("VisibleImportDialog")

    def check_selection(self):
        import FreeCADGui as Gui

        recompute_document(self.doc)
        self.assertEqual(self.doc.StepPMI.Label, "STEP PMI")
        self.assertEqual(len(self.doc.StepPMI.Group), 3)
        for obj in self.doc.StepPMI.Group:
            self.assertEqual(obj.ResolutionStatus, "resolved")
            self.assertTrue(obj.Summary)
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(obj)
            self.assertIn(obj, Gui.Selection.getSelection())
            self.assertTrue(obj.References)
            for owner, subs in obj.References:
                for sub in subs:
                    Gui.Selection.addSelection(owner, sub)
                    self.assertIn(owner, Gui.Selection.getSelection())
                    self.assertTrue(any(sub in selected.SubElementNames
                                        for selected in Gui.Selection.getSelectionEx()))
            Gui.updateGui()

    def test_preference_and_selection(self):
        self.prefs.SetBool("ImportSemanticPMI", True)
        ImportGui.insert(self.fixture, self.doc.Name, merge=False, useLinkGroup=False, mode=0)
        self.check_selection()

    def test_keyword_and_selection(self):
        self.prefs.SetBool("ImportSemanticPMI", False)
        ImportGui.insert(self.fixture, self.doc.Name, merge=False,
                         useLinkGroup=True, mode=0, semanticPMI=True)
        self.check_selection()

    def test_option_off(self):
        self.prefs.SetBool("ImportSemanticPMI", True)
        ImportGui.insert(self.fixture, self.doc.Name, merge=False,
                         useLinkGroup=True, mode=0, semanticPMI=False)
        recompute_document(self.doc)
        self.assertEqual(len(self.doc.Objects), 1)
        self.assertFalse(any(hasattr(obj, "Category") for obj in self.doc.Objects))

    def test_default_off(self):
        self.prefs.RemBool("ImportSemanticPMI")
        ImportGui.insert(self.fixture, self.doc.Name, merge=False, useLinkGroup=False, mode=0)
        recompute_document(self.doc)
        self.assertEqual(len(self.doc.Objects), 1)

    def test_import_dialog_saves_and_restores_option(self):
        from PySide import QtCore, QtWidgets

        self.step_prefs.SetBool("VisibleImportDialog", True)
        self.prefs.RemBool("ImportSemanticPMI")
        observed = []
        errors = []

        def accept_dialog():
            # Catch callback errors here so none escape through QApplication::notify.
            dialog = QtWidgets.QApplication.activeModalWidget()
            try:
                checkbox = dialog.findChild(QtWidgets.QCheckBox, "checkBoxSemanticPMI")
                self.assertIsNotNone(checkbox)
                observed.append(checkbox.isChecked())
                checkbox.setChecked(True)
                dialog.accept()
            except Exception as error:
                errors.append(error)
                if dialog is not None:
                    dialog.reject()

        for _ in range(2):
            QtCore.QTimer.singleShot(0, accept_dialog)
            options = ImportGui.importOptions(self.fixture)
            self.assertFalse(errors, errors)
            self.assertTrue(self.prefs.GetBool("ImportSemanticPMI", False))
        self.assertEqual(observed, [False, True])
        options.update(merge=False, useLinkGroup=False, mode=0)
        ImportGui.insert(self.fixture, self.doc.Name, options=options)
        self.check_selection()

    def test_export_dialog_option_and_roundtrip(self):
        import tempfile
        import Part
        from PySide import QtCore, QtWidgets

        prefs = self.prefs
        general = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Part/General")
        touched = [(prefs, {"ExportSemanticPMI": "Bool", "ExportHiddenObject": "Bool",
                            "ExportLegacy": "Bool", "ExportKeepPlacement": "Bool"}),
                   (self.step_prefs, {"VisibleExportDialog": "Bool", "Scheme": "String",
                                      "Unit": "Int"}),
                   (general, {"WriteSurfaceCurveMode": "Int"})]
        saved = [(group, keys, {item[1]: item[2] for item in group.GetContents() or []})
                 for group, keys in touched]
        runtime_unit = Part.exportUnits()["write.step.unit"]
        runtime = {}
        observed, errors = [], []

        def accept_dialog():
            dialog = QtWidgets.QApplication.activeModalWidget()
            try:
                checkbox = dialog.findChild(QtWidgets.QCheckBox, "checkBoxSemanticPMI")
                self.assertIsNotNone(checkbox)
                if not runtime:
                    # Absent preferences expose the live OCCT settings here.
                    schema = dialog.findChild(QtWidgets.QComboBox, "comboBoxSchema")
                    runtime["write.step.schema"] = bytes(schema.currentData()).decode("ascii")
                    runtime["write.surfacecurve.mode"] = int(dialog.findChild(
                        QtWidgets.QCheckBox, "checkBoxPcurves").isChecked())
                observed.append(checkbox.isChecked())
                checkbox.setChecked(True)
                dialog.accept()
            except Exception as error:
                errors.append(error)
                if dialog is not None:
                    dialog.reject()

        try:
            for group, keys in touched:
                for key, kind in keys.items():
                    getattr(group, "Rem" + kind)(key)
            Part.setStaticValue("write.step.unit", "INCH")
            self.step_prefs.SetBool("VisibleExportDialog", True)
            with tempfile.TemporaryDirectory() as directory:
                path = os.path.join(directory, "export.step")
                for _ in range(2):
                    QtCore.QTimer.singleShot(0, accept_dialog)
                    options = ImportGui.exportOptions(path)
                    self.assertFalse(errors, errors)
                    self.assertTrue(options["semanticPMI"])
                self.assertEqual(observed, [False, True])
                ImportGui.insert(self.fixture, self.doc.Name, merge=False,
                                 useLinkGroup=False, semanticPMI=True)
                shapes = [o for o in self.doc.Objects if o.isDerivedFrom("Part::Feature")]
                report = ImportGui.export(shapes, path, options=options)
                self.assertEqual([r["status"] for r in report], ["written"] * 3)
                # Explicit false overrides the saved preference when no options dict is passed.
                self.assertIsNone(ImportGui.export(shapes, path, semanticPMI=False))
                report = ImportGui.export(shapes, path, semanticPMI=True)
                self.assertEqual([r["status"] for r in report], ["written"] * 3)
                restored = App.newDocument("GuiPmiRestored")
                try:
                    ImportGui.insert(path, restored.Name, merge=False,
                                     useLinkGroup=False, semanticPMI=True)
                    self.assertEqual(len(restored.StepPMI.Group), 3)
                    self.assertTrue(all(o.ResolutionStatus == "resolved"
                                        for o in restored.StepPMI.Group))
                finally:
                    close_document(restored)
        finally:
            for group, keys, values in saved:
                for key, kind in keys.items():
                    if key in values:
                        getattr(group, "Set" + kind)(key, values[key])
                    else:
                        getattr(group, "Rem" + kind)(key)
            for key, value in runtime.items():
                Part.setStaticValue(key, value)
            Part.setStaticValue("write.step.unit", runtime_unit)
