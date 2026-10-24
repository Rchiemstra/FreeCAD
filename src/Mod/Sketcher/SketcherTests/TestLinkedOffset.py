# SPDX-License-Identifier: LGPL-2.1-or-later

"""Persistence and explicit operation tests seed version-1 metadata through XML."""

import json
import os
import tempfile
import unittest
import uuid
import zipfile
import xml.etree.ElementTree as ET

import FreeCAD
import Part
import Sketcher


class TestLinkedOffset(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.doc = FreeCAD.newDocument("LinkedOffsetPersistence")
        self.sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        for y in (0, 2):
            self.sketch.addGeometry(Part.LineSegment(
                FreeCAD.Vector(0, y, 0), FreeCAD.Vector(10, y, 0)))
        self.doc.recompute()
        path = os.path.join(self.directory.name, "Seed.FCStd")
        self.doc.saveAs(path)
        FreeCAD.closeDocument(self.doc.Name)
        relation_id, source_id, output_id = (str(uuid.uuid4()) for _ in range(3))
        relation = {
            "id": relation_id,
            "sources": [{"uuid": source_id, "type": "Part::GeomLineSegment",
                         "reversed": True, "seam": 0.25, "signature": "source"}],
            "outputs": [{"uuid": output_id, "type": "Part::GeomLineSegment",
                         "reversed": False, "seam": 0, "signature": "output"}],
            "parameters": {
                "generator": "BSpline", "distance": -2.0, "tolerance": 0.005,
                "normal": [0, 0, 1], "closed": False, "orientation": 0, "seam": 0.25,
                "join": "G1", "joinTolerance": 1e-7, "maxWork": 10000000,
                "maxDepth": 24, "maxInputPoles": 128, "maxOutputPoles": 256,
                "maxSegments": 64, "maxDegree": 8, "maxValidationSpans": 4096,
                "maxRefinementDepth": 20,
            },
        }
        with zipfile.ZipFile(path) as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        root = ET.fromstring(entries["Document.xml"])
        geometry = root.findall(".//Property[@name='Geometry']/GeometryList/Geometry")
        self.assertEqual(len(geometry), 2)
        for index, identity in enumerate((source_id, output_id)):
            extensions = geometry[index].find("GeoExtensions")
            self.assertIsNotNone(extensions)
            ET.SubElement(extensions, "GeoExtension", {
                "type": "Sketcher::LinkedOffsetExtension", "uuid": identity,
                "version": "1", "membership": relation_id if index else "",
                "record": json.dumps(relation) if index else "",
            })
            extensions.set("count", str(len(extensions)))
        entries["Document.xml"] = ET.tostring(root, encoding="utf-8", xml_declaration=True)
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in entries.items():
                archive.writestr(name, data)
        self.doc = FreeCAD.openDocument(path)
        self.sketch = self.doc.getObject("Sketch")

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)
        self.directory.cleanup()

    def test_parameters_readable(self):
        relation, = Sketcher.listLinkedOffsets(self.sketch)
        self.assertEqual(relation["status"], "Ok")
        self.assertEqual(relation["parameters"]["distance"], -2.0)
        self.assertEqual(relation["parameters"]["tolerance"], 0.005)
        self.assertTrue(relation["sources"][0]["reversed"])
        self.assertEqual(relation["sources"][0]["seam"], 0.25)
        # Changing the returned snapshot must not change persistent parameters.
        relation["parameters"]["distance"] = 100
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch)[0]
                         ["parameters"]["distance"], -2.0)

    def test_save_reopen(self):
        before = Sketcher.listLinkedOffsets(self.sketch)
        path = os.path.join(self.directory.name, "RoundTrip.FCStd")
        self.doc.saveAs(path)
        FreeCAD.closeDocument(self.doc.Name)
        self.doc = FreeCAD.openDocument(path)
        self.sketch = self.doc.getObject("Sketch")
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch), before)
        self.assertEqual(before[0]["status"], "Ok")

    def test_whole_sketch_copy_resolves_independently(self):
        copied = self.doc.copyObject(self.sketch, False)
        self.assertEqual(Sketcher.listLinkedOffsets(copied),
                         Sketcher.listLinkedOffsets(self.sketch))
        self.sketch.delGeometry(0)
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch)[0]
                         ["status"], "MissingSource")
        self.assertEqual(Sketcher.listLinkedOffsets(copied)[0]["status"], "Ok")
        copied.delGeometry(0)
        self.assertEqual(Sketcher.listLinkedOffsets(copied)[0]["status"], "MissingSource")

    def rewrite_document(self, mutate):
        path = os.path.join(self.directory.name, "Mutated.FCStd")
        self.doc.saveAs(path)
        FreeCAD.closeDocument(self.doc.Name)
        with zipfile.ZipFile(path) as archive:
            entries = {name: archive.read(name) for name in archive.namelist()}
        root = ET.fromstring(entries["Document.xml"])
        mutate(root)
        entries["Document.xml"] = ET.tostring(root, encoding="utf-8", xml_declaration=True)
        with zipfile.ZipFile(path, "w") as archive:
            for name, data in entries.items():
                archive.writestr(name, data)
        self.doc = FreeCAD.openDocument(path)
        self.sketch = self.doc.getObject("Sketch")

    def test_opaque_document_versions(self):
        for version in ("2", "x", "2147483648"):
            with self.subTest(version=version):
                def mutate(root):
                    for ext in root.findall(".//GeoExtension"):
                        if ext.get("type") == "Sketcher::LinkedOffsetExtension":
                            ext.set("version", version)
                            if ext.get("record"):
                                ext.set("record", "opaque <future> & invalid JSON")
                self.rewrite_document(mutate)
                self.assertEqual(self.sketch.GeometryCount, 2)
                self.assertEqual(self.sketch.Geometry[1].EndPoint, FreeCAD.Vector(10, 2, 0))
                self.assertEqual(Sketcher.listLinkedOffsets(self.sketch), [])
                def inspect(root):
                    linked = [e for e in root.findall(".//GeoExtension")
                              if e.get("type") == "Sketcher::LinkedOffsetExtension"]
                    self.assertEqual(len(linked), 2)
                    self.assertTrue(all(e.get("version") == version for e in linked))
                    self.assertEqual([e.get("record") for e in linked if e.get("record")],
                                     ["opaque <future> & invalid JSON"])
                self.rewrite_document(inspect)

    def test_copy_delete_never_retargets(self):
        self.sketch.addCopy([0], FreeCAD.Vector(100, 0, 0))
        self.sketch.delGeometry(0)
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch)[0]["status"], "MissingSource")

    def test_complete_copy_remaps_group(self):
        original, = Sketcher.listLinkedOffsets(self.sketch)
        self.sketch.addCopy([0, 1], FreeCAD.Vector(100, 0, 0))
        relations = Sketcher.listLinkedOffsets(self.sketch)
        self.assertEqual(len(relations), 2)
        self.assertTrue(all(r["status"] == "Ok" for r in relations))
        copied, = [r for r in relations if r["id"] != original["id"]]
        self.assertEqual(copied["sourceIndices"], [2])
        self.assertEqual(copied["outputIndices"], [3])
        self.assertNotEqual(copied["sources"][0]["uuid"], original["sources"][0]["uuid"])
        self.assertNotEqual(copied["outputs"][0]["uuid"], original["outputs"][0]["uuid"])
        self.rewrite_document(lambda root: None)
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch), relations)
        self.sketch.delGeometry(0)
        self.assertEqual(next(r for r in Sketcher.listLinkedOffsets(self.sketch)
                              if r["id"] == copied["id"])["status"], "Ok")

    def test_partial_output_copy_detaches(self):
        original = Sketcher.listLinkedOffsets(self.sketch)
        self.sketch.addCopy([1], FreeCAD.Vector(100, 0, 0))
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch), original)
        self.sketch.delGeometry(1)
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch), [])

    def test_conversion_cannot_resolve_ok(self):
        self.sketch.convertToNURBS(0)
        self.assertNotEqual(Sketcher.listLinkedOffsets(self.sketch)[0]["status"], "Ok")

    def test_join_cannot_resolve_ok(self):
        self.sketch.addGeometry(Part.LineSegment(
            FreeCAD.Vector(10, 0, 0), FreeCAD.Vector(20, 0, 0)))
        self.sketch.join(0, 2, 2, 1, 0)
        self.assertNotEqual(Sketcher.listLinkedOffsets(self.sketch)[0]["status"], "Ok")

    def test_python_arguments(self):
        for args in ((), (self.sketch, self.sketch), (None,), (1,), (self.doc,)):
            with self.subTest(args=args), self.assertRaises(TypeError):
                Sketcher.listLinkedOffsets(*args)
        deleted = self.doc.addObject("Sketcher::SketchObject", "Deleted")
        self.doc.removeObject(deleted.Name)
        with self.assertRaises(RuntimeError):
            Sketcher.listLinkedOffsets(deleted)

    def test_malformed_document_records_are_unavailable(self):
        valid, = Sketcher.listLinkedOffsets(self.sketch)
        record = {key: valid[key] for key in ("id", "sources", "outputs", "parameters")}
        cases = []
        for field in record:
            bad = json.loads(json.dumps(record))
            del bad[field]
            cases.append(bad)
        for field in record["parameters"]:
            bad = json.loads(json.dumps(record))
            del bad["parameters"][field]
            cases.append(bad)
        for field, value in (("distance", 0), ("tolerance", -1), ("normal", [0, 0, 0]),
                             ("maxWork", 1.5), ("closed", "false")):
            bad = json.loads(json.dumps(record))
            bad["parameters"][field] = value
            cases.append(bad)
        cases.extend(([], "broken", {"id": "broken"}))
        for bad in cases:
            with self.subTest(record=bad):
                def mutate(root):
                    for ext in root.findall(".//GeoExtension"):
                        if (ext.get("type") == "Sketcher::LinkedOffsetExtension"
                                and ext.get("record")):
                            ext.set("record", json.dumps(bad))
                self.rewrite_document(mutate)
                self.assertEqual(self.sketch.GeometryCount, 2)
                relation, = Sketcher.listLinkedOffsets(self.sketch)
                self.assertEqual(relation["status"], "InvalidRecord")
                self.assertTrue(relation["reason"])

    def test_unknown_extension_preserves_document_geometry(self):
        def mutate(root):
            for ext in root.findall(".//GeoExtension"):
                if ext.get("type") == "Sketcher::LinkedOffsetExtension":
                    ext.set("type", "Future::UnknownExtension")
        self.rewrite_document(mutate)
        self.assertEqual(self.sketch.GeometryCount, 2)
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch), [])

    def test_copy_undo_redo_preserves_original_ids(self):
        self.doc.UndoMode = 1
        before = Sketcher.listLinkedOffsets(self.sketch)
        self.doc.openTransaction("Copy linked group")
        self.sketch.addCopy([0, 1], FreeCAD.Vector(100, 0, 0))
        self.doc.commitTransaction()
        after = Sketcher.listLinkedOffsets(self.sketch)
        self.assertEqual(len(after), 2)
        self.doc.undo()
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch), before)
        self.doc.redo()
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch), after)

    def test_clone_copy_remaps(self):
        self.sketch.addCopy([0, 1], FreeCAD.Vector(100, 0, 0), True)
        relations = Sketcher.listLinkedOffsets(self.sketch)
        self.assertEqual(len(relations), 2)
        self.assertTrue(all(r["status"] == "Ok" for r in relations))

    def test_symmetry_copy_remaps(self):
        self.sketch.addSymmetric([0, 1], -1)
        relations = Sketcher.listLinkedOffsets(self.sketch)
        self.assertEqual(len(relations), 2)
        self.assertTrue(all(r["status"] == "Ok" for r in relations))

    def test_geometry_insertion_remaps(self):
        self.sketch.addGeometry(self.sketch.Geometry)
        relations = Sketcher.listLinkedOffsets(self.sketch)
        self.assertEqual(len(relations), 2)
        self.assertTrue(all(r["status"] == "Ok" for r in relations))

    def test_carbon_copy_remaps(self):
        target = self.doc.addObject("Sketcher::SketchObject", "Carbon")
        target.carbonCopy(self.sketch.Name, False)
        original, = Sketcher.listLinkedOffsets(self.sketch)
        copied, = Sketcher.listLinkedOffsets(target)
        self.assertEqual(copied["status"], "Ok")
        self.assertNotEqual(copied["id"], original["id"])
        self.assertNotEqual(copied["sources"][0]["uuid"], original["sources"][0]["uuid"])

    def test_split_invalidates_ancestry(self):
        self.sketch.split(0, FreeCAD.Vector(5, 0, 0))
        self.assertNotEqual(Sketcher.listLinkedOffsets(self.sketch)[0]["status"], "Ok")

    def test_extend_invalidates_ancestry(self):
        self.sketch.extend(0, 2, 2)
        self.assertNotEqual(Sketcher.listLinkedOffsets(self.sketch)[0]["status"], "Ok")

    def test_trim_invalidates_ancestry(self):
        self.sketch.addGeometry(Part.LineSegment(
            FreeCAD.Vector(5, -5, 0), FreeCAD.Vector(5, 5, 0)))
        self.sketch.trim(0, FreeCAD.Vector(2, 0, 0))
        self.assertNotEqual(Sketcher.listLinkedOffsets(self.sketch)[0]["status"], "Ok")

    def test_insert_delete_never_retargets(self):
        self.sketch.addGeometry(self.sketch.Geometry[0])
        self.sketch.delGeometry(0)
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch)[0]["status"], "MissingSource")


    def test_explicit_operation_api(self):
        for name in ("refreshLinkedOffsets", "editLinkedOffsets", "detachLinkedOffsets"):
            with self.subTest(name=name):
                self.assertTrue(callable(getattr(Sketcher, name, None)), name)

    def test_refresh_missing_source_preserves_redo(self):
        self.sketch.delGeometry(0)
        self.doc.openTransaction("sentinel")
        self.doc.addObject("App::FeaturePython", "Sentinel")
        self.doc.commitTransaction()
        self.doc.undo()
        before = (Sketcher.listLinkedOffsets(self.sketch), self.doc.UndoCount,
                  self.doc.RedoCount, self.sketch.Geometry[0].toShape().exportBrepToString())
        with self.assertRaises(ValueError):
            Sketcher.refreshLinkedOffsets(self.sketch)
        self.assertEqual(before, (Sketcher.listLinkedOffsets(self.sketch), self.doc.UndoCount,
                                  self.doc.RedoCount,
                                  self.sketch.Geometry[0].toShape().exportBrepToString()))

    def test_detach_legacy_preserves_unowned_constraints(self):
        self.sketch.addConstraint(Sketcher.Constraint("Block", 1))
        before = self.doc.UndoCount
        Sketcher.detachLinkedOffsets(self.sketch)
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch), [])
        # Version-1 records have no proof that an existing Block belongs to them.
        self.assertEqual(self.sketch.ConstraintCount, 1)
        self.assertEqual(self.doc.UndoCount, before + 1)
        self.doc.undo()
        self.assertEqual(len(Sketcher.listLinkedOffsets(self.sketch)), 1)
        self.doc.redo()
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch), [])


class TestLinkedOffsetOperations(unittest.TestCase):
    """Authentic line snapshots with explicit ownership, without requiring a GUI."""

    rewrite_document = TestLinkedOffset.rewrite_document
    tearDown = TestLinkedOffset.tearDown

    def setUp(self):
        TestLinkedOffset.setUp(self)
        self.sketch.addConstraint(Sketcher.Constraint("Block", 1))
        facades = self.sketch.GeometryFacadeList
        facades[1].Blocked = True
        self.sketch.GeometryFacadeList = facades

        def native(y):
            return ('<GeoExtensions count="0">\n</GeoExtensions>\n'
                    '<LineSegment StartX="0" StartY="%s" StartZ="0" '
                    'EndX="10" EndY="%s" EndZ="0"/>\n') % (y, y)

        def seed(root):
            for ext in root.findall(".//GeoExtension"):
                if ext.get("type") == "Sketcher::LinkedOffsetExtension" and ext.get("record"):
                    record = json.loads(ext.get("record"))
                    record["sources"][0].update(reversed=False, seam=0, signature=native(0))
                    record["outputs"][0]["signature"] = native(2)
                    record["parameters"].update(generator="Chain", seam=0)
                    record["ownedBlocks"] = ["test-owned-block"]
                    record["opaque"] = {"future": [1, "keep"]}
                    record["parameters"]["opaque"] = "keep"
                    record["outputs"][0]["opaque"] = "keep"
                    ext.set("record", json.dumps(record))
            block, = root.findall(".//Property[@name='Constraints']//Constrain")
            block.set("MetaData", json.dumps({"linkedOffsetBlock": "test-owned-block"}))
        self.rewrite_document(seed)
        self.doc.UndoMode = 1

    def state(self):
        return (tuple(g.toShape().exportBrepToString() for g in self.sketch.Geometry),
                tuple((str(c), c.Value) for c in self.sketch.Constraints),
                Sketcher.listLinkedOffsets(self.sketch), self.doc.UndoCount,
                self.doc.RedoCount, tuple(self.doc.UndoNames), tuple(self.doc.RedoNames),
                self.doc.getFileChangeState(), self.doc.isTouched(), tuple(self.sketch.State))

    def translate(self, index, vector):
        geometry = self.sketch.Geometry
        geometry[index].translate(vector)
        self.sketch.Geometry = geometry

    def test_refresh_signed_sides_and_stable_slots(self):
        relation, = Sketcher.listLinkedOffsets(self.sketch)
        self.translate(0, FreeCAD.Vector(3, 0, 0))
        count = self.doc.UndoCount
        Sketcher.refreshLinkedOffsets(self.sketch)
        after, = Sketcher.listLinkedOffsets(self.sketch)
        self.assertEqual(after["outputs"][0]["uuid"], relation["outputs"][0]["uuid"])
        self.assertEqual(after["outputIndices"], [1])
        self.assertEqual(self.sketch.Geometry[1].StartPoint, FreeCAD.Vector(3, 2, 0))
        self.assertFalse(after["stale"])
        self.assertEqual(self.doc.UndoCount, count + 1)
        Sketcher.editLinkedOffsets(self.sketch, None, 2, 0.01)
        self.assertEqual(self.sketch.Geometry[1].StartPoint, FreeCAD.Vector(3, -2, 0))
        self.doc.undo()
        self.assertEqual(self.sketch.Geometry[1].StartPoint, FreeCAD.Vector(3, 2, 0))
        self.doc.redo()
        self.assertEqual(self.sketch.Geometry[1].StartPoint, FreeCAD.Vector(3, -2, 0))

    def test_refresh_reference_dimension_and_opaque_roundtrip(self):
        ref = self.sketch.addConstraint(Sketcher.Constraint("DistanceX", 1, 1, 0.0))
        self.sketch.setDriving(ref, False)
        self.translate(0, FreeCAD.Vector(3, 0, 0))
        Sketcher.refreshLinkedOffsets(self.sketch)
        self.assertEqual(self.sketch.Constraints[ref].Value, 3)
        def inspect(root):
            for ext in root.findall(".//GeoExtension"):
                if ext.get("type") == "Sketcher::LinkedOffsetExtension" and ext.get("record"):
                    record = json.loads(ext.get("record"))
                    self.assertEqual(record["opaque"], {"future": [1, "keep"]})
                    self.assertEqual(record["parameters"]["opaque"], "keep")
                    self.assertEqual(record["outputs"][0]["opaque"], "keep")
        self.rewrite_document(inspect)
        Sketcher.editLinkedOffsets(self.sketch, None, -3, .01)
        self.assertEqual(self.sketch.Geometry[1].StartPoint, FreeCAD.Vector(3, 3, 0))

    def test_refresh_all_failure_preserves_redo(self):
        self.sketch.addCopy([0, 1], FreeCAD.Vector(100, 0, 0))
        self.translate(0, FreeCAD.Vector(3, 0, 0))
        self.sketch.delGeometry(2)
        self.doc.openTransaction("Sentinel")
        self.doc.addObject("App::FeaturePython", "Sentinel")
        self.doc.commitTransaction()
        self.doc.undo()
        before = self.state()
        with self.assertRaises(ValueError):
            Sketcher.refreshLinkedOffsets(self.sketch)
        self.assertEqual(self.state(), before)
        self.doc.redo()
        self.assertIsNotNone(self.doc.getObject("Sentinel"))

    def test_unblocking_and_driving_constraints_require_detach(self):
        self.sketch.delConstraint(0)
        before = self.state()
        with self.assertRaisesRegex(ValueError, "Detach"):
            Sketcher.refreshLinkedOffsets(self.sketch)
        self.assertEqual(before, self.state())
        self.sketch.addConstraint(Sketcher.Constraint("Horizontal", 1))
        before = self.state()
        with self.assertRaisesRegex(ValueError, "Detach"):
            Sketcher.refreshLinkedOffsets(self.sketch)
        self.assertEqual(before, self.state())
        Sketcher.detachLinkedOffsets(self.sketch)
        self.assertEqual(self.sketch.ConstraintCount, 1)
        self.assertEqual(self.sketch.Constraints[0].Type, "Horizontal")

    def test_detach_preserves_other_relation_and_user_dimensions(self):
        self.sketch.addCopy([0, 1], FreeCAD.Vector(100, 0, 0))
        ref = self.sketch.addConstraint(Sketcher.Constraint("DistanceX", 1, 1, 0.0))
        self.sketch.setDriving(ref, False)
        original = next(r for r in Sketcher.listLinkedOffsets(self.sketch)
                        if r["outputIndices"] == [1])
        constraints = self.sketch.ConstraintCount
        Sketcher.detachLinkedOffsets(self.sketch, [original["id"]])
        self.assertEqual(len(Sketcher.listLinkedOffsets(self.sketch)), 1)
        self.assertEqual(self.sketch.ConstraintCount, constraints - 1)
        self.assertTrue(any(not c.Driving for c in self.sketch.Constraints))
        Sketcher.refreshLinkedOffsets(self.sketch)

    def test_invalid_parameters_and_arguments_are_atomic(self):
        for distance, tolerance in ((0, .01), (1, -1), (float("inf"), .01),
                                    (1, float("nan")), (1, 1)):
            before = self.state()
            with self.assertRaises(ValueError):
                Sketcher.editLinkedOffsets(self.sketch, None, distance, tolerance)
            self.assertEqual(before, self.state())
        for name in ("refreshLinkedOffsets", "detachLinkedOffsets"):
            operation = getattr(Sketcher, name)
            relation = Sketcher.listLinkedOffsets(self.sketch)[0]["id"]
            for selection in ([], ["missing"], ["a", "a"], [relation + "\0suffix"]):
                before = self.state()
                with self.assertRaises(ValueError):
                    operation(self.sketch, selection)
                self.assertEqual(before, self.state())
            with self.assertRaises(TypeError):
                operation(None)
            with self.assertRaises(TypeError):
                operation(self.sketch, "a")

    def test_split_type_changed_and_ambiguous_sources_refuse(self):
        def change(root):
            for ext in root.findall(".//GeoExtension"):
                if ext.get("type") == "Sketcher::LinkedOffsetExtension" and ext.get("record"):
                    record = json.loads(ext.get("record"))
                    record["sources"][0]["type"] = "Part::GeomCircle"
                    ext.set("record", json.dumps(record))
        self.rewrite_document(change)
        before = self.state()
        with self.assertRaises(ValueError):
            Sketcher.refreshLinkedOffsets(self.sketch)
        self.assertEqual(before, self.state())
        Sketcher.detachLinkedOffsets(self.sketch)
        self.assertEqual(self.sketch.GeometryCount, 2)

    def test_independent_complete_copy_refresh(self):
        copied = self.doc.copyObject(self.sketch, False)
        self.translate(0, FreeCAD.Vector(3, 0, 0))
        Sketcher.refreshLinkedOffsets(copied)
        self.assertEqual(copied.Geometry[1].StartPoint, FreeCAD.Vector(0, 2, 0))
        Sketcher.refreshLinkedOffsets(self.sketch)
        self.assertEqual(self.sketch.Geometry[1].StartPoint, FreeCAD.Vector(3, 2, 0))

    def test_split_source_retains_last_valid_output(self):
        self.sketch.split(0, FreeCAD.Vector(5, 0, 0))
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch)[0]["status"], "TopologyChanged")
        before = self.state()
        with self.assertRaises(ValueError):
            Sketcher.refreshLinkedOffsets(self.sketch)
        self.assertEqual(before, self.state())
        Sketcher.detachLinkedOffsets(self.sketch)
        self.assertEqual(Sketcher.listLinkedOffsets(self.sketch), [])

    def test_duplicate_uuid_refuses_without_mutation(self):
        def duplicate(root):
            linked = [e for e in root.findall(".//GeoExtension")
                      if e.get("type") == "Sketcher::LinkedOffsetExtension"]
            linked[0].set("uuid", linked[1].get("uuid"))
        self.rewrite_document(duplicate)
        before = self.state()
        with self.assertRaises(ValueError):
            Sketcher.refreshLinkedOffsets(self.sketch)
        self.assertEqual(before, self.state())

    def test_linked_source_refuses_feedback(self):
        relation, = Sketcher.listLinkedOffsets(self.sketch)
        def linked(root):
            for ext in root.findall(".//GeoExtension"):
                if (ext.get("type") == "Sketcher::LinkedOffsetExtension"
                        and not ext.get("record")):
                    ext.set("membership", "another-relation")
        self.rewrite_document(linked)
        before = self.state()
        with self.assertRaisesRegex(ValueError, "Linked sources"):
            Sketcher.refreshLinkedOffsets(self.sketch, [relation["id"]])
        self.assertEqual(before, self.state())

    def test_source_reversal_is_preserved(self):
        def reverse(root):
            for ext in root.findall(".//GeoExtension"):
                if ext.get("type") == "Sketcher::LinkedOffsetExtension" and ext.get("record"):
                    record = json.loads(ext.get("record"))
                    record["sources"][0].update(reversed=True, seam=10)
                    record["parameters"]["seam"] = 10
                    ext.set("record", json.dumps(record))
        self.rewrite_document(reverse)
        Sketcher.refreshLinkedOffsets(self.sketch)
        self.assertEqual(self.sketch.Geometry[1].StartPoint, FreeCAD.Vector(10, -2, 0))
        after, = Sketcher.listLinkedOffsets(self.sketch)
        self.assertTrue(after["sources"][0]["reversed"])
        self.assertEqual(after["sources"][0]["seam"], 10)

    def add_extra_output(self):
        self.sketch.addGeometry(Part.LineSegment(FreeCAD.Vector(0, 4, 0),
                                                FreeCAD.Vector(10, 4, 0)))
        self.sketch.addConstraint(Sketcher.Constraint("Block", 2))
        facades = self.sketch.GeometryFacadeList
        facades[2].Blocked = True
        self.sketch.GeometryFacadeList = facades
        def extra_output(root):
            geometry = root.findall(".//Property[@name='Geometry']/GeometryList/Geometry")
            owner = next(e for e in geometry[1].findall(".//GeoExtension")
                         if e.get("type") == "Sketcher::LinkedOffsetExtension")
            record = json.loads(owner.get("record"))
            output = dict(record["outputs"][0])
            output["uuid"] = str(uuid.uuid4())
            output["signature"] = output["signature"].replace('Y="2"', 'Y="4"')
            record["outputs"].append(output)
            record["ownedBlocks"].append("second-owned-block")
            owner.set("record", json.dumps(record))
            extensions = geometry[2].find("GeoExtensions")
            ET.SubElement(extensions, "GeoExtension", {
                "type": "Sketcher::LinkedOffsetExtension", "uuid": output["uuid"],
                "membership": record["id"], "version": "1", "record": "",
                "blockToken": "second-owned-block"})
            extensions.set("count", str(len(extensions)))
            constraints = root.findall(".//Property[@name='Constraints']//Constrain")
            constraints[1].set("MetaData", json.dumps({"linkedOffsetBlock": "second-owned-block"}))
        self.rewrite_document(extra_output)

    def test_changed_output_cardinality_is_refused(self):
        self.add_extra_output()
        before = self.state()
        with self.assertRaisesRegex(ValueError, "cardinality"):
            Sketcher.refreshLinkedOffsets(self.sketch)
        self.assertEqual(before, self.state())


    def test_detach_with_missing_designated_output_removes_owned_blocks(self):
        self.add_extra_output()
        self.sketch.delGeometry(1)
        self.assertEqual(len(Sketcher.listLinkedOffsets(self.sketch)), 1)
        Sketcher.detachLinkedOffsets(self.sketch)
        self.assertEqual(self.sketch.ConstraintCount, 0)
        self.assertEqual(self.sketch.Geometry[1].StartPoint, FreeCAD.Vector(0, 4, 0))

    def test_partial_copy_preserves_opaque_payload(self):
        self.sketch.addCopy([1], FreeCAD.Vector(100, 0, 0))
        def inspect(root):
            geometry = root.findall(".//Property[@name='Geometry']/GeometryList/Geometry")
            ext = next(e for e in geometry[2].findall(".//GeoExtension")
                       if e.get("type") == "Sketcher::LinkedOffsetExtension")
            self.assertEqual(ext.get("membership"), "")
            self.assertEqual(json.loads(ext.get("record"))["opaque"],
                             {"future": [1, "keep"]})
        self.rewrite_document(inspect)

    def test_stored_generator_budget_is_honored(self):
        def budget(root):
            for ext in root.findall(".//GeoExtension"):
                if ext.get("type") == "Sketcher::LinkedOffsetExtension" and ext.get("record"):
                    record = json.loads(ext.get("record"))
                    record["parameters"]["maxWork"] = 1
                    ext.set("record", json.dumps(record))
        self.rewrite_document(budget)
        before = self.state()
        with self.assertRaises(ValueError):
            Sketcher.refreshLinkedOffsets(self.sketch)
        self.assertEqual(before, self.state())
