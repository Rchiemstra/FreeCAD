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
        self.assertEqual(len(actual), len(expected))
        for a, b in zip(actual, expected):
            self.assertAlmostEqual(a, b, places=6)

    def variant(self, replacements=(), extra="", **options):
        """Hand-authored Part 21 variants; expected data is specified in README.txt."""
        from pathlib import Path
        text = Path(DATA, "views-mm.step").read_text()
        for old, new in replacements:
            self.assertEqual(text.count(old), 1, old)
            text = text.replace(old, new)
        text = text.replace("ENDSEC;\nEND-ISO-10303-21;", extra + "\nENDSEC;\nEND-ISO-10303-21;")
        path = Path(self.temp.name, "variant.step")
        path.write_text(text)
        doc = App.newDocument("VariantViews")
        settings = dict(merge=False, useLinkGroup=True, semanticPMI=True, savedViews=True)
        settings.update(options)
        Import.insert(str(path), doc.Name, **settings)
        doc.recompute()
        return doc

    def test_conflicting_units_followed_by_angles(self):
        doc = self.variant([
            ("#330=DRAUGHTING_MODEL('views',(#315,#316,#322,#329),#80);",
             "#330=DRAUGHTING_MODEL('views',(#315,#316,#322,#329),#340);")],
            "#340=(GEOMETRIC_REPRESENTATION_CONTEXT(3) GLOBAL_UNIT_ASSIGNED_CONTEXT((#81,#341,#84,#85)) REPRESENTATION_CONTEXT('','conflicting'));\n"
            "#341=(LENGTH_UNIT() NAMED_UNIT(*) SI_UNIT($,.METRE.));")
        views = self.records(doc.StepViews)
        for view in views:
            self.assertIn("unknown", view.Units, "conflicting lengths must not convert")
            self.assertEqual(view.PresentationStatus, "unavailable")
            self.assertIn("length units", view.UnavailableReason)
            self.assertEqual(json.loads(view.RawMetadata)["record"]["sourceLengthFactor"], 0)
            for unit in ("#81", "#341", "#84", "#85"):
                self.assertIn(unit, view.SourceUnits)
            self.vector(view.Origin, (10, 20, 30))
            self.assertTrue(all(p.PresentationStatus == "unavailable" for p in view.Planes))

    def test_shared_camera_owners_unavailable(self):
        # Second model maps a different representation and a different dimension.
        doc = self.variant(extra=
            "#340=FACETED_BREP_SHAPE_REPRESENTATION('other owner',(#351),#80);\n"
            "#341=REPRESENTATION_MAP(#320,#340);\n"
            "#342=MAPPED_ITEM('other model',#341,#320);\n"
            "#343=DRAUGHTING_MODEL('other views',(#315,#342,#347),#80);\n"
            "#344=DRAUGHTING_MODEL_ITEM_ASSOCIATION('other PMI','',#114,#343,#347);\n"
            "#345=PRODUCT_DEFINITION_SHAPE('','',#350);\n"
            "#346=SHAPE_DEFINITION_REPRESENTATION(#345,#340);\n"
            "#347=DRAUGHTING_CALLOUT('other dimension',(#328));\n"
            "#348=PRODUCT('OTHER','Other block','',(#3));\n"
            "#349=PRODUCT_DEFINITION_FORMATION('1','',#348);\n"
            "#350=PRODUCT_DEFINITION('design','',#349,#6);\n"
            "#351=FACETED_BREP('Other block',#352);\n"
            "#352=CLOSED_SHELL('',(#40,#41,#42,#43,#44,#45));")
        shared, sibling = self.records(doc.StepViews)
        self.assertFalse(shared.Shapes, "shared camera must refuse first model shape links")
        self.assertFalse(shared.Pmi, "shared camera must refuse first model PMI links")
        self.assertEqual(shared.AssociationStatus, "unavailable")
        for role in ("shape", "pmi"):
            self.assertTrue(any(role in r and "ambiguous draughting models" in r
                                for r in shared.UnresolvedReferences), shared.UnresolvedReferences)
        self.assertEqual(len(sibling.Shapes), 1)
        self.assertEqual(len(sibling.Pmi), 1)
        self.assertIn("#330", shared.DraughtingModels)
        self.assertIn("#343", shared.DraughtingModels)
        # Distinct source solids establish two independent exact owners.
        model = [o for o in doc.Objects if o.isDerivedFrom("Part::Feature") and o.Shape.Solids]
        self.assertEqual(len(model), 2, [(o.TypeId, o.Label) for o in model])

    def test_invalid_projection_dimensions(self):
        # Centering each window removes the old asymmetric-window fallback reason.
        for width, height, origin, volume, expected in (
                (0, 12, "0.,-6.,0.", "4.,2.,.T.,80.", "window dimensions"),
                (-18, 12, "9.,-6.,0.", "4.,2.,.T.,80.", "window dimensions"),
                (18, -12, "-9.,6.,0.", "4.,2.,.T.,80.", "window dimensions"),
                (18, 0, "-9.,0.,0.", "4.,2.,.T.,80.", "window dimensions"),
                (18, 12, "-9.,-6.,0.", "0.,2.,.T.,80.", "view plane distance"),
                (18, 12, "-9.,-6.,0.", "-4.,2.,.T.,80.", "view plane distance"),
                (18, 12, "-9.,-6.,0.", "4.,-2.,.T.,80.", "clipping distances"),
                (18, 12, "-9.,-6.,0.", "4.,80.,.T.,2.", "clipping distances")):
            with self.subTest(width=width, height=height, volume=volume):
                doc = self.variant([
                    ("#303=CARTESIAN_POINT('',(-7.,-5.,0.));",
                     "#303=CARTESIAN_POINT('',(" + origin + "));"),
                    ("#305=PLANAR_BOX('',18.,12.,#304);",
                     f"#305=PLANAR_BOX('',{width}.,{height}.,#304);"),
                    ("#306=VIEW_VOLUME(.PARALLEL.,#302,4.,2.,.T.,80.,.T.,.T.,#305);",
                     "#306=VIEW_VOLUME(.PARALLEL.,#302," + volume + ",.T.,.T.,#305);")])
                view = self.records(doc.StepViews)[0]
                self.assertEqual(view.PresentationStatus, "unavailable", "invalid dimensions accepted")
                self.assertIn(expected, view.UnavailableReason)
                self.vector(json.loads(view.RawMetadata)["record"]["WindowSize"], (width, height))

    def test_over_depth_clipping_preserves_views(self):
        # Alternating union/intersection nodes are valid OCCT 7.6 select operands.
        nodes = []
        for number in range(400, 470):
            kind = "UNION" if number % 2 == 0 else "INTERSECTION"
            child = "#310" if number == 469 else f"#{number + 1}"
            nodes.append(f"#{number}=CAMERA_MODEL_D3_MULTI_CLIPPING_{kind}('deep',({child}));")
        doc = self.variant([("#306,(#310,#312));", "#306,(#310,#400,#312));")],
                           "\n".join(nodes))
        groups = [o for o in doc.Objects if getattr(o, "MetadataKind", "") == "SavedViews"]
        self.assertEqual(len(groups), 1, "one deep tree discarded all camera records")
        deep, sibling = self.records(groups[0])
        self.assertEqual((deep.StepEntityId, sibling.StepEntityId), ("#315", "#316"))
        self.assertEqual(deep.PresentationStatus, "unavailable")
        self.assertIn("clipping", deep.UnavailableReason)
        self.assertIn("depth", deep.UnavailableReason)
        self.assertIn("#400", deep.RawMetadata)
        self.assertIn("#469", deep.SourceClippingTree)
        self.assertEqual({p.StepEntityId for p in deep.Planes}, {"#310", "#312"})
        self.assertEqual(len(sibling.Planes), 2)
        self.assertEqual(sibling.ClippingExpression, "(((#310*#312)+#310))")
        self.assertNotIn("depth", sibling.UnavailableReason)

    def test_missing_volume_and_unsupported_clipping_isolated(self):
        for replacements, expected in (
                ([("#301,#306,(#310,#312)", "#301,$,(#310,#312)")], "missing view volume"),
                ([("#306,(#310,#312)", "#306,(#310,#323,#312)")], "clipping")):
            with self.subTest(expected=expected):
                doc = self.variant(replacements)
                groups = [o for o in doc.Objects if getattr(o, "MetadataKind", "") == "SavedViews"]
                self.assertEqual(len(groups), 1)
                partial, sibling = self.records(groups[0])
                self.assertEqual(partial.StepEntityId, "#315")
                self.assertIn(expected, partial.UnavailableReason)
                self.assertEqual(partial.PresentationStatus, "unavailable")
                self.assertIn("#301", partial.RawMetadata)
                self.assertEqual(len(partial.Planes), 2)
                self.assertEqual(len(sibling.Planes), 2)
                self.vector(sibling.CameraPosition, (50, 22, 33))
                self.assertIn("incomplete XCAF", sibling.UnavailableReason)
                for view in (partial, sibling):
                    self.assertFalse(view.Shapes)
                    self.assertFalse(view.Pmi)
                    for role in ("shape", "pmi"):
                        self.assertTrue(any(role in r and "incomplete XCAF" in r
                                            for r in view.UnresolvedReferences))

    def test_partial_records_source_free_restore_copy(self):
        doc = self.variant([("#301,#306,(#310,#312)", "#301,$,(#310,#312)")])
        raw = [v.RawMetadata for v in self.records(doc.StepViews)]
        path = os.path.join(self.temp.name, "partial-views.FCStd")
        doc.saveAs(path)
        App.closeDocument(doc.Name)
        os.remove(os.path.join(self.temp.name, "variant.step"))
        restored = App.openDocument(path)
        self.assertEqual([v.RawMetadata for v in self.records(restored.StepViews)], raw)
        dest = App.newDocument("PartialRecordsCopy")
        dest.openTransaction("copy partial views")
        group = dest.copyObject(restored.StepViews, True)
        dest.commitTransaction()
        count = len(dest.Objects)
        dest.undo()
        self.assertEqual(len(dest.Objects), 0)
        dest.redo()
        group = dest.StepViews
        self.assertEqual(len(dest.Objects), count)
        self.assertEqual([v.RawMetadata for v in self.records(group)], raw)
        App.closeDocument(restored.Name)
        for view in self.records(group):
            self.assertEqual(view.PresentationStatus, "unavailable")
            self.assertFalse(view.Shapes + view.Pmi)
            self.assertEqual(len(view.Planes), 2)
            self.assertTrue(all(p.Document == dest for p in view.Planes))

    def test_partial_plane_keeps_valid_sibling(self):
        doc = self.variant([("#310=PLANE('positive X',#309);", "#310=PLANE('positive X',$);")])
        for view in self.records(doc.StepViews):
            self.assertEqual(view.PresentationStatus, "unavailable")
            self.assertIn("clipping plane #310", view.UnavailableReason)
            planes = {p.StepEntityId: p for p in view.Planes}
            self.assertEqual(set(planes), {"#310", "#312"})
            self.assertEqual(planes["#310"].PresentationStatus, "unavailable")
            self.assertIn("missing plane basis", planes["#310"].UnavailableReason)
            self.assertEqual(planes["#312"].PresentationStatus, "data available")
            self.vector(planes["#312"].Normal, (-1, 0, 0))

    def test_window_rotation_and_center_policy(self):
        for origin, placement, expected in (
                ("-9.,-6.,0.", "#30,#31", ""),
                ("6.,-9.,0.", "#30,#32", "rotated window"),
                ("-9.,-6.,0.", "#30,#32", "asymmetric window")):
            with self.subTest(origin=origin, placement=placement):
                doc = self.variant([
                    ("#303=CARTESIAN_POINT('',(-7.,-5.,0.));",
                     "#303=CARTESIAN_POINT('',(" + origin + "));"),
                    ("#304=AXIS2_PLACEMENT_3D('',#303,#30,#31);",
                     "#304=AXIS2_PLACEMENT_3D('',#303," + placement + ");")])
                view = self.records(doc.StepViews)[0]
                if expected:
                    self.assertEqual(view.PresentationStatus, "unavailable")
                    self.assertIn(expected, view.UnavailableReason)
                else:
                    self.assertEqual(view.PresentationStatus, "data available")
                self.vector(view.WindowOrigin, [float(v) for v in origin.split(",")])
                self.vector(view.WindowRight, (0, 1, 0) if "#32" in placement else (1, 0, 0))
                if origin == "6.,-9.,0.":
                    self.assertNotIn("asymmetric", view.UnavailableReason)

    def test_duplicate_names_keep_source_identity(self):
        doc = self.variant([("'Perspective',#301", "'Parallel \\X2\\89C656FE\\X0\\',#301")])
        first, second = self.records(doc.StepViews)
        self.assertEqual(json.loads(first.RawMetadata)["record"]["Name"],
                         json.loads(second.RawMetadata)["record"]["Name"])
        self.assertNotEqual(first.SourceId, second.SourceId)
        self.assertEqual((first.StepEntityId, second.StepEntityId), ("#315", "#316"))
        self.assertEqual((first.Projection, second.Projection), ("parallel", "perspective"))
        self.assertTrue(first.Shapes and first.Pmi and second.Shapes and second.Pmi)

    def test_incomplete_xcaf_table_refuses_all_links(self):
        # The orphan is last: OCCT has already created two labels when it returns early.
        doc = self.variant(extra="#340=CAMERA_MODEL_D3('orphan',#301,#306);")
        views = self.records(doc.StepViews)
        self.assertEqual([v.StepEntityId for v in views], ["#315", "#316", "#340"])
        for view in views:
            self.assertEqual(view.SourceId, view.StepEntityId)
            self.assertFalse(view.Shapes)
            self.assertFalse(view.Pmi)
            self.assertEqual(view.AssociationStatus, "unavailable")
            self.assertIn("incomplete XCAF", view.UnavailableReason)
            self.assertTrue(all("incomplete XCAF" in r for r in view.UnresolvedReferences))
            self.vector(view.CameraPosition, (50, 22, 33))

    def test_partial_copy_does_not_rebind_source_names(self):
        source = self.load()
        view = self.records(source.StepViews)[0]
        raw = view.RawMetadata
        dest = App.newDocument("PartialViewCopy")
        omitted_names = [o.Name for o in view.Shapes + view.Pmi + view.Planes]
        copied = dest.copyObject(view, False)
        self.assertEqual(copied.RawMetadata, raw)
        self.assertFalse(copied.Shapes)
        self.assertFalse(copied.Pmi)
        self.assertFalse(copied.Planes)
        # Later matching names must never replay provenance or create links.
        for name in omitted_names:
            dest.addObject("App::FeaturePython", name)
        path = os.path.join(self.temp.name, "partial.FCStd")
        dest.recompute()
        dest.saveAs(path)
        App.closeDocument(dest.Name)
        dest = App.openDocument(path)
        copied = dest.getObject(view.Name)
        App.closeDocument(source.Name)
        dest.recompute()
        self.assertFalse(copied.Shapes + copied.Pmi + copied.Planes)

    def test_observer_objects_survive_rollback(self):
        import StepViews
        doc = self.load()
        original = StepViews._property
        sentinels = []
        class Observer:
            def slotCreatedObject(self, obj):
                if obj.Document == doc and obj.Name.startswith("StepSavedView"):
                    sentinel = doc.addObject("App::FeaturePython", "ObserverOwned")
                    sentinels.append(sentinel.Name)
        observer = Observer()
        def fail(obj, kind, name, *args):
            if name == "CameraPosition":
                raise RuntimeError("injected metadata rollback")
            return original(obj, kind, name, *args)
        App.addDocumentObserver(observer)
        try:
            with patch.object(StepViews, "_property", side_effect=fail):
                self.load(doc=doc)
        finally:
            App.removeDocumentObserver(observer)
        self.assertEqual(len(sentinels), 1)
        self.assertIsNotNone(doc.getObject(sentinels[0]))
        self.assertEqual(len([o for o in doc.Objects
                              if getattr(o, "MetadataKind", "") == "SavedViews"]), 1)
        self.assertEqual(len([o for o in doc.Objects if o.Name.startswith("StepSavedView")]), 2)
        self.assertEqual(len([o for o in doc.Objects if o.Name.startswith("StepPMI")
                              and o.TypeId == "App::DocumentObjectGroup"]), 2)

    def test_multi_document_links_unavailable(self):
        path = os.path.join(DATA, "views-nested.step")
        for mode in (1, 2, 3, 4):
            with self.subTest(mode=mode):
                doc = App.newDocument("MultiDocumentViews")
                doc.saveAs(os.path.join(self.temp.name, doc.Name + ".FCStd"))
                Import.insert(path, doc.Name, mode=mode, merge=False, useLinkGroup=True,
                              savedViews=True, semanticPMI=True)
                views = self.records(doc.StepViews)
                self.assertEqual(len(views), 2)
                for view in views:
                    self.assertFalse(view.Shapes)
                    self.assertFalse(view.Pmi)
                    self.assertEqual(view.AssociationStatus, "unavailable")
                    for role in ("shape", "pmi"):
                        self.assertTrue(any(role in r and "multi-document" in r
                                            for r in view.UnresolvedReferences))

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
