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

    def test_user_attribute_options_and_dialog(self):
        from PySide import QtCore, QtWidgets
        fixture = os.path.join(os.path.dirname(__file__), 'TestData', 'StepAttributes', 'attributes-mm.step')
        saved = {x[1]: x[2] for x in self.prefs.GetContents() or []}
        try:
            self.prefs.SetBool('ImportUserAttributes', True)
            ImportGui.insert(fixture, self.doc.Name, merge=False, useLinkGroup=True, mode=0)
            self.assertIsNotNone(self.doc.getObject('StepAttributes'), 'GUI attribute preference ignored')
            self.prefs.RemBool('ImportUserAttributes')
            ImportGui.insert(fixture, self.doc.Name, merge=False, useLinkGroup=False, mode=0,
                             userAttributes=False)
            self.assertEqual(len([o for o in self.doc.Objects
                                  if getattr(o, 'MetadataKind', '') == 'UserAttributes']), 1)
            self.step_prefs.SetBool('VisibleImportDialog', True)
            observed, errors = [], []
            def accept_dialog():
                dialog = QtWidgets.QApplication.activeModalWidget()
                try:
                    checkbox = dialog.findChild(QtWidgets.QCheckBox, 'checkBoxUserAttributes')
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
                options = ImportGui.importOptions(fixture)
                self.assertFalse(errors, errors)
                self.assertTrue(options['userAttributes'])
                self.assertTrue(self.prefs.GetBool('ImportUserAttributes', False))
            self.assertEqual(observed, [False, True])
            options.update(merge=False, useLinkGroup=False, mode=0)
            ImportGui.insert(fixture, self.doc.Name, options=options, userAttributes=False)
            self.assertEqual(len([o for o in self.doc.Objects
                                  if getattr(o, 'MetadataKind', '') == 'UserAttributes']), 1)
            self.prefs.SetBool('ImportUserAttributes', False)
            ImportGui.open(fixture, options=options, merge=False, useLinkGroup=True, mode=0)
            opened = App.ActiveDocument
            try:
                self.assertIsNotNone(opened.getObject('StepAttributes'))
                self.assertAlmostEqual(next(r for r in opened.StepAttributes.Group
                                            if r.SupplierName == 'span').Value.Value, 2)
                import json
                import FreeCADGui as Gui
                import StepAttributes
                active = App.ActiveDocument
                camera = Gui.activeDocument().activeView().getCamera()
                records = [json.loads(r.RawMetadata)['record'] for r in opened.StepAttributes.Group]
                for record in records:
                    record['ObjectName'] = ''
                    record['OwnerReason'] = 'no imported owner in this materialization'
                # Creating data in a different document must not activate it or a camera.
                StepAttributes.create(self.doc, records)
                self.assertEqual(App.ActiveDocument, active)
                self.assertEqual(Gui.activeDocument().activeView().getCamera(), camera)
            finally:
                if opened != self.doc:
                    close_document(opened)
        finally:
            if 'ImportUserAttributes' in saved:
                self.prefs.SetBool('ImportUserAttributes', saved['ImportUserAttributes'])
            else:
                self.prefs.RemBool('ImportUserAttributes')

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

    def test_saved_views_options_and_dialog(self):
        """The same native option works in GUI open, insert and import dialog."""
        from PySide import QtCore, QtWidgets
        import FreeCADGui as Gui
        from unittest.mock import patch
        import StepViews

        prefs = self.prefs
        saved = {x[1]: x[2] for x in prefs.GetContents() or []}
        existing = set(App.listDocuments())
        path = os.path.join(os.path.dirname(__file__), "TestData", "StepViews", "views-mm.step")
        observed, errors = [], []

        def accept_dialog():
            dialog = QtWidgets.QApplication.activeModalWidget()
            try:
                box = dialog.findChild(QtWidgets.QCheckBox, "checkBoxSavedViews")
                self.assertIsNotNone(box)
                observed.append(box.isChecked())
                box.setChecked(True)
                dialog.accept()
            except Exception as error:
                errors.append(error)
                if dialog is not None:
                    dialog.reject()

        try:
            prefs.RemBool("ImportSavedViews")
            self.step_prefs.SetBool("VisibleImportDialog", True)
            for _ in range(2):
                QtCore.QTimer.singleShot(0, accept_dialog)
                options = ImportGui.importOptions(path)
                self.assertFalse(errors, errors)
                self.assertTrue(options["savedViews"])
            self.assertEqual(observed, [False, True])
            options.update(merge=False, useLinkGroup=False, mode=0)
            ImportGui.insert(path, self.doc.Name, options=options, savedViews=False)
            self.assertFalse(any(getattr(o, "MetadataKind", "") == "SavedViews" for o in self.doc.Objects))
            for links in (False, True):
                options["useLinkGroup"] = links
                ImportGui.insert(path, self.doc.Name, options=options, semanticPMI=True)
                group = [o for o in self.doc.Objects if getattr(o, "MetadataKind", "") == "SavedViews"][-1]
                views = [o for o in group.Group if o.RecordType == "SavedView"]
                self.assertEqual(len(views), 2)
                self.assertTrue(all(o.Shapes and o.Pmi for o in views))
            # Metadata creation must itself leave camera and active document untouched.
            camera = Gui.activeDocument().activeView().getCamera()
            active = App.ActiveDocument
            records = [__import__("json").loads(o.RawMetadata)["record"] for o in group.Group]
            StepViews.create(self.doc, records)
            self.assertEqual(Gui.activeDocument().activeView().getCamera(), camera)
            self.assertEqual(App.ActiveDocument, active)
            original = StepViews._property
            def fail(obj, kind, name, *args):
                if name == "CameraPosition":
                    raise RuntimeError("injected GUI view failure")
                return original(obj, kind, name, *args)
            before = len([o for o in self.doc.Objects if getattr(o, "MetadataKind", "") == "SavedViews"])
            with patch.object(StepViews, "_property", side_effect=fail):
                ImportGui.insert(path, self.doc.Name, savedViews=True, semanticPMI=True,
                                 merge=False, useLinkGroup=False)
            self.assertEqual(len([o for o in self.doc.Objects if getattr(o, "MetadataKind", "") == "SavedViews"]), before)
            ImportGui.open(path, savedViews=True, merge=False, useLinkGroup=True)
            self.assertTrue(App.ActiveDocument.StepViews.Group)
        finally:
            for name in set(App.listDocuments()) - existing:
                close_document(App.getDocument(name))
            if "ImportSavedViews" in saved:
                prefs.SetBool("ImportSavedViews", saved["ImportSavedViews"])
            else:
                prefs.RemBool("ImportSavedViews")

    def test_saved_views_geometry_colors_hierarchy_parity(self):
        """Combined metadata capture preserves native geometry in both converters."""
        import tempfile
        from pathlib import Path

        existing = set(App.listDocuments())
        text = Path(__file__).with_name("TestData").joinpath(
            "StepAttributes", "attributes-nested.step").read_text()
        # Independently authored surface style: RGB (0.8, 0.2, 0.1) on solid #11.
        style = """
#500=COLOUR_RGB('supplier red',0.8,0.2,0.1);
#501=FILL_AREA_STYLE_COLOUR('',#500);
#502=FILL_AREA_STYLE('',(#501));
#503=SURFACE_STYLE_FILL_AREA(#502);
#504=SURFACE_SIDE_STYLE('',(#503));
#505=SURFACE_STYLE_USAGE(.BOTH.,#504);
#506=PRESENTATION_STYLE_ASSIGNMENT((#505));
#507=STYLED_ITEM('',(#506),#11);
#508=MECHANICAL_DESIGN_GEOMETRIC_PRESENTATION_REPRESENTATION('',(#507),#80);
"""
        text = text.replace("ENDSEC;\nEND-ISO-10303-21;", style + "ENDSEC;\nEND-ISO-10303-21;")

        def model(doc):
            result, colors = [], []
            for obj in doc.Objects:
                solid = obj.isDerivedFrom("Part::Feature") and obj.Shape.Solids
                if not solid and obj.TypeId not in ("App::Part", "App::Link", "App::LinkGroup"):
                    continue
                children = getattr(obj, "Group", getattr(obj, "ElementList", []))
                hierarchy = sorted((o.TypeId, o.Label) for o in children)
                appearance = tuple(tuple(c) for c in getattr(obj.ViewObject, "DiffuseColor", []))
                colors.extend(appearance)
                result.append((obj.TypeId, obj.Label, str(getattr(obj, "Placement", None)),
                               round(obj.Shape.Volume, 5) if solid else None,
                               hierarchy, appearance))
            return sorted(result), colors

        try:
            with tempfile.TemporaryDirectory(prefix="gui-step-views-") as directory:
                path = Path(directory, "colored-views.step")
                path.write_text(text)
                for links in (False, True):
                    signatures = []
                    for enabled in (False, True):
                        doc = App.newDocument("ColorViewParity")
                        ImportGui.insert(str(path), doc.Name, merge=False, useLinkGroup=links,
                                         mode=0, savedViews=enabled, semanticPMI=enabled,
                                         userAttributes=enabled)
                        recompute_document(doc)
                        signature, colors = model(doc)
                        self.assertTrue(signature)
                        self.assertTrue(any(all(abs(a-b) < 1e-5 for a, b in
                                                zip(color[:3], (0.8, 0.2, 0.1)))
                                            for color in colors), colors)
                        signatures.append(signature)
                        if enabled:
                            self.assertEqual(len(doc.StepPMI.Group), 5)
                            self.assertEqual(len(doc.StepViews.Group), 4)
                            self.assertEqual(len(doc.StepAttributes.Group), 11)
                    self.assertEqual(signatures[0], signatures[1])
        finally:
            for name in set(App.listDocuments()) - existing:
                close_document(App.getDocument(name))
