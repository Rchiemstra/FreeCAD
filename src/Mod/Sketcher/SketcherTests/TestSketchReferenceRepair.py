# SPDX-License-Identifier: LGPL-2.1-or-later
"""Repair must preserve sketch identity, including after serialization and undo."""

import os
import tempfile
import unittest

import FreeCAD as App
import Part
import Sketcher

from SketchReferenceRepair import (
    RepairError,
    Candidate,
    broken_references,
    candidates,
    repair,
    external_geometry,
)


def edge(x=0, length=10):
    return Part.makeLine(App.Vector(x, 0, 0), App.Vector(x + length, 0, 0))


def signature(sketch):
    constraints = tuple(
        (
            c.Type,
            c.Name,
            c.First,
            c.FirstPos,
            c.Second,
            c.SecondPos,
            c.Third,
            c.ThirdPos,
            c.Value,
            c.Driving,
            c.IsActive,
        )
        for c in sketch.Constraints
    )
    ids = tuple(sketch.getGeometryId(i) for i in range(sketch.GeometryCount))
    externals = tuple(
        (
            g.id,
            type(g.geometry).__name__,
            tuple(tuple(v.Point) for v in g.geometry.toShape().Vertexes),
        )
        for g in external_geometry(sketch)
    )
    points = tuple(
        tuple(sketch.getPoint(i, p))
        for i in range(sketch.GeometryCount)
        for p in (1, 2)
    )
    return constraints, tuple(sketch.ExpressionEngine), ids, externals, points


def rejected_snapshot(sketch):
    return (
        signature(sketch),
        tuple(sketch.ExternalGeometry),
        tuple((g.reference, g.missing) for g in external_geometry(sketch)),
        tuple(sketch.State),
        sketch.Document.UndoCount,
    )


class TestSketchReferenceRepair(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("ReferenceRepair")
        self.doc.UndoMode = 1
        self.source = self.doc.addObject("PartDesign::Feature", "Source")
        self.sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def broken(self, vertex=False, wanted=None):
        if wanted is None:
            wanted = Part.Vertex(App.Vector(0, 0, 0)) if vertex else edge()
        # Removing the last element leaves its cached external geometry available.
        self.source.Shape = Part.makeCompound(
            [edge(30), edge(50), edge(60), edge(70), wanted]
        )
        self.doc.recompute()
        self.sketch.addExternal("Source", "Vertex9" if vertex else "Edge5")
        self.sketch.addGeometry(
            Part.LineSegment(App.Vector(0, 0, 0), App.Vector(0, 5, 0))
        )
        self.sketch.addConstraint(Sketcher.Constraint("Coincident", 0, 1, -3, 1))
        dim = self.sketch.addConstraint(Sketcher.Constraint("Distance", 0, 5.0))
        self.sketch.renameConstraint(dim, "Height")
        self.sketch.setExpression("Constraints.Height", "2 + 3")
        self.doc.recompute()
        self.source.Shape = Part.makeCompound([edge(30)])
        self.doc.recompute()
        # A feature without element history cannot supply native rename recovery.
        return wanted.copy(noElementMap=True)

    def test_edge_recompute_save_reopen_undo_redo(self):
        wanted = self.broken()
        self.assertEqual(len(broken_references(self.sketch)), 1)
        self.source.Shape = Part.makeCompound([wanted, edge(30)])
        self.doc.recompute()
        before = signature(self.sketch)
        ref = broken_references(self.sketch)[0]
        ranked = candidates(self.sketch, ref)
        self.assertEqual(ranked[0].subelement, "Edge1")
        repair(self.sketch, [(ref, ranked[0])])
        self.doc.recompute()
        self.assertEqual(signature(self.sketch), before)
        self.assertFalse(broken_references(self.sketch))
        self.doc.undo()
        self.doc.recompute()
        self.assertEqual(signature(self.sketch), before)
        self.assertEqual(len(broken_references(self.sketch)), 1)
        self.doc.redo()
        self.doc.recompute()
        self.assertEqual(signature(self.sketch), before)
        self.assertFalse(broken_references(self.sketch))
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "repair.FCStd")
            self.doc.saveAs(path)
            App.closeDocument(self.doc.Name)
            self.doc = App.openDocument(path)
            self.doc.recompute()
            self.sketch = self.doc.getObject("Sketch")
            self.assertEqual(signature(self.sketch), before)
            self.assertFalse(broken_references(self.sketch))

    def test_vertex(self):
        wanted = self.broken(vertex=True)
        self.source.Shape = Part.makeCompound([wanted, edge(30)])
        self.doc.recompute()
        before = signature(self.sketch)
        ref = broken_references(self.sketch)[0]
        ranked = candidates(self.sketch, ref)
        self.assertEqual(ranked[0].subelement, "Vertex1")
        repair(self.sketch, [(ref, ranked[0])])
        self.doc.recompute()
        self.assertEqual(signature(self.sketch), before)
        self.assertFalse(broken_references(self.sketch))

    def test_ambiguous_unchanged(self):
        wanted = self.broken()
        self.source.Shape = Part.makeCompound(
            [wanted, wanted.copy(noElementMap=True), edge(30)]
        )
        self.doc.recompute()
        ref = broken_references(self.sketch)[0]
        before = rejected_snapshot(self.sketch)
        with self.assertRaisesRegex(RepairError, "[Aa]mbiguous"):
            repair(self.sketch, [(ref, candidates(self.sketch, ref)[0])])
        self.assertEqual(rejected_snapshot(self.sketch), before)

    def test_split_and_reversed_refused(self):
        self.broken()
        for replacement in (
            edge(length=5),
            Part.makeLine(App.Vector(10, 0, 0), App.Vector(0, 0, 0)),
        ):
            self.source.Shape = Part.makeCompound([replacement, edge(30)])
            self.doc.recompute()
            ref = broken_references(self.sketch)[0]
            before = rejected_snapshot(self.sketch)
            with self.assertRaisesRegex(RepairError, "endpoint|compatible"):
                repair(self.sketch, [(ref, candidates(self.sketch, ref)[0])])
            self.assertEqual(rejected_snapshot(self.sketch), before)

    def test_attachment_face(self):
        self.source.Shape = Part.makeBox(10, 10, 10)
        self.sketch.AttachmentSupport = (self.source, ["Face6"])
        self.sketch.MapMode = "FlatFace"
        self.doc.recompute()
        placement = self.sketch.Placement
        face = self.source.Shape.Faces[5].copy(noElementMap=True)
        self.source.Shape = Part.makeCompound([face])
        self.doc.recompute()
        ref = broken_references(self.sketch)[0]
        self.assertEqual(ref.kind, "attachment")
        repair(self.sketch, [(ref, candidates(self.sketch, ref)[0])])
        self.doc.recompute()
        self.assertTrue(self.sketch.Placement.isSame(placement, 1e-7))
        self.assertFalse(broken_references(self.sketch))

    def test_batch_is_one_transaction(self):
        wanted = self.broken()
        second = self.doc.addObject("PartDesign::Feature", "Second")
        second.Shape = Part.makeCompound([edge(50), edge(60), edge(20)])
        self.doc.recompute()
        self.sketch.addExternal(second.Name, "Edge3")
        self.doc.recompute()
        second.Shape = Part.makeCompound([edge(50)])
        self.doc.recompute()
        second.Shape = Part.makeCompound([edge(20), edge(50)])
        self.source.Shape = Part.makeCompound([wanted, edge(30)])
        self.doc.recompute()
        refs = broken_references(self.sketch)
        self.assertEqual(len(refs), 2)
        before = signature(self.sketch)
        undo_count = self.doc.UndoCount
        repair(self.sketch, [(ref, candidates(self.sketch, ref)[0]) for ref in refs])
        self.assertEqual(self.doc.UndoCount, undo_count + 1)
        self.doc.recompute()
        self.assertFalse(broken_references(self.sketch))
        self.assertEqual(signature(self.sketch), before)
        self.doc.undo()
        self.doc.recompute()
        self.assertEqual(len(broken_references(self.sketch)), 2)
        self.assertEqual(signature(self.sketch), before)

    def test_stale_selection_refused(self):
        wanted = self.broken()
        self.source.Shape = Part.makeCompound([wanted, edge(30)])
        self.doc.recompute()
        ref = broken_references(self.sketch)[0]
        selected = candidates(self.sketch, ref)[0]
        self.source.Shape = Part.makeCompound([edge(length=4), edge(30)])
        self.doc.recompute()
        before = rejected_snapshot(self.sketch)
        with self.assertRaises(RepairError):
            repair(self.sketch, [(ref, selected)])
        self.assertEqual(rejected_snapshot(self.sketch), before)

    def test_native_binding_rejects_invalid_ids(self):
        self.source.Shape = edge()
        self.doc.recompute()
        self.sketch.addExternal(self.source.Name, "Edge1")
        before = rejected_snapshot(self.sketch)
        self.assertEqual(self.sketch.getExternalReferenceType(-3), "Edge")
        for geo_id in (0, -1, -2, -1000):
            with self.assertRaises(ValueError):
                self.sketch.attachExternal(geo_id, self.source.Name, "Edge1")
            with self.assertRaises(ValueError):
                self.sketch.getExternalReferenceType(geo_id)
        with self.assertRaises(ValueError):
            self.sketch.attachExternal(-3, "MissingObject", "Edge1")
        self.assertEqual(rejected_snapshot(self.sketch), before)

    def test_arc(self):
        arc = Part.Arc(
            App.Vector(0, 0, 0), App.Vector(5, 5, 0), App.Vector(10, 0, 0)
        ).toShape()
        arc = self.broken(wanted=arc)
        self.source.Shape = Part.makeCompound([arc, edge(30)])
        self.doc.recompute()
        ref = broken_references(self.sketch)[0]
        before = signature(self.sketch)
        ranked = candidates(self.sketch, ref)
        self.assertEqual(ranked[0].subelement, "Edge1")
        self.assertTrue(ranked[0].compatible)
        repair(self.sketch, [(ref, ranked[0])])
        self.doc.recompute()
        self.assertFalse(broken_references(self.sketch))
        self.assertEqual(signature(self.sketch), before)

    def test_attachment_frame_change_rolls_back(self):
        self.source.Shape = Part.makeBox(10, 10, 10)
        self.sketch.AttachmentSupport = (self.source, ["Face6"])
        self.sketch.MapMode = "FlatFace"
        self.doc.recompute()
        placement = self.sketch.Placement
        face = self.source.Shape.Faces[5].copy(noElementMap=True)
        self.source.Shape = Part.makeCompound([face])
        # FlatFace projects the support object's origin onto the face plane.
        # Moving that origin within the plane changes the attachment frame,
        # while the candidate still has the same plane and normal.
        self.source.Placement = App.Placement(App.Vector(5, 0, 0), App.Rotation())
        self.doc.recompute()
        ref = broken_references(self.sketch)[0]
        before = rejected_snapshot(self.sketch), self.sketch.AttachmentSupport
        with self.assertRaisesRegex(RepairError, "placement"):
            repair(self.sketch, [(ref, candidates(self.sketch, ref)[0])])
        self.doc.recompute()
        self.assertEqual(
            (rejected_snapshot(self.sketch), self.sketch.AttachmentSupport), before
        )
        self.assertTrue(self.sketch.Placement.isSame(placement, 1e-7))

    def test_projected_edge_refuses_vertex_mapping(self):
        vertical = Part.makeLine(App.Vector(0, 0, 0), App.Vector(0, 0, 10))
        self.broken(wanted=vertical)
        self.assertIsInstance(external_geometry(self.sketch)[0].geometry, Part.Point)
        self.source.Shape = Part.makeCompound([Part.Vertex(App.Vector()), edge(30)])
        self.doc.recompute()
        ref = broken_references(self.sketch)[0]
        before = rejected_snapshot(self.sketch)
        with self.assertRaisesRegex(RepairError, "Candidate|type"):
            repair(
                self.sketch, [(ref, Candidate(self.source.Name, "Vertex1", 0, True))]
            )
        self.assertEqual(rejected_snapshot(self.sketch), before)
        self.assertTrue(
            all(c.subelement.startswith("Edge") for c in candidates(self.sketch, ref))
        )

    def test_whole_object_support_is_not_a_broken_subelement(self):
        datum = self.doc.addObject("App::FeaturePython", "Datum")
        self.sketch.AttachmentSupport = (datum, [""])
        self.doc.recompute()
        self.assertEqual(broken_references(self.sketch), [])

    def test_projected_edge_repairs_to_edge(self):
        vertical = Part.makeLine(App.Vector(0, 0, 0), App.Vector(0, 0, 10))
        vertical = self.broken(wanted=vertical)
        self.source.Shape = Part.makeCompound([vertical, edge(30)])
        self.doc.recompute()
        ref = broken_references(self.sketch)[0]
        ranked = candidates(self.sketch, ref)
        self.assertEqual(ranked[0].subelement, "Edge1")
        self.assertTrue(ranked[0].compatible)
        before = signature(self.sketch)
        repair(self.sketch, [(ref, ranked[0])])
        self.doc.recompute()
        self.assertFalse(broken_references(self.sketch))
        self.assertEqual(signature(self.sketch), before)
