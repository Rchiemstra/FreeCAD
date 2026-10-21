# SPDX-License-Identifier: LGPL-2.1-or-later
"""Additional acceptance checks mixed into the registered TestStepPmi suite."""
import json
import os
import shutil
import tempfile

import FreeCAD as App
import Import
import StepPmi


FIXTURE = os.path.join(os.path.dirname(__file__), "TestData", "StepPmi", "semantic.step")


class StepPmiExtendedTests:

    def check_independent(self, doc):
        self.assertEqual(len(doc.StepPMI.Group), 5)
        dims = {obj.Kind: obj for obj in doc.StepPMI.Group if obj.Category == "Dimension"}
        dim, angle = dims["linear distance"], dims["location angular"]
        self.assertEqual(dim.Units, "mm")
        self.assertAlmostEqual(dim.Value, 25.4)
        self.assertAlmostEqual(dim.LowerDeviation, -0.254)
        self.assertAlmostEqual(dim.UpperDeviation, 0.508)
        for actual, expected in zip(dim.Values, [25.4, 0.254, 0.508]):
            self.assertAlmostEqual(actual, expected)
        self.assertEqual(dim.SemanticName, "linear distance")
        self.assertEqual(angle.Units, "deg")
        self.assertAlmostEqual(angle.Value, 90)
        self.assertEqual(angle.SemanticName, "角度")
        tol = next(obj for obj in doc.StepPMI.Group if obj.Category == "Tolerance")
        self.assertEqual(tol.Kind, "angularity")
        self.assertEqual(tol.Units, "mm")
        self.assertAlmostEqual(tol.Value, 0.127)
        self.assertEqual(tol.SemanticName, "傾斜度")
        self.assertEqual(tol.Modifiers, ["Free_State"])
        self.assertEqual([obj.DatumIdentifier for obj in tol.Datums], ["α", "B"])
        self.assertEqual(tol.DatumSystemPositions, [1, 2])
        self.assertEqual(tol.Datums[0].Label, "Datum α")
        for obj in doc.StepPMI.Group:
            self.assertEqual(obj.ResolutionStatus, "resolved", obj.UnresolvedReferences)
            self.assertTrue(obj.References)
            self.assertFalse(obj.Warnings)
        faces = [owner.Shape.getElement(sub) for owner, subs in dim.References for sub in subs]
        self.assertAlmostEqual(faces[0].distToShape(faces[1])[0], 25.4)
        owner = dim.References[0][0]
        self.assertTrue(owner.Shape.isValid())
        self.assertEqual(len(owner.Shape.Faces), 6)
        self.assertAlmostEqual(owner.Shape.Volume, 6 * 25.4 ** 3)
        refs = lambda obj: [owner.Shape.getElement(sub)
                            for owner, subs in obj.References for sub in subs]
        self.assertTrue(refs(tol)[0].isSame(faces[1]))
        self.assertTrue(refs(tol.Datums[0])[0].isSame(faces[0]))
        self.assertTrue(refs(tol.Datums[1])[0].isSame(refs(angle)[1]))
        return dim, angle, tol

    def test_independent_inches_angles_unicode(self):
        path = os.path.join(os.path.dirname(FIXTURE), "independent-inch.step")
        for links in (False, True):
            with self.subTest(links=links):
                doc = App.newDocument("IndependentPmi")
                Import.insert(path, doc.Name, merge=False, useLinkGroup=links, semanticPMI=True)
                self.check_independent(doc)

    def test_independent_save_reopen_unicode(self):
        source = os.path.join(self.directory.name, "independent.step")
        shutil.copyfile(os.path.join(os.path.dirname(FIXTURE), "independent-inch.step"), source)
        doc = App.newDocument("IndependentPersistent")
        Import.insert(source, doc.Name, merge=False, useLinkGroup=True, semanticPMI=True)
        self.check_independent(doc)
        target = os.path.join(self.directory.name, "independent.FCStd")
        doc.recompute()
        doc.saveAs(target)
        App.closeDocument(doc.Name)
        os.remove(source)
        self.check_independent(App.openDocument(target))

    def check_soft_failure(self, opening, partial):
        from unittest.mock import patch

        template = self.load()
        snapshots = [json.loads(obj.RawMetadata)["record"] for obj in template.StepPMI.Group]
        calls = []
        real_create = StepPmi.create

        def fail_once(document, records):
            calls.append(document.Name)
            if len(calls) == 1:
                if partial:
                    group = document.addObject("App::DocumentObjectGroup", "StepPMI")
                    group.addObject(document.addObject("App::FeaturePython", "StepPMIDimension"))
                raise RuntimeError("Injected PMI failure")
            return real_create(document, records)

        # The C++ Console writes to the native stderr descriptor.
        import sys
        sys.stderr.flush()
        captured = tempfile.TemporaryFile(mode="w+b")
        stderr_copy = os.dup(2)
        os.dup2(captured.fileno(), 2)
        try:
            with patch.object(StepPmi, "create", side_effect=fail_once):
                for attempt in range(2):
                    if opening:
                        Import.open(FIXTURE, merge=False, useLinkGroup=False, semanticPMI=True)
                        doc = App.ActiveDocument
                    else:
                        doc = App.newDocument("PmiFailure")
                        # A pre-existing PMI group must survive a failed second insert.
                        Import.insert(FIXTURE, doc.Name, merge=False,
                                      useLinkGroup=False, semanticPMI=False)
                        real_create(doc, snapshots)
                        existing = {obj.Name for obj in doc.Objects}
                        Import.insert(FIXTURE, doc.Name, merge=False,
                                      useLinkGroup=False, semanticPMI=True)
                    geometry = [obj for obj in doc.Objects if obj.isDerivedFrom("Part::Feature")]
                    self.assertTrue(geometry)
                    self.assertAlmostEqual(geometry[-1].Shape.Volume, 6000)
                    if attempt == 0:
                        if opening:
                            self.assertFalse(any(hasattr(obj, "Category") for obj in doc.Objects))
                            self.assertEqual(len(doc.Objects), 1)
                        else:
                            self.assertEqual({obj.Name for obj in doc.Objects} - existing,
                                             {geometry[-1].Name})
                    elif opening:
                        self.check_semantics(doc)
                    else:
                        self.assertEqual(sum(hasattr(obj, "MetadataVersion") for obj in doc.Objects), 2)
        finally:
            os.dup2(stderr_copy, 2)
            os.close(stderr_copy)
        captured.seek(0)
        warnings = captured.read().decode("utf-8", errors="replace").splitlines()
        captured.close()
        self.assertGreaterEqual(len(calls), 2)
        failures = [message for message in warnings if "geometry kept" in message]
        self.assertEqual(len(failures), 1, warnings)
        self.assertIn(FIXTURE, failures[0])

    def test_open_soft_failure(self):
        self.check_soft_failure(opening=True, partial=False)

    def test_insert_soft_failure(self):
        self.check_soft_failure(opening=False, partial=False)

    def test_open_partial_pmi_cleanup(self):
        self.check_soft_failure(opening=True, partial=True)

    def test_insert_partial_pmi_cleanup(self):
        self.check_soft_failure(opening=False, partial=True)

    def test_unmapped_step_modifier_is_reported(self):
        path = os.path.join(os.path.dirname(FIXTURE), "unmapped-modifier.step")
        for links in (False, True):
            with self.subTest(links=links):
                doc = App.newDocument("UnmappedModifier")
                Import.insert(path, doc.Name, merge=False, useLinkGroup=links, semanticPMI=True)
                self.assertEqual(len(doc.StepPMI.Group), 5)
                tol = next(obj for obj in doc.StepPMI.Group if obj.Category == "Tolerance")
                self.assertAlmostEqual(tol.Value, 0.127)
                self.assertEqual(tol.ResolutionStatus, "resolved")
                self.assertTrue(any(".UNMAPPED_TEST_MODIFIER." in warning
                                    and "#132" in warning for warning in tol.Warnings), tol.Warnings)
                self.assertIn(".UNMAPPED_TEST_MODIFIER.", tol.RawMetadata)
                self.assertEqual([obj.DatumIdentifier for obj in tol.Datums], ["α", "B"])
                target = os.path.join(self.directory.name, "unmapped.FCStd")
                doc.recompute()
                doc.saveAs(target)
                App.closeDocument(doc.Name)
                restored = App.openDocument(target)
                tol = next(obj for obj in restored.StepPMI.Group if obj.Category == "Tolerance")
                self.assertTrue(any(".UNMAPPED_TEST_MODIFIER." in warning for warning in tol.Warnings))
