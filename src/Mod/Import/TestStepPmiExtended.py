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
        real_property = StepPmi._property

        def fail_once(document, records, warnings=()):
            calls.append(document.Name)
            if len(calls) == 1:
                if partial:
                    def fail_property(obj, *args):
                        if obj.Name.startswith("StepPMIDimension"):
                            raise RuntimeError("Injected partial PMI failure")
                        return real_property(obj, *args)

                    with patch.object(StepPmi, "_property", side_effect=fail_property):
                        return real_create(document, records, warnings)
                raise RuntimeError("Injected PMI failure")
            return real_create(document, records, warnings)

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
                                    and "#132" in warning for warning in doc.StepPMI.Warnings), doc.StepPMI.Warnings)
                self.assertIn(".UNMAPPED_TEST_MODIFIER.", doc.StepPMI.RawMetadata)
                self.assertTrue(all(not obj.Warnings for obj in doc.StepPMI.Group))
                self.assertEqual([obj.DatumIdentifier for obj in tol.Datums], ["α", "B"])
                target = os.path.join(self.directory.name, "unmapped.FCStd")
                doc.recompute()
                doc.saveAs(target)
                App.closeDocument(doc.Name)
                restored = App.openDocument(target)
                tol = next(obj for obj in restored.StepPMI.Group if obj.Category == "Tolerance")
                self.assertTrue(any(".UNMAPPED_TEST_MODIFIER." in warning for warning in restored.StepPMI.Warnings))

    def load_fixture(self, name, links):
        doc = App.newDocument("PmiRegression")
        Import.insert(os.path.join(os.path.dirname(FIXTURE), name + ".step"), doc.Name,
                      merge=False, useLinkGroup=links, semanticPMI=True)
        return doc

    def test_signed_source_deviations(self):
        for name, lower, upper in (("independent-inch", -0.01, 0.02),
                                   ("positive-deviations", 0.01, 0.02)):
            for links in (False, True):
                with self.subTest(name=name, links=links):
                    doc = self.load_fixture(name, links)
                    dim = next(obj for obj in doc.StepPMI.Group if obj.Kind == "linear distance")
                    self.assertTrue(dim.HasDeviations)
                    self.assertAlmostEqual(dim.LowerDeviation, lower * 25.4)
                    self.assertAlmostEqual(dim.UpperDeviation, upper * 25.4)
                    self.assertIn(f"{lower * 25.4:+g}/{upper * 25.4:+g}", dim.Summary)
                    self.assertIn("exact magnitude match", dim.DeviationStatus)
                    self.assertFalse(dim.Warnings)

    def test_conflicting_source_signs_are_unknown(self):
        for links in (False, True):
            doc = self.load_fixture("conflicting-signs", links)
            dim = next(obj for obj in doc.StepPMI.Group if obj.Kind == "linear distance")
            self.assertFalse(dim.HasDeviations)
            self.assertFalse(hasattr(dim, "LowerDeviation"))
            self.assertAlmostEqual(dim.LowerDeviationMagnitude, 0.254)
            self.assertAlmostEqual(dim.UpperDeviationMagnitude, 0.508)
            self.assertEqual(dim.DeviationStatus, "magnitudes only; signs unknown")
            self.assertIn("deviation magnitudes 0.254/0.508 mm (signs unknown)", dim.Summary)
            self.assertTrue(dim.Warnings)

    def test_absolute_range_bounds(self):
        for links in (False, True):
            doc = self.load_fixture("absolute-range", links)
            dim = next(obj for obj in doc.StepPMI.Group if obj.Kind == "linear distance")
            self.assertTrue(dim.HasBounds)
            self.assertFalse(dim.HasDeviations)
            self.assertAlmostEqual(dim.LowerBound, 0.9 * 25.4)
            self.assertAlmostEqual(dim.UpperBound, 1.1 * 25.4)
            self.assertIn("bounds 22.86 to 27.94 mm", dim.Summary)
            self.assertFalse(dim.DeviationStatus)

    def test_shared_placed_instances_are_not_guessed(self):
        for links in (False, True):
            doc = self.load_fixture("placed-instances", links)
            geometry = [obj for obj in doc.Objects if obj.isDerivedFrom("Part::Feature")]
            self.assertTrue(geometry)
            if links:
                instances = [obj for obj in doc.Objects if obj.TypeId == "App::Link"]
                self.assertGreaterEqual(len(instances), 2)
                self.assertEqual(len({obj.LinkedObject.Name for obj in instances}), 1)
                self.assertGreater(len({obj.Placement.Base.x for obj in instances}), 1)
            else:
                self.assertEqual(len(geometry), 3)
                self.assertGreater(len({obj.Placement.Base.x for obj in geometry}), 1)
            for obj in doc.StepPMI.Group:
                self.assertEqual(obj.ResolutionStatus, "unresolved reference")
                self.assertFalse(obj.References)
                self.assertTrue(all(ref["status"] in ("ambiguous association", "unresolved reference")
                                    for ref in json.loads(obj.RawMetadata)["record"]["references"]))

    def test_datums_in_one_compartment(self):
        for links in (False, True):
            doc = self.load_fixture("shared-compartment", links)
            tol = next(obj for obj in doc.StepPMI.Group if obj.Category == "Tolerance")
            self.assertEqual({obj.DatumIdentifier for obj in tol.Datums}, {"α", "B"})
            self.assertEqual(tol.DatumSystemPositions, [1, 1])
            self.assertTrue(all(obj.DatumPosition == 1 for obj in tol.Datums))

    def test_observer_object_survives_pmi_rollback(self):
        from unittest.mock import patch

        class Observer:
            def slotCreatedObject(self, obj):
                if obj.Name.startswith("StepPMIDimension"):
                    unrelated = obj.Document.addObject("Part::Feature", "ObserverGeometry")
                    import Part
                    unrelated.Shape = Part.makeBox(2, 3, 4)

        observer = Observer()
        real_property = StepPmi._property

        def fail_property(obj, *args):
            if obj.Name.startswith("StepPMIDimension"):
                raise RuntimeError("Forced PMI failure after observer geometry creation")
            return real_property(obj, *args)

        captured = tempfile.TemporaryFile(mode="w+b")
        stderr_copy = os.dup(2)
        App.addDocumentObserver(observer)
        try:
            os.dup2(captured.fileno(), 2)
            with patch.object(StepPmi, "_property", side_effect=fail_property):
                doc = self.load()
        finally:
            App.removeDocumentObserver(observer)
            os.dup2(stderr_copy, 2)
            os.close(stderr_copy)
        captured.seek(0)
        messages = captured.read().decode("utf-8", errors="replace").splitlines()
        captured.close()
        self.assertEqual(len(doc.Objects), 2)
        self.assertAlmostEqual(doc.ObserverGeometry.Shape.Volume, 24)
        imported = next(obj for obj in doc.Objects if obj.Name != "ObserverGeometry")
        self.assertAlmostEqual(imported.Shape.Volume, 6000)
        self.assertFalse(any(obj.Name.startswith("StepPMI") for obj in doc.Objects))
        failures = [line for line in messages if "geometry kept" in line]
        self.assertEqual(len(failures), 1, messages)
        self.assertEqual(sum("STEP PMI:" in line for line in messages), 1, messages)
        self.assertIn(FIXTURE, failures[0])

    def test_nonmatching_source_magnitudes_are_unknown(self):
        # OCCT 7.6 does not scale generic MEASURE_WITH_UNIT bounds, whereas the
        # source units require inches -> mm. Never accept a fuzzy/source-order match.
        for links in (False, True):
            doc = self.load_fixture("unmatched-signs", links)
            dim = next(obj for obj in doc.StepPMI.Group if obj.Kind == "linear distance")
            self.assertFalse(dim.HasDeviations)
            self.assertEqual(dim.DeviationStatus, "magnitudes only; signs unknown")
            self.assertAlmostEqual(dim.LowerDeviationMagnitude, 0.01)
            self.assertAlmostEqual(dim.UpperDeviationMagnitude, 0.02)
            self.assertIn("signs unknown", dim.Summary)
            self.assertTrue(dim.Warnings)

    def test_negative_upper_deviation_loss_is_reported(self):
        for links in (False, True):
            doc = self.load_fixture("negative-deviations", links)
            dim = next(obj for obj in doc.StepPMI.Group if obj.Kind == "linear distance")
            self.assertFalse(dim.HasDeviations)
            self.assertFalse(dim.HasBounds)
            self.assertFalse(dim.HasValue)
            self.assertEqual(dim.DeviationStatus,
                             "uninterpreted magnitudes; source semantics unavailable")
            self.assertEqual(len(dim.UninterpretedMagnitudes), 2)
            self.assertAlmostEqual(dim.UninterpretedMagnitudes[0], 25.4)
            self.assertAlmostEqual(dim.UninterpretedMagnitudes[1], 0.508)
            self.assertIn("uninterpreted magnitudes 25.4/0.508", dim.Summary)
            self.assertNotIn("bounds", dim.Summary)
            self.assertNotIn("LowerDeviation", dim.PropertiesList)
            self.assertTrue(any("negative upper" in warning for warning in dim.Warnings))
