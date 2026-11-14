# SPDX-License-Identifier: LGPL-2.1-or-later
"""Export acceptance checks mixed into TestStepPmi."""
import json
import os
import re
from unittest.mock import patch

import FreeCAD as App
import Import
import Part


class StepPmiExportTests:
    def test_export_omitted_owner_with_shared_shape(self):
        for legacy in (False, True):
            doc = self.load()
            owner = doc.StepPMI.Group[0].References[0][0]
            other = doc.addObject("Part::Feature", "SharedShape")
            other.Shape = owner.Shape
            assembly = doc.addObject("App::Part", "Assembly")
            assembly.addObject(other)
            path = os.path.join(self.directory.name, "omitted.step")
            report = Import.export([assembly], path, legacy=legacy, semanticPMI=True)
            self.assertEqual(len(report), 3)
            self.assertTrue(all(r["status"] == "skipped" and "not exported" in r["reason"]
                                for r in report), report)
            self.assertFalse(hasattr(self.reload_pmi(path), "StepPMI"))

    def test_export_nested_assembly_instances(self):
        doc = self.load()
        owner = doc.StepPMI.Group[0].References[0][0]
        assembly = doc.addObject("App::Part", "Subassembly")
        assembly.addObject(owner)
        instance = doc.addObject("App::Link", "Instance")
        instance.setLink(assembly)
        instance.Placement.Base = App.Vector(100, 0, 0)
        path = os.path.join(self.directory.name, "nested.step")
        report = Import.export([assembly, instance], path, semanticPMI=True)
        self.assertEqual(len(report), 3)
        self.assertTrue(all(r["status"] == "skipped" and "ambiguous" in r["reason"]
                            for r in report), report)
        self.assertFalse(hasattr(self.reload_pmi(path), "StepPMI"))

    def test_export_deleted_datum(self):
        doc = self.load()
        tolerance = next(o for o in doc.StepPMI.Group if o.Category == "Tolerance")
        doc.removeObject(tolerance.Datums[0].Name)
        self.assertEqual(tolerance.Datums, [])
        path, report = self.export_pmi(doc)
        row = next(r for r in report if r["object"] == tolerance.Name)
        self.assertEqual(row["status"], "skipped", report)
        self.assertIn("datum", row["reason"])
        restored = self.reload_pmi(path)
        self.assertEqual([o.Category for o in restored.StepPMI.Group], ["Dimension"])

    def test_export_changed_live_references(self):
        for change in ("remove", "replace", "add"):
            doc = self.load()
            dim = next(o for o in doc.StepPMI.Group if o.Category == "Dimension")
            references = dim.References
            owner, subs = references[0]
            flattened = [(obj, [sub]) for obj, names in references for sub in names]
            if change == "remove":
                dim.References = flattened[:1]
            elif change == "replace":
                other = doc.addObject("Part::Feature", "Other")
                other.Shape = owner.Shape
                dim.References = [(other, names) for _, names in flattened]
            else:
                dim.References = flattened + [(owner, ["Face3"])]
            self.assertTrue(dim.References)
            path, report = self.export_pmi(doc)
            row = next(r for r in report if r["object"] == dim.Name)
            self.assertEqual(row["status"], "skipped", (change, report))
            self.assertIn("reference", row["reason"])
            restored = self.reload_pmi(path)
            self.assertFalse(any(getattr(o, "Category", "") == "Dimension"
                                 for o in restored.Objects))

    def test_export_projected_zone_units(self):
        # Extend the checked-in millimetre fixture with a 25.4 mm projected zone.
        fixture = os.path.join(os.path.dirname(__file__), "TestData", "StepPmi", "semantic.step")
        with open(fixture, encoding="utf-8") as stream:
            source = stream.read()
        zone = ("#373 = TOLERANCE_ZONE_FORM('cylindrical or circular');\n"
                "#374 = TOLERANCE_ZONE('','',#4,.F.,(#372),#373);\n"
                "#375 = LENGTH_MEASURE_WITH_UNIT(LENGTH_MEASURE(25.4),#346);\n"
                "#376 = PROJECTED_ZONE_DEFINITION(#374,(),#370,#375);\n")
        fixture = os.path.join(self.directory.name, "projected.step")
        with open(fixture, "w", encoding="utf-8") as stream:
            stream.write(source.replace("ENDSEC;\nEND-ISO", zone + "ENDSEC;\nEND-ISO"))
        doc = App.newDocument("Projected")
        Import.insert(fixture, doc.Name, merge=False, useLinkGroup=False, semanticPMI=True)
        tolerance = next(o for o in doc.StepPMI.Group if o.Category == "Tolerance")
        numbers = json.loads(tolerance.RawMetadata)["record"]["numbers"]
        self.assertEqual(numbers["zoneModifierCode"], 1)
        self.assertAlmostEqual(numbers["zoneModifierValue"], 25.4)
        settings = Part.exportUnits()
        try:
            for unit, expected in (("MM", 25.4), ("INCH", 1.0), ("M", 0.0254)):
                Part.setStaticValue("write.step.unit", unit)
                path, report = self.export_pmi(doc)
                self.assertTrue(all(r["status"] == "written" for r in report), report)
                with open(path, encoding="utf-8") as stream:
                    text = stream.read()
                projection = re.search(r"PROJECTED_ZONE_DEFINITION\([^;]*,\s*#(\d+)\)", text)
                self.assertIsNotNone(projection, text[-3000:])
                measure = re.search(r"#" + projection[1] + r"\s*=\s*"
                                    r"LENGTH_MEASURE_WITH_UNIT\(LENGTH_MEASURE\(([^)]+)\)", text)
                self.assertIsNotNone(measure)
                self.assertAlmostEqual(float(measure[1]), expected)
                restored = self.reload_pmi(path)
                after = next(o for o in restored.StepPMI.Group if o.Category == "Tolerance")
                values = json.loads(after.RawMetadata)["record"]["numbers"]
                self.assertEqual(values["zoneModifierCode"], 1)
                self.assertAlmostEqual(values["zoneModifierValue"], 25.4)
                self.assertAlmostEqual(after.Value, tolerance.Value)
        finally:
            Part.setStaticValue("write.step.unit", settings["write.step.unit"])

    def export_pmi(self, doc, **options):
        path = os.path.join(self.directory.name, "export.step")
        shapes = [o for o in doc.Objects if o.isDerivedFrom("Part::Feature")]
        report = Import.export(shapes, path, semanticPMI=True, **options)
        return path, report

    def reload_pmi(self, path):
        doc = App.newDocument("PmiExportRestored")
        Import.insert(path, doc.Name, merge=False, useLinkGroup=False, semanticPMI=True)
        return doc

    def test_export_roundtrip(self):
        for links, legacy in ((False, False), (True, False), (False, True)):
            doc = self.load(links)
            path, report = self.export_pmi(doc, legacy=legacy)
            self.assertEqual([r["status"] for r in report], ["written"] * 3)
            with open(path, encoding="utf-8") as stream:
                # Typed measure selects are required even if a reader accepts bare reals.
                self.assertRegex(stream.read(),
                                 r"LENGTH_MEASURE_WITH_UNIT\s*\(\s*LENGTH_MEASURE\(-")
            self.check_semantics(self.reload_pmi(path))

    def test_export_signed_deviations_and_modifiers(self):
        for fixture in ("independent-inch", "positive-deviations", "absolute-range"):
            doc = self.load_fixture(fixture, False)
            path, report = self.export_pmi(doc)
            self.assertTrue(all(r["status"] == "written" for r in report), report)
            restored = self.reload_pmi(path)
            for before, after in zip(doc.StepPMI.Group, restored.StepPMI.Group):
                for prop in ("Category", "Kind", "HasBounds", "HasDeviations", "Modifiers"):
                    self.assertEqual(getattr(before, prop), getattr(after, prop))
                for prop in ("Value", "LowerBound", "UpperBound",
                             "LowerDeviation", "UpperDeviation"):
                    if hasattr(before, prop):
                        self.assertAlmostEqual(getattr(before, prop), getattr(after, prop))
            if fixture == "independent-inch":
                self.check_independent(restored)

    def test_export_changed_or_removed_face(self):
        for removed in (False, True):
            doc = self.load()
            owner = doc.StepPMI.Group[0].References[0][0]
            owner.Shape = Part.makeBox(3, 4, 5) if not removed else Part.Shape()
            _, report = self.export_pmi(doc)
            self.assertEqual(len(report), 3)
            self.assertTrue(all(r["status"] == "skipped" for r in report), report)
            self.assertTrue(all("changed" in r["reason"] or "removed" in r["reason"]
                                for r in report), report)

    def test_export_missing_reference_and_unsupported_type(self):
        doc = self.load()
        other = doc.addObject("Part::Feature", "Other")
        other.Shape = Part.makeBox(1, 2, 3)
        path = os.path.join(self.directory.name, "other.step")
        report = Import.export([other], path, semanticPMI=True)
        self.assertEqual(len(report), 3)
        self.assertTrue(all("not exported" in r["reason"] for r in report), report)
        dim = next(o for o in doc.StepPMI.Group if o.Category == "Dimension")
        import json
        raw = json.loads(dim.RawMetadata)
        raw["record"]["type"] = 999
        dim.RawMetadata = json.dumps(raw)
        _, report = self.export_pmi(doc)
        self.assertTrue(any(r["object"] == dim.Name and "unsupported" in r["reason"]
                            for r in report), report)

    def test_export_placed_and_shared_instances(self):
        doc = self.load()
        owner = doc.StepPMI.Group[0].References[0][0]
        owner.Placement.Base = App.Vector(50, 20, 10)
        path, report = self.export_pmi(doc, keepPlacement=True)
        self.assertTrue(all(r["status"] == "written" for r in report), report)
        restored = self.reload_pmi(path)
        self.check_semantics(restored)
        restored_owner = restored.StepPMI.Group[0].References[0][0]
        self.assertAlmostEqual(owner.Shape.BoundBox.XMin, restored_owner.Shape.BoundBox.XMin)
        # Slice 1 explicitly marks shared source occurrences ambiguous.
        doc = self.load_fixture("placed-instances", False)
        _, report = self.export_pmi(doc)
        self.assertTrue(all(r["status"] == "skipped" for r in report), report)
        self.assertTrue(all("unresolved" in r["reason"] for r in report), report)

    def test_export_off_and_failure_keep_geometry_bytes(self):
        doc = self.load()
        owner = doc.StepPMI.Group[0].References[0][0]
        path = os.path.join(self.directory.name, "same.step")
        # Explicit false must also win over an enabled saved preference.
        self.prefs.SetBool("ExportSemanticPMI", True)

        def content():
            with open(path, encoding="utf-8") as stream:
                return re.sub(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d", "TIMESTAMP", stream.read())

        Import.export([owner], path, semanticPMI=False)
        expected = content()
        self.assertIsNone(Import.export([owner], path, semanticPMI=False))
        self.assertEqual(expected, content())
        self.assertNotIn("GEOMETRIC_TOLERANCE", expected)
        geometry_only = self.reload_pmi(path)
        self.assertFalse(hasattr(geometry_only, "StepPMI"))
        self.assertAlmostEqual(geometry_only.Objects[0].Shape.Volume, owner.Shape.Volume)
        import StepPmiExport
        with patch.object(StepPmiExport, "prepare", side_effect=RuntimeError("injected failure")):
            report = Import.export([owner], path, semanticPMI=True)
        self.assertEqual(expected, content())
        self.assertEqual(len(report), 3)
        self.assertTrue(all(r["status"] == "skipped" for r in report), report)
        import sys
        with patch.dict(sys.modules, {"StepPmiExport": None}):
            report = Import.export([owner], path, semanticPMI=True)
        self.assertEqual(expected, content())
        self.assertEqual(len(report), 3)
        self.assertTrue(all(r["status"] == "skipped" for r in report), report)

    def test_export_success_preserves_geometry_and_assembly(self):
        from StepPmiExport import fingerprint

        def snapshot(doc):
            result = []
            for obj in doc.Objects:
                if hasattr(obj, "Category") or hasattr(obj, "MetadataVersion"):
                    continue
                placement = tuple(round(v, 9) for v in obj.Placement.toMatrix().A)
                children = tuple(child.Name for child in getattr(
                    obj, "ElementList", getattr(obj, "Group", [])))
                linked = getattr(obj, "LinkedObject", None)
                geometry = fingerprint(obj) if hasattr(obj, "Shape") else None
                result.append((obj.Name, obj.Label, obj.TypeId, placement, children,
                               linked.Name if linked else None, geometry))
            return result

        for legacy in (False, True):
            doc = self.load()
            owner = doc.StepPMI.Group[0].References[0][0]
            owner.Label = "PMI plate"
            owner.Placement = App.Placement(App.Vector(50, 20, 10),
                                            App.Rotation(App.Vector(0, 0, 1), 30))
            subassembly = doc.addObject("App::Part", "Subassembly")
            subassembly.Label = "Located subassembly"
            subassembly.addObject(owner)
            subassembly.Placement.Base = App.Vector(5, 2, 1)
            assembly = doc.addObject("App::Part", "Assembly")
            assembly.Label = "Export assembly"
            assembly.addObject(subassembly)
            other = doc.addObject("Part::Feature", "Other")
            other.Label = "Companion cylinder"
            other.Shape = Part.makeCylinder(2, 7)
            other.Placement.Base = App.Vector(100, 10, 5)
            assembly.addObject(other)
            path = os.path.join(self.directory.name, "geometry.step")
            Import.export([assembly], path, legacy=legacy, semanticPMI=False)
            baseline = App.newDocument("GeometryBaseline")
            Import.insert(path, baseline.Name, merge=False, useLinkGroup=True, semanticPMI=False)
            expected = snapshot(baseline)
            self.assertTrue(any(row[-1] for row in expected))
            self.assertTrue(any(row[4] for row in expected))
            report = Import.export([assembly], path, legacy=legacy, semanticPMI=True)
            self.assertEqual([r["status"] for r in report], ["written"] * 3)
            restored = App.newDocument("GeometryWithPmi")
            Import.insert(path, restored.Name, merge=False, useLinkGroup=True, semanticPMI=True)
            self.assertEqual(snapshot(restored), expected)
            self.check_semantics(restored, resolved=False)

    def test_export_after_document_restore(self):
        doc = self.load()
        target = os.path.join(self.directory.name, "export.FCStd")
        doc.saveAs(target)
        App.closeDocument(doc.Name)
        restored = App.openDocument(target)
        path, report = self.export_pmi(restored)
        self.assertTrue(all(r["status"] == "written" for r in report), report)
        self.check_semantics(self.reload_pmi(path))

    def test_export_partial_failure_keeps_geometry_bytes(self):
        import StepPmiExport
        doc = self.load()
        owner = doc.StepPMI.Group[0].References[0][0]
        path = os.path.join(self.directory.name, "same.step")
        Import.export([owner], path, semanticPMI=False)
        with open(path, encoding="utf-8") as stream:
            expected = stream.read()
        original = StepPmiExport.prepare

        def fail_after_datum(owners):
            candidates = original(owners)
            for item in candidates:
                if item["record"]["category"] == "Dimension":
                    del item["record"]["numbers"]["qualifier"]
            return candidates

        with patch.object(StepPmiExport, "prepare", side_effect=fail_after_datum):
            report = Import.export([owner], path, semanticPMI=True)
        with open(path, encoding="utf-8") as stream:
            actual = stream.read()
        normalize = lambda text: re.sub(r"\d{4}-\d\d-\d\dT\d\d:\d\d:\d\d", "TIME", text)
        self.assertEqual(normalize(expected), normalize(actual))
        self.assertTrue(all(r["status"] == "skipped" for r in report), report)

    def test_export_units_and_preference(self):
        doc = self.load()
        prefs = self.prefs
        saved = prefs.GetBool("ExportSemanticPMI", False)
        had_pref = any(item[1] == "ExportSemanticPMI" for item in prefs.GetContents() or [])
        settings = Part.exportUnits()
        try:
            prefs.SetBool("ExportSemanticPMI", True)
            for unit in ("MM", "INCH", "M"):
                Part.setStaticValue("write.step.unit", unit)
                path = os.path.join(self.directory.name, "units.step")
                shapes = [o for o in doc.Objects if o.isDerivedFrom("Part::Feature")]
                report = Import.export(shapes, path)
                self.assertEqual([r["status"] for r in report], ["written"] * 3)
                self.check_semantics(self.reload_pmi(path))
                self.assertIsNone(Import.export(shapes, path, semanticPMI=False))
        finally:
            Part.setStaticValue("write.step.unit", settings["write.step.unit"])
            if had_pref:
                prefs.SetBool("ExportSemanticPMI", saved)
            else:
                prefs.RemBool("ExportSemanticPMI")

    def test_export_incomplete_source_is_reported(self):
        for fixture in ("conflicting-signs", "negative-deviations", "unmapped-modifier"):
            doc = self.load_fixture(fixture, False)
            path, report = self.export_pmi(doc)
            skipped = [r for r in report if r["status"] == "skipped"]
            self.assertTrue(skipped, (fixture, report))
            self.assertTrue(all(r["reason"] for r in skipped), report)
            if fixture == "unmapped-modifier":
                self.assertEqual(len(skipped), len(doc.StepPMI.Group))

    def test_export_placed_assembly(self):
        doc = self.load()
        owner = doc.StepPMI.Group[0].References[0][0]
        assembly = doc.addObject("App::Part", "Assembly")
        assembly.addObject(owner)
        owner.Placement.Base = App.Vector(50, 20, 10)
        assembly.Placement.Base = App.Vector(5, 2, 1)
        path = os.path.join(self.directory.name, "assembly.step")
        report = Import.export([assembly], path, semanticPMI=True)
        self.assertTrue(all(r["status"] == "written" for r in report), report)
        self.check_semantics(self.reload_pmi(path))
        instance = doc.addObject("App::Link", "Instance")
        instance.setLink(owner)
        instance.Placement.Base = App.Vector(100, 0, 0)
        assembly.addObject(instance)
        report = Import.export([assembly], path, semanticPMI=True)
        self.assertTrue(all(r["status"] == "skipped" for r in report), report)

    def test_export_duplicate_datum_identifiers_are_not_rebound(self):
        doc = self.load()
        fixture = os.path.join(os.path.dirname(__file__), "TestData", "StepPmi", "semantic.step")
        Import.insert(fixture, doc.Name, merge=False, useLinkGroup=False, semanticPMI=True)
        path, report = self.export_pmi(doc)
        datum_rows = [r for r in report if "Datum" in r["object"]]
        self.assertEqual(len(datum_rows), 2)
        self.assertTrue(all(r["status"] == "skipped" and "duplicate datum" in r["reason"]
                            for r in datum_rows), report)
        restored = self.reload_pmi(path)
        self.assertTrue(all(o.Category == "Dimension" for o in restored.StepPMI.Group))
