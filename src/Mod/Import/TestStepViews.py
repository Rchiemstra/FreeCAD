# SPDX-License-Identifier: LGPL-2.1-or-later
"""Independent AP242 cameras/clipping acceptance (no display activation)."""
import json
import os
import shutil
import tempfile
import unittest
from unittest.mock import patch

import FreeCAD as App
import Import

DATA = os.path.join(os.path.dirname(__file__), "TestData", "StepViews")


class TestStepViews(unittest.TestCase):
    def setUp(self):
        self.existing = set(App.listDocuments())
        self.temp = tempfile.TemporaryDirectory(prefix="step-views-")
        self.prefs = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Import")
        self.saved = {x[1]: x[2] for x in self.prefs.GetContents() or []}
        self.prefs.SetBool("ImportSavedViews", False)

    def tearDown(self):
        for name in set(App.listDocuments()) - self.existing:
            App.closeDocument(name)
        if "ImportSavedViews" in self.saved:
            self.prefs.SetBool("ImportSavedViews", self.saved["ImportSavedViews"])
        else:
            self.prefs.RemBool("ImportSavedViews")
        self.temp.cleanup()

    def load(self, unit="mm", links=False, doc=None, **kwargs):
        doc = doc or App.newDocument("Views")
        options = dict(merge=False, useLinkGroup=links, mode=0,
                       semanticPMI=True, savedViews=True)
        options.update(kwargs)
        Import.insert(os.path.join(DATA, "views-" + unit + ".step"), doc.Name, **options)
        doc.recompute()
        return doc

    def records(self, group):
        self.assertEqual(group.MetadataVersion, 1)
        return [o for o in group.Group if o.RecordType == "SavedView"]

    def vector(self, actual, expected):
        for a, b in zip(actual, expected):
            self.assertAlmostEqual(a, b, places=6)

    def test_saved_view_clips_owned(self):
        for unit, scale in (("mm", 1), ("inch", 25.4), ("metre", 1000)):
            for links in (False, True):
                with self.subTest(unit=unit, links=links):
                    doc = self.load(unit, links)
                    views = self.records(doc.StepViews)
                    self.assertEqual(len(views), 2)
                    parallel, perspective = views
                    self.assertEqual(parallel.Label, "Parallel 视图")
                    self.assertEqual(parallel.StepEntityId, "#315")
                    self.assertEqual(parallel.Projection, "parallel")
                    self.assertEqual(perspective.Projection, "perspective")
                    for view in views:
                        self.vector(view.Origin, [v * scale for v in (10, 20, 30)])
                        self.vector(view.ProjectionPoint, [v * scale for v in (2, 3, 40)])
                        self.vector(view.CameraPosition, [v * scale for v in (50, 22, 33)])
                        self.vector(view.ViewDirection, (1, 0, 0))
                        self.vector(view.UpDirection, (0, 0, 1))
                        self.vector(view.WindowSize, (18 * scale, 12 * scale))
                        self.vector(view.WindowOrigin, (-7 * scale, -5 * scale, 0))
                        self.assertAlmostEqual(view.ViewPlaneDistance, 4 * scale)
                        self.assertAlmostEqual(view.FrontDistance, 2 * scale)
                        self.assertAlmostEqual(view.BackDistance, 80 * scale)
                        self.assertEqual(view.Units, "mm")
                        self.assertIn("asymmetric", view.UnavailableReason)
                        self.assertEqual(len(view.Planes), 2)
                        raw = json.loads(view.RawMetadata)
                        self.assertEqual(raw["schemaVersion"], 1)
                        self.assertEqual(raw["record"]["sourceLengthFactor"], scale)
                    self.assertTrue(parallel.FrontClipping)
                    self.assertFalse(perspective.FrontClipping)
                    self.assertTrue(parallel.SidesClipping)
                    self.assertEqual(parallel.ClippingExpression, "(#310*#312)")
                    self.assertEqual(perspective.ClippingExpression, "(((#310*#312)+#310))")
                    self.assertIn("union", perspective.UnavailableReason)
                    planes = sorted(parallel.Planes, key=lambda p: p.Label)
                    self.vector(planes[0].Normal, (-1, 0, 0))
                    self.vector(planes[1].Normal, (1, 0, 0))
                    self.assertAlmostEqual(planes[0].Offset, 5 * scale)
                    self.assertAlmostEqual(planes[1].Offset, -5 * scale)
                    self.assertFalse(planes[0].Capping)

    def test_view_links_scoped(self):
        for links in (False, True):
            doc = self.load(links=links)
            first = doc.StepViews
            second_before = set(o.Name for o in doc.Objects)
            self.load(links=links, doc=doc)
            second = next(o for o in doc.Objects if getattr(o, "MetadataKind", "") == "SavedViews" and o != first)
            for group in (first, second):
                for view in self.records(group):
                    self.assertEqual(len(view.Shapes), 1, view.UnresolvedReferences)
                    self.assertEqual(len(view.Pmi), 1, view.UnresolvedReferences)
                    self.assertFalse(view.UnresolvedReferences)
                    self.assertIn(view.Shapes[0], group.ImportedObjects)
                    self.assertIn(view.Pmi[0], group.ImportedObjects)
                    self.assertEqual(view.Pmi[0].Category, "Dimension")
                    self.assertAlmostEqual(view.Shapes[0].Shape.Volume, 6)
                    self.assertEqual(view.Shapes[0].Name in second_before, group == first)
                    self.assertEqual(view.Pmi[0].Name in second_before, group == first)
            no_pmi = self.load(links=links, semanticPMI=False)
            for view in self.records(no_pmi.StepViews):
                self.assertFalse(view.Pmi)
                self.assertTrue(any("PMI disabled" in x for x in view.UnresolvedReferences))
            nested = self.load("nested", links)
            for view in self.records(nested.StepViews):
                self.assertFalse(view.Shapes)
                self.assertTrue(any("occurrence" in x for x in view.UnresolvedReferences))
            merged = self.load(links=links, merge=True)
            self.assertFalse(self.records(merged.StepViews)[0].Shapes)

    def test_legacy_auxiliary_labels_excluded(self):
        doc = self.load()
        geometry = [o for o in doc.Objects if o.isDerivedFrom("Part::Feature")]
        self.assertEqual(len(geometry), 1)
        self.assertEqual(len(geometry[0].Shape.Faces), 6)
        self.assertAlmostEqual(geometry[0].Shape.Volume, 6)
        self.assertTrue(geometry[0].Shape.isValid())
        # Explicit disable still uses the legacy path, including OCCT auxiliary labels.
        off = self.load(savedViews=False)
        self.assertFalse(any(getattr(o, "MetadataKind", "") == "SavedViews" for o in off.Objects))
        self.assertGreater(len([o for o in off.Objects if o.isDerivedFrom("Part::Feature")]), 1)

    def test_source_free_restore_copy_undo(self):
        source = os.path.join(self.temp.name, "supplier.step")
        shutil.copyfile(os.path.join(DATA, "views-inch.step"), source)
        doc = App.newDocument("PersistentViews")
        doc.openTransaction("view import")
        Import.insert(source, doc.Name, merge=False, useLinkGroup=True,
                      savedViews=True, semanticPMI=True)
        doc.commitTransaction()
        count = len(doc.Objects)
        doc.undo()
        self.assertEqual(len(doc.Objects), 0)
        doc.redo()
        self.assertEqual(len(doc.Objects), count)
        path = os.path.join(self.temp.name, "views.FCStd")
        doc.recompute()
        doc.saveAs(path)
        App.closeDocument(doc.Name)
        os.remove(source)
        restored = App.openDocument(path)
        view = self.records(restored.StepViews)[0]
        self.vector(view.CameraPosition, (1270, 558.8, 838.2))
        dest = App.newDocument("CopiedViews")
        copied = dest.copyObject(restored.StepViews, True)
        for obj in self.records(copied):
            for owner in obj.Shapes + obj.Pmi + obj.Planes:
                self.assertEqual(owner.Document, dest)
            self.assertTrue(obj.Shapes)
            self.assertTrue(obj.Pmi)
        App.closeDocument(restored.Name)
        self.assertEqual(len(self.records(copied)), 2)

    def test_partial_import_failure_rollback(self):
        import StepViews
        for links in (False, True):
            doc = self.load(links=links)
            old = {o.Name for o in doc.Objects}
            original = StepViews._property
            def fail(obj, kind, name, *args):
                if name == "CameraPosition":
                    raise RuntimeError("injected partial view metadata failure")
                return original(obj, kind, name, *args)
            with patch.object(StepViews, "_property", side_effect=fail):
                self.load(links=links, doc=doc)
            self.assertTrue(old.issubset({o.Name for o in doc.Objects}))
            self.assertEqual(len([o for o in doc.Objects if getattr(o, "MetadataKind", "") == "SavedViews"]), 1)
            self.assertEqual(len([o for o in doc.Objects if o.TypeId == "App::DocumentObjectGroup" and o.Name.startswith("StepPMI")]), 2)
            self.assertEqual(len([o for o in doc.Objects if o.isDerivedFrom("Part::Feature")]), 2)
            self.load(links=links, doc=doc)
            self.assertEqual(len([o for o in doc.Objects if getattr(o, "MetadataKind", "") == "SavedViews"]), 2)

    def test_default_preference_and_open(self):
        path = os.path.join(DATA, "views-mm.step")
        doc = App.newDocument("DefaultViews")
        self.prefs.RemBool("ImportSavedViews")
        Import.insert(path, doc.Name, merge=False, useLinkGroup=True)
        self.assertFalse(any(getattr(o, "MetadataKind", "") == "SavedViews" for o in doc.Objects))
        self.prefs.SetBool("ImportSavedViews", True)
        Import.open(path, merge=False, useLinkGroup=True)
        self.assertEqual(len(self.records(App.ActiveDocument.StepViews)), 2)
        disabled = self.load(savedViews=False)
        self.assertFalse(any(getattr(o, "MetadataKind", "") == "SavedViews" for o in disabled.Objects))

    def test_unknown_units_and_omitted_owner(self):
        # A source with a context-dependent length has no conversion to mm.
        from pathlib import Path
        source = Path(self.temp.name) / "unknown.step"
        text = Path(DATA, "views-mm.step").read_text()
        text = text.replace("#81=(LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT(.MILLI.,.METRE.));",
                            "#81=(CONTEXT_DEPENDENT_UNIT('supplier unit') LENGTH_UNIT() NAMED_UNIT(#86));")
        source.write_text(text)
        doc = App.newDocument("UnknownViewUnits")
        Import.insert(str(source), doc.Name, merge=False, useLinkGroup=True,
                      semanticPMI=False, savedViews=True)
        view = self.records(doc.StepViews)[0]
        self.assertIn("unknown", view.Units)
        self.assertIn("length units", view.UnavailableReason)
        self.assertIn("supplier unit", view.SourceUnits)
        self.vector(view.Origin, (10, 20, 30))
        self.assertEqual(json.loads(view.RawMetadata)["record"]["sourceLengthFactor"], 0)
        # Owner observations cannot be reconstructed from similarly named geometry.
        import StepViews
        record = json.loads(view.RawMetadata)["record"]
        record["references"] = [{"sourceId": "0:1:1:999", "role": "shape",
                                  "scope": "definition", "objectName": "",
                                  "reason": "omitted exact owner"}]
        group = StepViews.create(doc, [record])
        copied_view = self.records(group)[0]
        self.assertFalse(copied_view.Shapes)
        self.assertTrue(any("omitted exact owner" in r for r in copied_view.UnresolvedReferences))

    def test_real_geometry_placements_hierarchy_unchanged(self):
        for links in (False, True):
            enabled = self.load("nested", links)
            disabled = self.load("nested", links, savedViews=False)
            def model(doc):
                result = []
                for obj in doc.Objects:
                    if obj.isDerivedFrom("Part::Feature") and obj.Shape.Solids:
                        result.append((obj.TypeId, obj.Label, round(obj.Shape.Volume, 5),
                                       str(getattr(obj, "Placement", None))))
                    elif obj.TypeId in ("App::Part", "App::Link", "App::LinkGroup"):
                        result.append((obj.TypeId, obj.Label, str(getattr(obj, "Placement", None))))
                return sorted(result)
            self.assertEqual(model(enabled), model(disabled))
            self.assertTrue(model(enabled))

    def test_repeated_insert_undo_redo_isolation(self):
        for links in (False, True):
            doc = self.load(links=links)
            first = doc.StepViews
            old_names = {o.Name for o in doc.Objects}
            doc.openTransaction("second saved-view import")
            self.load(doc=doc, links=links)
            doc.commitTransaction()
            second = next(o for o in doc.Objects
                          if getattr(o, "MetadataKind", "") == "SavedViews" and o != first)
            second_name = second.Name
            doc.undo()
            self.assertEqual({o.Name for o in doc.Objects}, old_names)
            self.assertTrue(self.records(first)[0].Shapes)
            doc.redo()
            second = doc.getObject(second_name)
            for group in (first, second):
                for view in self.records(group):
                    self.assertTrue(view.Shapes)
                    self.assertTrue(view.Pmi)
                    self.assertEqual(view.Shapes[0].Name in old_names, group == first)
                    self.assertEqual(view.Pmi[0].Name in old_names, group == first)
