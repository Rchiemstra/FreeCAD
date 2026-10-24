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
                         "signature": "output"}],
            "parameters": {"distance": -2.0, "tolerance": 0.005, "closed": False},
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
