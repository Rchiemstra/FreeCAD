# SPDX-License-Identifier: LGPL-2.1-or-later

"""Persistence tests seed metadata through XML; the public API is inspection only."""

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
