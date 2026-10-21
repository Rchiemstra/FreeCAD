# SPDX-License-Identifier: LGPL-2.1-or-later
"""Headless acceptance tests for opt-in semantic STEP PMI import."""
import json
import os
import shutil
import tempfile
import unittest

import FreeCAD as App
import Import
import Part
import StepPmi
from TestStepPmiExtended import StepPmiExtendedTests


FIXTURE = os.path.join(os.path.dirname(__file__), "TestData", "StepPmi", "semantic.step")


class TestStepPmi(StepPmiExtendedTests, unittest.TestCase):
    def setUp(self):
        self.existing = set(App.listDocuments())
        self.directory = tempfile.TemporaryDirectory(prefix="freecad-step-pmi-")
        self.prefs = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Import")
        self.saved = {item[1]: item[2] for item in (self.prefs.GetContents() or [])}
        self.prefs.SetBool("ImportSemanticPMI", False)

    def tearDown(self):
        for name in set(App.listDocuments()) - self.existing:
            App.closeDocument(name)
        if "ImportSemanticPMI" in self.saved:
            self.prefs.SetBool("ImportSemanticPMI", self.saved["ImportSemanticPMI"])
        else:
            self.prefs.RemBool("ImportSemanticPMI")
        self.directory.cleanup()

    def load(self, links=False, saved=False, **options):
        doc = App.newDocument("PmiTest")
        if saved:
            doc.saveAs(os.path.join(self.directory.name, doc.Name + ".FCStd"))
        settings = dict(merge=False, useLinkGroup=links, mode=0, semanticPMI=True)
        settings.update(options)
        Import.insert(FIXTURE, doc.Name, **settings)
        doc.recompute()
        return doc

    def check_semantics(self, doc, resolved=True):
        groups = [obj for obj in doc.Objects if obj.Label == "STEP PMI"]
        self.assertEqual(len(groups), 1)
        self.assertEqual(groups[0].MetadataVersion, 1)
        self.assertEqual(len(groups[0].Group), 3)
        records = {obj.Category: obj for obj in groups[0].Group}
        self.assertEqual(set(records), {"Dimension", "Tolerance", "Datum"})
        dim, tol, datum = (records[k] for k in ("Dimension", "Tolerance", "Datum"))
        self.assertEqual(dim.Label, "Dimension 1")
        self.assertEqual(dim.Kind, "linear distance")
        self.assertEqual(dim.Units, "mm")
        self.assertAlmostEqual(dim.Value, 10)
        self.assertTrue(dim.HasDeviations)
        self.assertFalse(dim.HasBounds)
        self.assertAlmostEqual(dim.LowerDeviation, -0.1)
        self.assertAlmostEqual(dim.UpperDeviation, 0.2)
        self.assertEqual(dim.Summary, "linear distance 10 mm, -0.1/+0.2 mm")
        self.assertEqual(tol.Kind, "perpendicularity")
        self.assertEqual(tol.Units, "mm")
        self.assertAlmostEqual(tol.Value, 0.05)
        self.assertEqual(datum.Label, "Datum A")
        self.assertEqual(datum.DatumIdentifier, "A")
        self.assertEqual(datum.DatumPosition, 1)
        self.assertEqual(tol.Datums, [datum])
        self.assertEqual(tol.DatumSystemPositions, [1])
        self.assertEqual(tol.Summary, "perpendicularity 0.05 mm; datum A")
        raw = json.loads(dim.RawMetadata)
        self.assertEqual(raw["schemaVersion"], 1)
        self.assertEqual([ref["role"] for ref in raw["record"]["references"]], ["first", "second"])
        self.assertEqual(len(raw["record"]["values"]), 3)
        self.assertIn("ReadOnly", dim.getEditorMode("Value"))
        self.assertIn("ReadOnly", tol.getEditorMode("Datums"))
        self.assertIn("ReadOnly", datum.getEditorMode("RawMetadata"))
        if resolved:
            for obj in records.values():
                self.assertEqual(obj.ResolutionStatus, "resolved", obj.UnresolvedReferences)
                self.assertTrue(obj.References)
                self.assertFalse(obj.UnresolvedReferences)
                self.assertFalse(obj.Warnings)
            self.assertEqual(sum(len(subs) for _, subs in dim.References), 2)
            self.assertEqual(sum(len(subs) for _, subs in tol.References), 1)
            # Compare geometry, not just face numbers: opposite sides separated by 10 mm.
            faces = [owner.Shape.getElement(sub) for owner, subs in dim.References for sub in subs]
            self.assertAlmostEqual(faces[0].distToShape(faces[1])[0], 10)
            tf = [owner.Shape.getElement(sub) for owner, subs in tol.References for sub in subs]
            df = [owner.Shape.getElement(sub) for owner, subs in datum.References for sub in subs]
            self.assertTrue(tf[0].isSame(faces[0]))
            self.assertTrue(df[0].isSame(faces[1]))
        return records

    def test_legacy(self):
        self.check_semantics(self.load(False))

    def test_links(self):
        self.check_semantics(self.load(True))

    def test_open_api(self):
        Import.open(FIXTURE, merge=False, useLinkGroup=True, semanticPMI=True)
        self.check_semantics(App.ActiveDocument)

    def roundtrip(self, links):
        source = os.path.join(self.directory.name, "supplier.step")
        shutil.copyfile(FIXTURE, source)
        doc = App.newDocument("PmiPersistent")
        Import.insert(source, doc.Name, merge=False, useLinkGroup=links, semanticPMI=True)
        self.check_semantics(doc)
        target = os.path.join(self.directory.name, "part.FCStd")
        doc.recompute()
        doc.saveAs(target)
        App.closeDocument(doc.Name)
        os.remove(source)
        restored = App.openDocument(target)
        restored.recompute()
        self.check_semantics(restored)

    def test_save_reopen_legacy(self):
        self.roundtrip(False)

    def test_save_reopen_links(self):
        self.roundtrip(True)

    def test_option_off(self):
        self.prefs.SetBool("ImportSemanticPMI", True)
        for links in (False, True):
            doc = self.load(links, semanticPMI=False)
            self.assertFalse(any(obj.Label == "STEP PMI" for obj in doc.Objects))
            self.assertEqual(len(doc.Objects), 1)

    def test_preference(self):
        self.prefs.SetBool("ImportSemanticPMI", True)
        doc = App.newDocument("PmiPreference")
        Import.insert(FIXTURE, doc.Name, merge=False, useLinkGroup=False)
        self.check_semantics(doc)

    def test_default_off(self):
        self.prefs.RemBool("ImportSemanticPMI")
        doc = App.newDocument("PmiDefaultOff")
        Import.insert(FIXTURE, doc.Name, merge=False, useLinkGroup=False)
        self.assertEqual(len(doc.Objects), 1)
        self.assertFalse(any(obj.Label == "STEP PMI" for obj in doc.Objects))

    def test_no_pmi_unchanged(self):
        source_doc = App.newDocument("PlainSource")
        shape = source_doc.addObject("Part::Feature", "Box")
        shape.Shape = Part.makeBox(10, 20, 30)
        path = os.path.join(self.directory.name, "plain.step")
        Import.export([shape], path)
        for links in (False, True):
            snapshots = []
            for enabled in (False, True):
                doc = App.newDocument("PlainImport")
                Import.insert(path, doc.Name, merge=False, useLinkGroup=links, semanticPMI=enabled)
                snapshots.append([(obj.TypeId, obj.Label, obj.Shape.Volume,
                                   sorted(obj.PropertiesList)) for obj in doc.Objects])
            self.assertEqual(snapshots[0], snapshots[1])

    def test_merge_unsupported(self):
        for links in (False, True):
            doc = self.load(links, merge=True)
            records = self.check_semantics(doc, resolved=False)
            for obj in records.values():
                self.assertEqual(obj.ResolutionStatus, "unsupported association")
                self.assertFalse(obj.References)
                self.assertTrue(obj.UnresolvedReferences)

    def test_multi_document_unsupported(self):
        for mode in (1, 2, 3, 4):
            with self.subTest(mode=mode):
                doc = self.load(True, saved=True, mode=mode)
                records = self.check_semantics(doc, resolved=False)
                for obj in records.values():
                    self.assertEqual(obj.ResolutionStatus, "unsupported association")
                    self.assertFalse(obj.References)

    def test_unresolved_owner(self):
        imported = self.load()
        records = [json.loads(obj.RawMetadata)["record"] for obj in imported.StepPMI.Group]
        # A consumer must not silently resolve a lost object by name or guess a face.
        empty = App.newDocument("UnavailableGeometry")
        StepPmi.create(empty, records)
        result = self.check_semantics(empty, resolved=False)
        for obj in result.values():
            self.assertEqual(obj.ResolutionStatus, "unresolved reference")
            self.assertFalse(obj.References)
            self.assertTrue(obj.UnresolvedReferences)

    def test_repeated_insert_scopes_datums(self):
        doc = self.load(True)
        Import.insert(FIXTURE, doc.Name, merge=False, useLinkGroup=True, semanticPMI=True)
        groups = [obj for obj in doc.Objects
                  if obj.TypeId == "App::DocumentObjectGroup" and hasattr(obj, "MetadataVersion")]
        self.assertEqual(len(groups), 2)
        for group in groups:
            records = {obj.Category: obj for obj in group.Group}
            self.assertEqual(records["Tolerance"].Datums, [records["Datum"]])
            for obj in records.values():
                self.assertEqual(obj.ResolutionStatus, "resolved")
