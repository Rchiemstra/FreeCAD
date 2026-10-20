# SPDX-License-Identifier: LGPL-2.1-or-later
"""Registered integration tests for saved-file model comparison."""

import os
import shutil
import struct
import subprocess
import tempfile
import unittest
import xml.etree.ElementTree as ET
import zipfile
from pathlib import Path
from unittest.mock import patch

import FreeCAD as App
import modeldiff as Diff
import Part
from Test.GuiRecompute import (
    close_document,
    close_document_by_name,
    recompute_document,
    save_document_as,
)


class ModelDiffTests(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(prefix="freecad-modeldiff-")
        self.local = str(Path(self.tmp.name) / "local model.FCStd")
        self.remote = str(Path(self.tmp.name) / "remote model.FCStd")
        self.original_docs = set(App.listDocuments())
        self.doc = App.newDocument("ModelDiffFixture")

    def tearDown(self):
        for name in set(App.listDocuments()) - self.original_docs:
            close_document_by_name(name)
        self.tmp.cleanup()

    def save(self, remote=False):
        recompute_document(self.doc)
        save_document_as(self.doc, self.remote if remote else self.local)

    def box(self, name="Box"):
        return self.doc.addObject("Part::Box", name)

    def compare(self):
        before = set(App.listDocuments())
        result = Diff.compare_files(self.local, self.remote)
        self.assertEqual(set(App.listDocuments()), before)
        return result

    def changes(self, result):
        return {change.name: change for change in result.objects}

    def test_identical_file(self):
        self.assertIn("TestModelDiff", App.__unit_test__)
        self.box()
        self.save()
        result = Diff.compare_files(self.local, self.local)
        self.assertTrue(result.identical, result.report())
        self.assertIn("No differences", result.report())
        self.assertTrue(all(v.role == "Unchanged" for v in result.visuals))

    def test_identical_copy(self):
        self.box()
        self.save()
        shutil.copyfile(self.local, self.remote)
        self.assertTrue(self.compare().identical)

    def test_document_properties(self):
        self.doc.Comment = "Before"
        self.doc.addProperty("App::PropertyInteger", "ReviewNumber")
        self.doc.ReviewNumber = 1
        self.doc.addProperty("App::PropertyInteger", "Counter")
        large = (1 << (struct.calcsize("l") * 8 - 2)) + 1
        self.doc.Counter = large
        self.doc.addProperty("App::PropertyFloat", "Precision")
        self.doc.Precision = 10.000000000000002
        self.save()
        self.doc.Comment = "After"
        self.doc.ReviewNumber = 2
        self.doc.Counter = large + 1
        self.doc.Precision = 10.000000000000004
        self.save(True)
        result = self.compare()
        self.assertEqual(result.properties["Comment"], ("Before", "After"))
        self.assertEqual(result.properties["ReviewNumber"], ("1", "2"))
        self.assertEqual(result.properties["Counter"], (str(large), str(large + 1)))
        self.assertEqual(
            result.properties["Precision"], ("10.000000000000002", "10.000000000000004")
        )
        self.assertIn("ReviewNumber: 1 -> 2", result.report())

    def test_added_removed_objects(self):
        self.box("RemovedBox")
        self.save()
        self.doc.removeObject("RemovedBox")
        self.box("AddedBox")
        self.save(True)
        result = self.compare()
        changes = self.changes(result)
        self.assertEqual(changes["RemovedBox"].kind, "Removed")
        self.assertEqual(changes["AddedBox"].kind, "Added")
        self.assertAlmostEqual(changes["AddedBox"].measures[1], 1000)
        self.assertEqual(
            {(v.group, v.role) for v in result.visuals},
            {("Removed", "Removed"), ("Added", "Added")},
        )

    def test_renamed_label_pairs_by_name(self):
        box = self.box()
        box.Label = "Before label"
        self.save()
        box.Label = "After label"
        self.save(True)
        with patch.object(Diff, "_solid_parts", wraps=Diff._solid_parts) as booleans:
            result = self.compare()
        self.assertEqual(booleans.call_count, 0)
        change = self.changes(result)["Box"]
        self.assertEqual(change.kind, "Changed")
        self.assertEqual(change.properties["Label"], ("Before label", "After label"))
        self.assertEqual(
            [(v.group, v.role) for v in result.visuals], [("Changed", "Unchanged")]
        )

    def test_changed_type_same_internal_name(self):
        self.box()
        self.save()
        self.doc.removeObject("Box")
        self.doc.addObject("Part::Cylinder", "Box")
        self.save(True)
        change = self.changes(self.compare())["Box"]
        self.assertEqual(change.kind, "Changed")
        self.assertEqual(change.properties["TypeId"], ("Part::Box", "Part::Cylinder"))

    def test_property_added_and_removed(self):
        box = self.box()
        box.addProperty("App::PropertyString", "Before")
        box.Before = "old value"
        box.addProperty("App::PropertyString", "FileName")
        box.FileName = "before.step"
        self.save()
        box.removeProperty("Before")
        box.addProperty("App::PropertyString", "After")
        box.After = "new value"
        box.FileName = "after.step"
        self.save(True)
        props = self.changes(self.compare())["Box"].properties
        self.assertEqual(props["Before"], ("old value", None))
        self.assertEqual(props["After"], (None, "new value"))
        self.assertEqual(props["FileName"], ("before.step", "after.step"))

    def test_box_material_and_measurements(self):
        box = self.box()
        self.save()
        box.Length = 20
        self.save(True)
        result = self.compare()
        change = self.changes(result)["Box"]
        self.assertEqual(change.properties["Length"], ("10", "20"))
        self.assertEqual(
            tuple(round(v) for v in change.measures), (1000, 2000, 600, 1000)
        )
        material = {
            v.role: v.geometry.Volume for v in result.visuals if v.name == "Box"
        }
        self.assertAlmostEqual(material["Added"], 1000)
        self.assertAlmostEqual(material["Unchanged"], 1000)
        self.assertNotIn("Removed", material)
        self.assertIn("change +1000", result.report())

    def test_shift_has_added_removed_common(self):
        box = self.box()
        self.save()
        box.Placement.Base.x = 5
        self.save(True)
        material = {
            v.role: v.geometry.Volume for v in self.compare().visuals if v.name == "Box"
        }
        self.assertEqual(set(material), {"Added", "Removed", "Unchanged"})
        for volume in material.values():
            self.assertAlmostEqual(volume, 500)

    def test_parent_placement_changes_child_geometry(self):
        part = self.doc.addObject("App::Part", "Assembly")
        box = self.box()
        part.addObject(box)
        self.save()
        part.Placement.Base.x = 5
        self.save(True)
        result = self.compare()
        self.assertIn("Box", self.changes(result))
        material = {
            v.role: v.geometry.Volume for v in result.visuals if v.name == "Box"
        }
        self.assertEqual(set(material), {"Added", "Removed", "Unchanged"})
        for volume in material.values():
            self.assertAlmostEqual(volume, 500)

    def test_partdesign_pad(self):
        import Sketcher  # noqa: F401 - registers Sketcher object types

        body = self.doc.addObject("PartDesign::Body", "Body")
        sketch = body.newObject("Sketcher::SketchObject", "Profile")
        points = [
            App.Vector(0, 0, 0),
            App.Vector(10, 0, 0),
            App.Vector(10, 10, 0),
            App.Vector(0, 10, 0),
        ]
        for i in range(4):
            sketch.addGeometry(Part.LineSegment(points[i], points[(i + 1) % 4]), False)
        pad = body.newObject("PartDesign::Pad", "Pad")
        pad.Profile = sketch
        pad.Length = 10
        self.save()
        pad.Length = 20
        self.save(True)
        result = self.compare()
        self.assertEqual({v.name for v in result.visuals}, {"Body"})
        self.assertAlmostEqual(self.changes(result)["Body"].measures[1], 2000)

    def test_primitives(self):
        for type_id in ("Part::Cylinder", "Part::Sphere", "Part::Cone", "Part::Torus"):
            self.doc.addObject(type_id, type_id.split("::")[1])
        self.save()
        self.doc.getObject("Sphere").Radius = 8
        self.doc.getObject("Cylinder").Height = 20
        self.save(True)
        changes = self.changes(self.compare())
        self.assertIn("Radius", changes["Sphere"].properties)
        self.assertIn("Height", changes["Cylinder"].properties)
        self.assertNotIn("Cone", changes)

    def test_part_boolean_result_only(self):
        box = self.box()
        tool = self.doc.addObject("Part::Cylinder", "Tool")
        for kind in ("Cut", "Fuse", "Common"):
            boolean = self.doc.addObject("Part::" + kind, kind)
            boolean.Base, boolean.Tool = box, tool
        if App.GuiUp:
            box.ViewObject.Visibility = tool.ViewObject.Visibility = False
        self.save()
        tool.Radius = 3
        self.save(True)
        result = self.compare()
        for name in ("Cut", "Fuse", "Common"):
            self.assertIn(name, self.changes(result))
        self.assertEqual({v.name for v in result.visuals}, {"Cut", "Fuse", "Common"})

    def test_partdesign_body_tip_only(self):
        body = self.doc.addObject("PartDesign::Body", "Body")
        first = body.newObject("PartDesign::Feature", "First")
        first.Shape = Part.makeBox(10, 10, 10)
        tip = body.newObject("PartDesign::Feature", "TipFeature")
        tip.Shape = Part.makeBox(20, 10, 10)
        body.Tip = tip
        self.save()
        tip.Shape = Part.makeBox(30, 10, 10)
        self.save(True)
        result = self.compare()
        self.assertEqual({v.name for v in result.visuals}, {"Body"})
        self.assertAlmostEqual(
            sum(v.geometry.Volume for v in result.visuals if v.role == "Added"), 1000
        )

    def test_body_history_does_not_multiply_booleans(self):
        body = self.doc.addObject("PartDesign::Body", "Body")
        features = []
        for i in range(20):
            feature = body.newObject("PartDesign::Feature", "Feature")
            feature.Shape = Part.makeBox(10 + i, 10, 10)
            features.append(feature)
        body.Tip = features[-1]
        self.save()
        for i, feature in enumerate(features):
            feature.Shape = Part.makeBox(20 + i, 10, 10)
        self.save(True)
        with patch.object(Diff, "_solid_parts", wraps=Diff._solid_parts) as booleans:
            result = self.compare()
        self.assertEqual(booleans.call_count, 1)
        self.assertEqual({v.name for v in result.visuals}, {"Body"})
        self.assertGreaterEqual(len(result.objects), 21)

    def test_sketch(self):
        import Sketcher  # noqa: F401 - registers Sketcher object types

        sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        sketch.addGeometry(
            Part.LineSegment(App.Vector(0, 0, 0), App.Vector(10, 0, 0)), False
        )
        self.save()
        sketch.addGeometry(
            Part.LineSegment(App.Vector(10, 0, 0), App.Vector(10, 10, 0)), False
        )
        self.save(True)
        result = self.compare()
        self.assertIn("Sketch", self.changes(result))
        self.assertEqual({v.role for v in result.visuals}, {"Added", "Removed"})
        self.assertFalse(result.warnings)

    def test_mesh_overlay(self):
        import Mesh

        mesh = self.doc.addObject("Mesh::Feature", "Mesh")
        mesh.Mesh = Mesh.Mesh(
            [(App.Vector(), App.Vector(10, 0, 0), App.Vector(0, 10, 0))]
        )
        self.save()
        mesh.Mesh = Mesh.Mesh(
            [(App.Vector(), App.Vector(20, 0, 0), App.Vector(0, 10, 0))]
        )
        self.save(True)
        result = self.compare()
        self.assertEqual({v.role for v in result.visuals}, {"Added", "Removed"})
        self.assertTrue(all(v.mesh for v in result.visuals))
        doc = Diff.show_result(result)
        self.assertEqual(sum(obj.TypeId == "Mesh::Feature" for obj in doc.Objects), 2)

    def test_draft_wire(self):
        import Draft

        wire = Draft.make_wire(
            [App.Vector(), App.Vector(10, 0, 0), App.Vector(10, 10, 0)]
        )
        self.save()
        wire.Points = [App.Vector(), App.Vector(20, 0, 0), App.Vector(20, 10, 0)]
        self.save(True)
        result = self.compare()
        self.assertIn(wire.Name, self.changes(result))
        self.assertEqual({v.role for v in result.visuals}, {"Added", "Removed"})

    def test_face_overlay(self):
        face = self.doc.addObject("Part::Feature", "Face")
        face.Shape = Part.makePlane(10, 10)
        self.save()
        face.Shape = Part.makePlane(20, 10)
        self.save(True)
        result = self.compare()
        self.assertEqual({v.role for v in result.visuals}, {"Added", "Removed"})
        self.assertAlmostEqual(self.changes(result)["Face"].measures[3], 200)

    def test_spreadsheet_values(self):
        sheet = self.doc.addObject("Spreadsheet::Sheet", "Sheet")
        sheet.set("A1", "10")
        sheet.set("B1", "=A1 * 2")
        self.save()
        sheet.set("A1", "20")
        self.save(True)
        result = self.compare()
        change = self.changes(result)["Sheet"]
        self.assertTrue(
            any("A1" in name for name in change.properties), change.properties
        )
        self.assertIn("10 -> 20", result.report())
        self.assertEqual(result.visuals, [])

    def test_link_and_part_global_placement(self):
        part = self.doc.addObject("App::Part", "Assembly")
        box = self.box()
        part.addObject(box)
        part.Placement.Base.x = 50
        link = self.doc.addObject("App::Link", "Link")
        link.setLink(box)
        link.LinkPlacement.Base.x = 100
        self.save()
        part.Placement.Base.x = 55
        box.Length = 20
        self.save(True)
        result = self.compare()
        self.assertIn("Assembly", self.changes(result))
        self.assertIn("Link", self.changes(result))
        box_added = next(
            v for v in result.visuals if v.name == "Box" and v.role == "Added"
        )
        self.assertAlmostEqual(box_added.geometry.BoundBox.XMin, 60)
        link_added = next(
            v for v in result.visuals if v.name == "Link" and v.role == "Added"
        )
        self.assertAlmostEqual(link_added.geometry.BoundBox.XMin, 110)

    def test_link_placement_only(self):
        box = self.box()
        link = self.doc.addObject("App::Link", "Link")
        link.setLink(box)
        link.LinkPlacement.Base.x = 20
        self.save()
        link.LinkPlacement.Base.x = 25
        self.save(True)
        result = self.compare()
        material = {v.role: v.geometry for v in result.visuals if v.name == "Link"}
        self.assertEqual(set(material), {"Added", "Removed", "Unchanged"})
        self.assertAlmostEqual(material["Added"].BoundBox.XMin, 30)
        self.assertAlmostEqual(material["Removed"].BoundBox.XMin, 20)
        self.assertAlmostEqual(material["Unchanged"].BoundBox.XMin, 25)
        for geometry in material.values():
            self.assertAlmostEqual(geometry.Volume, 500)

    def _external_pair(self, label="After external link"):
        external = App.newDocument("ModelDiffExternal")
        box = external.addObject("Part::Box", "ExternalBox")
        recompute_document(external)
        path = str(Path(self.tmp.name) / "external.FCStd")
        save_document_as(external, path)
        self.save()
        link = self.doc.addObject("App::Link", "ExternalLink")
        link.setLink(box)
        self.save()
        link.Label = label
        self.save(True)
        return external, box, link, path

    def test_available_external_link(self):
        external, _box, link, _ = self._external_pair()
        close_document(external)
        before = link.LinkedObject
        result = self.compare()
        self.assertEqual(link.LinkedObject, before)
        self.assertEqual({v.name for v in result.visuals}, {"ExternalLink"})
        self.assertAlmostEqual(result.visuals[0].geometry.Volume, 1000)
        for volume in self.changes(result)["ExternalLink"].measures[:2]:
            self.assertAlmostEqual(volume, 1000)
        self.assertFalse(result.warnings)

    def test_open_edited_external_file_untouched(self):
        external, box, link, _ = self._external_pair()
        box.Length = 99
        recompute_document(external)
        active = App.ActiveDocument.Name
        result = self.compare()
        self.assertEqual(App.ActiveDocument.Name, active)
        self.assertEqual(box.Length.Value, 99)
        self.assertAlmostEqual(box.Shape.Volume, 9900)
        self.assertEqual(link.LinkedObject, box)
        self.assertAlmostEqual(result.visuals[0].geometry.Volume, 1000)

    def test_absolute_external_link(self):
        external, _box, _link, path = self._external_pair()
        close_document(external)
        for filename in (self.local, self.remote):
            with zipfile.ZipFile(filename) as archive:
                members = [(info, archive.read(info)) for info in archive.infolist()]
            with zipfile.ZipFile(filename, "w") as archive:
                for info, content in members:
                    if info.filename == "Document.xml":
                        root = ET.fromstring(content)
                        references = [
                            n
                            for n in root.iter()
                            if n.tag.startswith("XLink") and n.get("file")
                        ]
                        self.assertTrue(references)
                        for node in references:
                            node.set("file", path)
                        content = ET.tostring(
                            root, encoding="utf-8", xml_declaration=True
                        )
                    archive.writestr(info, content)
        result = self.compare()
        self.assertAlmostEqual(result.visuals[0].geometry.Volume, 1000)
        self.assertFalse(result.warnings)

    def test_missing_external_link(self):
        external, _box, _link, path = self._external_pair(
            "Missing dependency still compared"
        )
        close_document(external)
        os.unlink(path)
        result = self.compare()
        self.assertIn("ExternalLink", self.changes(result))
        self.assertIn("Missing dependency still compared", result.report())
        self.assertIn("Missing external link", result.report())

    def test_boolean_failure_falls_back(self):
        box = self.box()
        self.save()
        box.Length = 20
        self.save(True)
        with patch.object(
            Diff, "_solid_parts", side_effect=RuntimeError("test kernel failure")
        ):
            result = self.compare()
        self.assertEqual({v.role for v in result.visuals}, {"Added", "Removed"})
        self.assertIn("Boolean failed; using two-colour overlay", result.report())
        self.assertIn("test kernel failure", result.report())

    def test_boolean_failure_continues_other_objects(self):
        first, second = self.box("First"), self.box("Second")
        self.save()
        first.Length = second.Length = 20
        self.save(True)
        original = Diff._solid_parts
        calls = 0

        def fail_first(old, new):
            nonlocal calls
            calls += 1
            if calls == 1:
                raise RuntimeError("first Boolean fails")
            return original(old, new)

        with patch.object(Diff, "_solid_parts", side_effect=fail_first):
            result = self.compare()
        self.assertEqual(calls, 2)
        self.assertEqual(len(result.warnings), 1)
        self.assertEqual(
            {v.role for v in result.visuals if v.name == "Second"},
            {"Added", "Unchanged"},
        )
        shown = Diff.show_result(result)
        self.assertEqual(len(shown.Changed.Group), 4)

    def test_invalid_boolean_result_falls_back(self):
        class InvalidShape:
            def isNull(self):
                return False

            def isValid(self):
                return False

        class InvalidBoolean:
            def cut(self, other):
                return InvalidShape()

            def common(self, other):
                return InvalidShape()

        with self.assertRaises(ValueError):
            Diff._solid_parts(InvalidBoolean(), InvalidBoolean())

    def test_diff_document_contents_detached(self):
        box = self.box()
        self.save()
        box.Placement.Base.x = 5
        self.save(True)
        result = self.compare()
        close_document(self.doc)
        os.unlink(self.local)
        os.unlink(self.remote)
        shown = Diff.show_result(result)
        self.assertEqual(shown.FileName, "")
        for name in ("Added", "Removed", "Changed", "Unchanged"):
            self.assertIsNotNone(shown.getObject(name))
        self.assertEqual(len(shown.Changed.Group), 3)
        self.assertEqual(shown.DiffReport, result.report())
        for obj in shown.Changed.Group:
            self.assertEqual(obj.SourceName, "Box")
            self.assertEqual(obj.TypeId, "Part::Feature")
            self.assertAlmostEqual(obj.Shape.Volume, 500)

    def test_open_edited_input_untouched(self):
        box = self.box()
        self.save()
        shutil.copyfile(self.local, self.remote)
        box.Length = 99
        self.assertTrue(self.compare().identical)
        self.assertEqual(box.Length.Value, 99)

    def test_bad_input_cleans_snapshots(self):
        self.box()
        self.save()
        before = set(App.listDocuments())
        with self.assertRaises((OSError, zipfile.BadZipFile)):
            Diff.compare_files(self.local, self.remote)
        self.assertEqual(set(App.listDocuments()), before)

    def test_native_restore_failure_cleanup(self):
        self.box()
        self.save()
        shutil.copyfile(self.local, self.remote)
        original = Diff._restore
        count = 0

        def fail_second(path):
            nonlocal count
            count += 1
            if count == 2:
                raise OSError("cannot restore")
            return original(path)

        before = set(App.listDocuments())
        with patch.object(Diff, "_restore", side_effect=fail_second), self.assertRaises(
            OSError
        ):
            Diff.compare_files(self.local, self.remote)
        self.assertEqual(set(App.listDocuments()), before)

    def test_cli_headless(self):
        box = self.box()
        self.save()
        box.Length = 20
        self.save(True)
        expected = self.compare().report()
        binary = str(Path(App.ConfigGet("BinPath")) / "FreeCADCmd")
        for options in ([], ["--set-config", "RunMode=Exit", "ScriptFileName=Ignored"]):
            with self.subTest(options=options):
                process = subprocess.run(
                    check=False,
                    args=[binary, "--diff", self.local, self.remote, *options],
                    text=True,
                    capture_output=True,
                    timeout=180,
                    env={**os.environ, "QT_QPA_PLATFORM": "offscreen"},
                )
                self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
                self.assertIn(expected, process.stdout)

    def test_cli_requires_two_inputs(self):
        binary = str(Path(App.ConfigGet("BinPath")) / "FreeCADCmd")
        for args in ([], ["one.FCStd"], ["one", "two", "three"]):
            process = subprocess.run(
                check=False,
                args=[binary, "--diff", *args],
                text=True,
                capture_output=True,
                timeout=30,
            )
            self.assertNotEqual(process.returncode, 0)
            self.assertIn("Exactly two files", process.stdout + process.stderr)

    def test_cli_missing_input_fails(self):
        self.box()
        self.save()
        binary = str(Path(App.ConfigGet("BinPath")) / "FreeCADCmd")
        process = subprocess.run(
            check=False,
            args=[binary, "--diff", self.local, self.remote],
            text=True,
            capture_output=True,
            timeout=180,
            env={**os.environ, "QT_QPA_PLATFORM": "offscreen"},
        )
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("remote model.FCStd", process.stdout + process.stderr)

    def test_cli_rejects_test_combination(self):
        binary = str(Path(App.ConfigGet("BinPath")) / "FreeCADCmd")
        process = subprocess.run(
            check=False,
            args=[binary, "--diff", "a.FCStd", "b.FCStd", "-t", "TestModelDiff"],
            text=True,
            capture_output=True,
            timeout=30,
        )
        self.assertNotEqual(process.returncode, 0)
        self.assertIn("cannot be combined", process.stdout + process.stderr)
