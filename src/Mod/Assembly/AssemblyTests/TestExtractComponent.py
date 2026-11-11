# SPDX-License-Identifier: LGPL-2.1-or-later
"""Component extraction integration tests, shared by headless and GUI CI."""

import os
from pathlib import Path
import tempfile
import threading
import time
import unittest
from unittest.mock import patch
import zipfile

import FreeCAD as App
import Assembly
import Part
import Sketcher
import Spreadsheet  # noqa: F401 -- registers Spreadsheet::Sheet
from ExtractComponent import ExtractionError, extract, preflight
from Test.GuiRecompute import close_document_by_name


class TestExtractComponent(unittest.TestCase):
    def setUp(self):
        if App.GuiUp:
            import FreeCADGui as Gui

            Gui.activateWorkbench("AssemblyWorkbench")
        self.before = set(App.listDocuments())
        self.directory = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.directory.name, "Component.FCStd")
        self.source_path = os.path.join(self.directory.name, "Assembly.FCStd")
        self.doc = App.newDocument("ExtractionTest")
        self.assembly = Assembly.createAssembly(self.doc)
        self.part = self.assembly.newObject("App::Part", "LocalPart")
        self.part.Label = "Component label"
        self.body = self.part.newObject("PartDesign::Body", "Body")
        self.sketch = self.body.newObject("Sketcher::SketchObject", "Profile")
        self.sketch.addGeometry(
            Part.Circle(App.Vector(), App.Vector(0, 0, 1), 3), False
        )
        self.sketch.addConstraint(Sketcher.Constraint("Radius", 0, 3.0))
        self.pad = self.body.newObject("PartDesign::Pad", "Pad")
        self.pad.Profile = self.sketch
        self.pad.Length = 8
        self.doc.recompute()
        self.doc.saveAs(self.source_path)
        self.doc.clearUndos()

    def tearDown(self):
        if App.GuiUp:
            import FreeCADGui as Gui

            for name in list(App.listDocuments()):
                gui_document = Gui.getDocument(name)
                if gui_document:
                    gui_document.resetEdit()
        self.flushGui()
        for name in list(App.listDocuments()):
            if name not in self.before:
                close_document_by_name(name)
        self.flushGui()
        self.directory.cleanup()

    def flushGui(self):
        if App.GuiUp:
            import FreeCADGui as Gui
            from PySide import QtCore

            Gui.updateGui()
            QtCore.QCoreApplication.processEvents()

    def runExtract(self, component=None):
        return extract(self.assembly, component or self.part, self.path)

    def assertShape(self, first, second):
        self.assertAlmostEqual(first.Volume, second.Volume, places=7)
        self.assertAlmostEqual(first.Area, second.Area, places=7)
        self.assertEqual(len(first.Vertexes), len(second.Vertexes))
        # GUI tessellation changes cached BoundBox approximations. Compare the
        # exact geometry, with the same tolerance in headless and GUI runs.
        first_bounds = first.optimalBoundingBox(False)
        second_bounds = second.optimalBoundingBox(False)
        self.assertLess((first_bounds.Center - second_bounds.Center).Length, 1e-7)
        for axis in ("XLength", "YLength", "ZLength"):
            self.assertAlmostEqual(
                getattr(first_bounds, axis), getattr(second_bounds, axis), places=7
            )
        for a, b in zip(first.Vertexes, second.Vertexes):
            self.assertLess((a.Point - b.Point).Length, 1e-7)

    def test_part_body_history_and_placement(self):
        self.part.Placement = App.Placement(
            App.Vector(4, 6, 9), App.Rotation(20, 30, 40)
        )
        self.part.Visibility = False
        shape = self.part.Shape.copy()
        placement = self.part.Placement
        result = self.runExtract()
        self.assertEqual(result.leftover, ())
        self.assertEqual(result.link.TypeId, "App::Link")
        self.assertEqual(result.link.Label, "Component label")
        self.assertFalse(result.link.Visibility)
        self.assertTrue(result.link.Placement.isSame(placement, 1e-7))
        self.assertIn(result.link, self.assembly.Group)
        self.assertIsNone(self.doc.getObject("LocalPart"))
        self.assertIsNone(self.doc.getObject("Pad"))
        self.assertEqual(result.document.Pad.Profile[0], result.document.Profile)
        self.assertEqual(result.document.Body.Tip, result.document.Pad)
        self.assertShape(shape, result.link.Shape)
        self.assertTrue(os.path.isfile(self.path))

    def test_body_directly_in_assembly(self):
        self.part.removeObject(self.body)
        self.assembly.addObject(self.body)
        result = self.runExtract(self.body)
        self.assertEqual(result.component.TypeId, "PartDesign::Body")
        self.assertIn(result.link, self.assembly.Group)
        self.assertIsNotNone(self.doc.getObject("LocalPart"))
        self.assertGreater(result.link.Shape.Volume, 0)

    def test_part_feature(self):
        box = self.assembly.newObject("Part::Box", "Box")
        box.Length = 7
        sheet = self.doc.addObject("Spreadsheet::Sheet", "BoxReader")
        sheet.set("A1", "=Box.Length")
        sheet.set("A2", "=Box.Shape.Volume")
        sheet.set("A3", "=Box.Placement.Base.x")
        sheet.set("A4", "=Box.Shape.CenterOfMass.x")
        self.doc.recompute()
        result = self.runExtract(box)
        self.assertAlmostEqual(result.link.Shape.Volume, 700)
        result.component.Length = 12
        result.document.recompute()
        self.doc.recompute()
        self.assertAlmostEqual(result.link.Shape.Volume, 1200)
        self.assertAlmostEqual(float(sheet.get("A1")), 12)
        self.assertAlmostEqual(float(sheet.get("A2")), 1200)
        result.link.Placement.Base.x = 9
        self.doc.recompute()
        self.assertAlmostEqual(float(sheet.get("A3")), 9)
        self.assertAlmostEqual(result.component.Placement.Base.x, 0)
        self.assertAlmostEqual(float(sheet.get("A4")), 15)
        result.document.save()
        self.doc.save()
        close_document_by_name(self.doc.Name)
        close_document_by_name(result.document.Name)
        self.doc = App.openDocument(self.source_path)
        self.doc.recompute()
        self.assertAlmostEqual(float(self.doc.BoxReader.get("A4")), 15)

    def test_part_feature_with_csg_history(self):
        base = self.assembly.newObject("Part::Box", "BaseBox")
        tool = self.assembly.newObject("Part::Cylinder", "ToolCylinder")
        cut = self.assembly.newObject("Part::Cut", "Cut")
        cut.Base = base
        cut.Tool = tool
        base.Visibility = False
        tool.Visibility = False
        self.doc.recompute()
        shape = cut.Shape.copy()
        result = self.runExtract(cut)
        self.assertEqual(result.document.Cut.Base, result.document.BaseBox)
        self.assertEqual(result.document.Cut.Tool, result.document.ToolCylinder)
        self.assertIsNone(self.doc.getObject("BaseBox"))
        self.assertIsNone(self.doc.getObject("ToolCylinder"))
        self.assertShape(shape, result.link.Shape)

    def test_ground_and_joint_references_solve(self):
        fixed = self.assembly.newObject("Part::Box", "Other")
        ground = Assembly.createGroundedJoint(self.assembly, self.part)
        joint = Assembly.createJoint(
            self.assembly,
            "Fixed",
            Assembly.makeJointReference(
                self.part, "Body.Pad.Face3", "Body.Pad.Vertex1"
            ),
            Assembly.makeJointReference(fixed, "Face6", "Vertex1"),
            solve=False,
            presolve=True,
        )
        self.assertEqual(self.assembly.solve(False), 0)
        before = fixed.Placement
        result = self.runExtract()
        self.assertEqual(ground.ObjectToGround, result.link)
        self.assertEqual(joint.Reference1[0], result.link)
        self.assertEqual(self.assembly.solve(False), 0)
        self.assertTrue(fixed.Placement.isSame(before, 1e-6))
        self.assertTrue(any(obj == ground for obj, prop in result.redirected))
        joint_name, ground_name, link_name = joint.Name, ground.Name, result.link.Name
        self.doc.save()
        close_document_by_name(self.doc.Name)
        close_document_by_name(result.document.Name)
        self.doc = App.openDocument(self.source_path)
        self.assertEqual(self.doc.getObject(ground_name).ObjectToGround.Name, link_name)
        self.assertEqual(self.doc.getObject(joint_name).Reference1[0].Name, link_name)
        self.assertEqual(self.doc.Assembly.solve(False), 0)
        self.assertTrue(self.doc.Other.Placement.isSame(before, 1e-6))

    def test_joint_subelements_and_assembly_rooted_reference(self):
        owner = self.doc.addObject("App::FeaturePython", "Reader")
        owner.addProperty("App::PropertyLinkSub", "Reference")
        owner.Reference = (self.assembly, ["LocalPart.Body.Pad.Face1"])
        result = self.runExtract()
        root, subs = owner.Reference
        self.assertEqual(root, self.assembly)
        self.assertEqual(subs, [result.link.Name + ".Body.Pad.Face1"])
        self.assertIsNotNone(root.getSubObject(subs[0]))

    def test_extract_component_inside_nested_assembly(self):
        sub = Assembly.createAssembly(self.doc, "Nested")
        self.assembly.addObject(sub)
        self.assembly.removeObject(self.part)
        sub.addObject(self.part)
        result = self.runExtract()
        self.assertIn(result.link, sub.Group)
        self.assertNotIn(result.link, self.assembly.Group)
        self.assertGreater(result.link.Shape.Volume, 0)

    def test_mixed_xlink_list_and_assembly_rooted_paths(self):
        other = self.assembly.newObject("Part::Box", "Other")
        reader = self.doc.addObject("App::FeaturePython", "XLinkReader")
        reader.addProperty("App::PropertyXLinkSubList", "References")
        reader.References = [
            (self.assembly, ["LocalPart.Body.Pad.Face1"]),
            (self.pad, ["Face2"]),
            (other, ["Face3"]),
        ]
        self.doc.recompute()
        shapes = {
            "root": self.assembly.getSubObject("LocalPart.Body.Pad.Face1"),
            "pad": self.pad.getSubObject("Face2"),
            "other": other.getSubObject("Face3"),
        }
        result = self.runExtract()
        link_name = result.link.Name

        def check(references):
            self.assertEqual(len(references), 3)
            seen = set()
            for obj, subs in references:
                if obj.TypeId == "Assembly::AssemblyObject":
                    kind = "root"
                    self.assertTrue(subs[0].startswith(link_name + "."))
                elif obj.Name == "Other":
                    kind = "other"
                else:
                    kind = "pad"
                    self.assertEqual(
                        obj.getLinkedObject(True).TypeId, "PartDesign::Pad"
                    )
                seen.add(kind)
                self.assertShape(shapes[kind], obj.getSubObject(subs[0]))
            self.assertEqual(seen, set(shapes))

        check(reader.References)
        self.assertIn((reader, "References"), result.redirected)
        self.doc.save()
        close_document_by_name(self.doc.Name)
        close_document_by_name(result.document.Name)
        self.doc = App.openDocument(self.source_path)
        check(self.doc.XLinkReader.References)

    def test_extract_subassembly_with_grounded_part(self):
        sub = Assembly.createAssembly(self.doc, "Nested")
        self.assembly.addObject(sub)
        self.assembly.removeObject(self.part)
        sub.addObject(self.part)
        Assembly.createGroundedJoint(sub, self.part)
        result = self.runExtract(sub)
        self.assertEqual(result.component.TypeId, "Assembly::AssemblyObject")
        self.assertEqual(result.component.solve(False), 0)
        self.assertEqual(
            result.document.GroundedJoint.ObjectToGround, result.document.LocalPart
        )
        self.assertIsNone(self.doc.getObject("Body"))

    def test_subassembly_with_external_component(self):
        standard = App.newDocument("StandardPart")
        box = standard.addObject("Part::Box", "StandardBox")
        standard.recompute()
        standard.saveAs(os.path.join(self.directory.name, "Standard.FCStd"))
        sub = Assembly.createAssembly(self.doc, "Nested")
        self.assembly.addObject(sub)
        child = sub.newObject("App::Link", "StandardComponent")
        child.LinkedObject = box
        Assembly.createGroundedJoint(sub, child)
        result = self.runExtract(sub)
        self.assertEqual(result.document.StandardComponent.getLinkedObject(), box)
        self.assertEqual(result.component.solve(False), 0)
        root_name = result.link.Name
        self.doc.save()
        close_document_by_name(self.doc.Name)
        close_document_by_name(result.document.Name)
        close_document_by_name(standard.Name)
        self.doc = App.openDocument(self.source_path)
        root = self.doc.getObject(root_name)
        component = root.getLinkedObject()
        self.assertEqual(component.TypeId, "Assembly::AssemblyObject")
        self.assertEqual(
            component.Document.StandardComponent.getLinkedObject().TypeId, "Part::Box"
        )
        self.assertAlmostEqual(root.getSubObject("StandardComponent.").Volume, 1000)
        self.assertEqual(component.solve(False), 0)

    def test_expression_and_spreadsheet_and_other_links(self):
        reader = self.doc.addObject("App::FeaturePython", "Reader")
        reader.addProperty("App::PropertyLength", "Value")
        reader.setExpression("Value", "Pad.Length + LocalPart.Placement.Base.x")
        sheet = self.doc.addObject("Spreadsheet::Sheet", "Sheet")
        sheet.set("A1", "=Pad.Length")
        other = self.doc.addObject("App::Link", "OtherLink")
        other.LinkedObject = self.pad
        self.doc.recompute()
        result = self.runExtract()
        self.assertAlmostEqual(reader.Value.Value, 8)
        self.assertAlmostEqual(float(sheet.get("A1")), 8)
        self.assertEqual(other.getLinkedObject(True), result.document.Pad)
        result.document.Pad.Length = 13
        result.document.recompute()
        self.doc.recompute()
        self.assertAlmostEqual(reader.Value.Value, 13)
        self.assertAlmostEqual(float(sheet.get("A1")), 13)
        self.assertAlmostEqual(other.Shape.Volume, result.document.Pad.Shape.Volume)

    def test_label_based_expressions(self):
        reader = self.doc.addObject("App::FeaturePython", "LabelReader")
        reader.addProperty("App::PropertyLength", "Value")
        self.part.Placement.Base.x = 5
        reader.setExpression(
            "Value", "<<Pad>>.Length + <<Component label>>.Placement.Base.x"
        )
        self.doc.recompute()
        result = self.runExtract()
        self.assertTrue(reader.ExpressionEngine)
        self.assertAlmostEqual(reader.Value.Value, 13)
        result.document.Pad.Length = 11
        result.document.recompute()
        self.doc.recompute()
        self.assertAlmostEqual(reader.Value.Value, 16)
        self.assertEqual(result.link.Label, "Component label")

    def makeExternalGeometry(self):
        body = self.assembly.newObject("PartDesign::Body", "Consumer")
        # Create the reference in a body, then move the sketch to a different
        # component: an established in-context reference must survive extraction.
        sketch = self.body.newObject("Sketcher::SketchObject", "ConsumerSketch")
        sketch.AttachmentSupport = (self.pad, ["Face3"])
        sketch.MapMode = "FlatFace"
        edge_name = next(
            f"Edge{i}"
            for i, edge in enumerate(self.pad.Shape.Edges, 1)
            if isinstance(edge.Curve, Part.Circle)
        )
        sketch.addExternal(self.pad.Name, edge_name)
        sketch.addGeometry(
            Part.Circle(App.Vector(10, 0, 0), App.Vector(0, 0, 1), 3), False
        )
        sketch.addConstraint(Sketcher.Constraint("Equal", 0, -3))
        self.body.removeObject(sketch)
        body.addObject(sketch)
        self.body.Tip = self.pad
        binder = body.newObject("PartDesign::ShapeBinder", "Binder")
        binder.Support = [(self.pad, ["Face1"])]
        subbinder = body.newObject("PartDesign::SubShapeBinder", "SubBinder")
        subbinder.Support = [(self.pad, ["Face1"])]
        attachment = self.doc.addObject("Part::Feature", "Attached")
        attachment.addExtension("Part::AttachExtensionPython")
        attachment.AttachmentSupport = (self.pad, ["Face3"])
        attachment.MapMode = "FlatFace"
        self.doc.recompute()
        return sketch, binder, subbinder, attachment

    def test_sketch_binders_and_attachment(self):
        sketch, binder, subbinder, attachment = self.makeExternalGeometry()
        shape = binder.Shape.copy()
        subshape = subbinder.Shape.copy()
        placement = attachment.Placement
        result = self.runExtract()
        self.assertTrue(sketch.ExternalGeometry)
        self.assertIn(sketch.ExternalGeometry[0][0], result.geometry_binders)
        self.assertEqual(
            sketch.ExternalGeometry[0][0].Support[0][0], result.document.Pad
        )
        self.assertShape(shape, binder.Shape)
        self.assertShape(subshape, subbinder.Shape)
        self.assertTrue(attachment.Placement.isSame(placement, 1e-6))
        self.assertTrue(result.forwarding_links)
        self.assertTrue(
            all(
                obj.State == ["Up-to-date"] or "Invalid" not in obj.State
                for obj in (sketch, binder, subbinder, attachment)
            )
        )

    def test_transformed_component_and_traced_geometry(self):
        self.part.Placement = App.Placement(
            App.Vector(7, 11, 13), App.Rotation(App.Vector(0, 0, 1), 25)
        )
        sketch, binder, subbinder, attachment = self.makeExternalGeometry()
        binder.TraceSupport = True
        subbinder.Context = (self.assembly, ["Consumer.SubBinder."])
        self.doc.recompute()
        before, sub_before = binder.Shape.copy(), subbinder.Shape.copy()
        sketch_before = sketch.Shape.copy()
        external_center = App.Vector(sketch.ExternalGeo[2].Center)
        attached = attachment.Placement
        result = self.runExtract()
        self.assertShape(before, binder.Shape)
        self.assertShape(sub_before, subbinder.Shape)
        self.assertShape(sketch_before, sketch.Shape)
        self.assertLess((sketch.ExternalGeo[2].Center - external_center).Length, 1e-7)
        self.assertTrue(attachment.Placement.isSame(attached, 1e-6))
        result.link.Placement.Base.x += 5
        self.doc.recompute()
        self.assertLess(
            (
                binder.Shape.BoundBox.Center
                - before.BoundBox.Center
                - App.Vector(5, 0, 0)
            ).Length,
            1e-6,
        )
        self.assertLess(
            (attachment.Placement.Base - attached.Base - App.Vector(5, 0, 0)).Length,
            1e-6,
        )

    def test_origin_plane_attachment(self):
        self.part.Placement.Base = App.Vector(3, 5, 7)
        plane = next(
            obj for obj in self.body.Origin.OriginFeatures if obj.TypeId == "App::Plane"
        )
        attached = self.doc.addObject("Part::Feature", "DatumAttached")
        attached.addExtension("Part::AttachExtensionPython")
        attached.AttachmentSupport = (plane, [""])
        attached.MapMode = "FlatFace"
        self.doc.recompute()
        self.assertNotIn("Invalid", attached.State)
        placement = attached.Placement
        result = self.runExtract()
        self.assertTrue(attached.Placement.isSame(placement, 1e-6))
        self.assertTrue(
            any(obj.TypeId == "App::Plane" for obj in result.geometry_binders)
        )
        self.assertNotIn("Invalid", attached.State)

    def test_attachment_paths_rooted_at_part_and_body(self):
        self.part.Placement = App.Placement(
            App.Vector(7, 11, 13), App.Rotation(App.Vector(0, 0, 1), 25)
        )
        self.body.Placement.Base = App.Vector(2, 3, 0)
        attached = []
        for target, path in (
            (self.part, "Body.Pad.Face3"),
            (self.body, "Pad.Face3"),
        ):
            obj = self.doc.addObject("Part::Feature", "PathAttached")
            obj.addExtension("Part::AttachExtensionPython")
            obj.AttachmentSupport = (target, [path])
            obj.MapMode = "FlatFace"
            attached.append(obj)
        self.doc.recompute()
        placements = [obj.Placement for obj in attached]
        for obj in attached:
            self.assertNotIn("Invalid", obj.State)
        result = self.runExtract()
        for obj, placement in zip(attached, placements):
            self.assertNotIn("Invalid", obj.State)
            self.assertTrue(obj.Placement.isSame(placement, 1e-6))
        result.link.Placement.Base.x += 5
        self.doc.recompute()
        for obj, placement in zip(attached, placements):
            self.assertLess(
                (obj.Placement.Base - placement.Base - App.Vector(5, 0, 0)).Length,
                1e-6,
            )

    def test_one_undo_and_redo(self):
        reader = self.doc.addObject("App::FeaturePython", "Reader")
        reader.addProperty("App::PropertyLength", "Value")
        reader.setExpression("Value", "Pad.Length")
        sketch, binder, subbinder, attachment = self.makeExternalGeometry()
        self.doc.clearUndos()
        result = self.runExtract()
        self.assertEqual(self.doc.UndoCount, 1)
        link_name = result.link.Name
        self.doc.undo()
        self.doc.recompute()
        self.assertIsNotNone(self.doc.getObject("LocalPart"))
        self.assertIsNotNone(self.doc.getObject("Pad"))
        self.assertIsNone(self.doc.getObject(link_name))
        self.assertEqual(reader.ExpressionEngine[0][1], "Pad.Length")
        self.assertEqual(sketch.ExternalGeometry[0][0], self.doc.Pad)
        self.assertEqual(binder.Support[0][0], self.doc.Pad)
        self.assertNotIn("Invalid", sketch.State)
        self.assertTrue(os.path.isfile(self.path))
        self.doc.redo()
        self.doc.recompute()
        self.assertIsNone(self.doc.getObject("LocalPart"))
        self.assertEqual(
            self.doc.getObject(link_name).getLinkedObject(), result.component
        )
        self.assertAlmostEqual(reader.Value.Value, 8)

    def test_save_reopen_relative_links_and_edit(self):
        sketch, binder, subbinder, attachment = self.makeExternalGeometry()
        reader = self.doc.addObject("App::FeaturePython", "Reader")
        reader.addProperty("App::PropertyLength", "Value")
        reader.setExpression("Value", "Pad.Length")
        sheet = self.doc.addObject("Spreadsheet::Sheet", "Sheet")
        sheet.set("A1", "=Pad.Length")
        result = self.runExtract()
        link_name = result.link.Name
        self.doc.save()
        with zipfile.ZipFile(self.source_path) as archive:
            xml = archive.read("Document.xml").decode()
        self.assertIn("Component.FCStd", xml)
        self.assertNotIn(self.directory.name + "/Component.FCStd", xml)
        close_document_by_name(self.doc.Name)
        close_document_by_name(result.document.Name)
        relocated = os.path.join(self.directory.name, "Relocated")
        os.mkdir(relocated)
        for attribute in ("source_path", "path"):
            old = getattr(self, attribute)
            new = os.path.join(relocated, os.path.basename(old))
            os.rename(old, new)
            setattr(self, attribute, new)
        # Loading the relocated assembly must resolve its relative external link.
        self.doc = App.openDocument(self.source_path)
        link = self.doc.getObject(link_name)
        self.assertIsNotNone(link.getLinkedObject())
        target = link.getLinkedObject().Document
        self.assertTrue(os.path.samefile(target.FileName, self.path))
        target.Pad.Length = 17
        target.recompute()
        target.save()
        self.doc.recompute()
        self.assertAlmostEqual(self.doc.Reader.Value.Value, 17)
        self.assertAlmostEqual(float(self.doc.Sheet.get("A1")), 17)
        self.assertTrue(self.doc.ConsumerSketch.ExternalGeometry)
        self.assertFalse(self.doc.Binder.Shape.isNull())
        self.assertFalse(self.doc.SubBinder.Shape.isNull())
        self.assertNotIn("Invalid", self.doc.Attached.State)
        self.assertAlmostEqual(
            self.doc.Binder.Shape.Area, target.Pad.Shape.Faces[0].Area, places=6
        )
        self.assertAlmostEqual(
            self.doc.SubBinder.Shape.Area, target.Pad.Shape.Faces[0].Area, places=6
        )
        target.Profile.setDatum(0, App.Units.Quantity("4 mm"))
        target.recompute()
        self.doc.recompute()
        self.assertAlmostEqual(
            self.doc.Binder.Shape.Area, target.Pad.Shape.Faces[0].Area, places=6
        )
        self.assertAlmostEqual(
            self.doc.ConsumerSketch.ExternalGeo[2].Radius, 4, places=6
        )
        self.assertAlmostEqual(self.doc.ConsumerSketch.Geometry[0].Radius, 4, places=6)
        self.assertEqual(self.doc.ConsumerSketch.ConstraintCount, 1)
        self.assertShape(target.LocalPart.Shape, link.Shape)

    def assertRefused(self, component=None, path=None):
        names = [obj.Name for obj in self.doc.Objects]
        undos = self.doc.UndoCount
        with self.assertRaises(ExtractionError) as raised:
            extract(self.assembly, component or self.part, path or self.path)
        self.assertEqual(names, [obj.Name for obj in self.doc.Objects])
        self.assertEqual(undos, self.doc.UndoCount)
        return raised.exception

    def assertKeptCommit(self, message, undos_before, detail):
        self.assertIn(self.path, message)
        self.assertIn("did not finish updating", message)
        self.assertIn("Undo restores the assembly", message)
        self.assertIn("extracted file stays on disk", message)
        self.assertIn(detail, message)
        self.assertEqual(
            list(self.doc.UndoNames),
            ["Extract component to file", *undos_before],
        )
        self.assertEqual(list(self.doc.RedoNames), [])
        self.assertIsNone(self.doc.getObject("LocalPart"))
        link = self.doc.getObject("LocalPartLink")
        self.assertIsNotNone(link)
        self.assertTrue(os.path.isfile(self.path))
        self.assertEqual(
            os.path.realpath(link.LinkedObject.Document.FileName),
            os.path.realpath(self.path),
        )
        self.assertIn(link.LinkedObject.Document.Name, App.listDocuments())
        return link

    def assertKeptUnverified(self, error, detail):
        message = str(error)
        self.assertTrue(os.path.isfile(self.path), message)
        self.assertIn(self.path, message)
        self.assertIn("could not be verified", message)
        self.assertIn("were kept", message)
        self.assertIn(detail, message)
        kept = [
            doc
            for doc in App.listDocuments().values()
            if os.path.realpath(doc.FileName or "") == os.path.realpath(self.path)
        ]
        self.assertEqual(len(kept), 1)
        self.assertIn(kept[0].Name, App.listDocuments())
        self.assertIsNotNone(self.doc.getObject("LocalPart"))
        return message

    def patchSourceCommit(self, status, mutate=None):
        """Replace the source commit with a result dict's status, running no callback."""
        import ExtractComponent

        real = ExtractComponent._commit_on_owner

        def commit(document, callback, pump=None, **kwargs):
            if getattr(document, "Name", None) == self.doc.Name:
                if mutate is not None:
                    mutate()
                error = ExtractionError(f"commit status {status}")
                error.commit_status = status
                raise error
            return real(document, callback, pump=pump, **kwargs)

        return patch("ExtractComponent._commit_on_owner", side_effect=commit)

    def _destination_saved_here(self):
        path = os.path.realpath(self.path)
        matches = [
            doc
            for doc in App.listDocuments().values()
            if doc.FileName and os.path.realpath(doc.FileName) == path
        ]
        self.assertEqual(len(matches), 1, [getattr(doc, "Name", None) for doc in matches])
        return matches[0]

    def test_refuse_unsaved(self):
        self.doc.FileName = ""
        self.assertRefused()
        self.assertFalse(os.path.exists(self.path))

    def test_refuse_existing_file(self):
        with open(self.path, "wb") as stream:
            stream.write(b"keep me")
        self.assertRefused()
        with open(self.path, "rb") as stream:
            self.assertEqual(stream.read(), b"keep me")

    def test_refuse_link_and_nonmember(self):
        link = self.assembly.newObject("App::Link", "Link")
        link.LinkedObject = self.part
        self.assertRefused(link)
        box = self.doc.addObject("Part::Box", "Unrelated")
        self.assertRefused(box)
        self.assertRefused(self.assembly)

    def test_refuse_outbound_dependency(self):
        outside = self.assembly.newObject("Part::Box", "Outside")
        self.pad.setExpression("Length", "Outside.Length")
        error = self.assertRefused()
        self.assertTrue(error.leftover)
        self.assertFalse(os.path.exists(self.path))

    def test_internal_placement_expression_updates(self):
        reader = self.doc.addObject("App::FeaturePython", "Reader")
        reader.addProperty("App::PropertyLength", "Value")
        reader.setExpression("Value", "Pad.Placement.Base.x")
        result = self.runExtract()
        result.document.Profile.Placement.Base.x = 12
        result.document.recompute()
        self.doc.recompute()
        self.assertAlmostEqual(reader.Value.Value, 12)

    def test_refuse_opaque_python_reference(self):
        reader = self.doc.addObject("App::FeaturePython", "OpaqueReader")
        reader.addProperty("App::PropertyPythonObject", "OpaqueReference")
        reader.OpaqueReference = {"component": self.part}
        error = self.assertRefused()
        self.assertIn((reader.Name, "OpaqueReference"), error.leftover)
        self.assertFalse(os.path.exists(self.path))

    def test_refuse_existing_transaction(self):
        self.doc.openTransaction("Unfinished edit")
        self.part.Label = "Unfinished edit"
        try:
            self.assertRefused()
            self.assertTrue(self.doc.HasPendingTransaction)
        finally:
            self.doc.abortTransaction()

    def test_refuse_opaque_reference_in_other_document(self):
        other = App.newDocument("OpaqueOtherDocument")
        reader = other.addObject("App::FeaturePython", "OpaqueReader")
        reader.addProperty("App::PropertyPythonObject", "Payload")
        reader.Payload = {"nested": [self.sketch, {"component": self.part}]}
        documents = set(App.listDocuments())
        files = set(os.listdir(self.directory.name))
        with patch(
            "ExtractComponent.tempfile.mkstemp",
            side_effect=AssertionError("preflight must refuse before creating a file"),
        ) as staging:
            error = self.assertRefused()
            staging.assert_not_called()
        self.assertIn((reader.FullName, "Payload"), error.leftover)
        self.assertEqual(reader.Payload["nested"][0], self.sketch)
        self.assertEqual(documents, set(App.listDocuments()))
        self.assertEqual(files, set(os.listdir(self.directory.name)))

    def test_refuse_external_pad_profile_before_writing(self):
        component = self.assembly.newObject("App::Part", "ProfileComponent")
        profile = component.newObject("Sketcher::SketchObject", "ExternalProfile")
        profile.addGeometry(Part.Circle(App.Vector(), App.Vector(0, 0, 1), 2), False)
        body = self.assembly.newObject("PartDesign::Body", "OtherBody")
        pad = body.newObject("PartDesign::Pad", "OtherPad")
        pad.Profile = profile
        pad.Length = 4
        self.doc.recompute()
        shape = pad.Shape.copy()
        files = set(os.listdir(self.directory.name))
        backlinks = {obj.Name: set(x.Name for x in obj.InList) for obj in self.doc.Objects}
        with patch(
            "ExtractComponent.tempfile.mkstemp",
            side_effect=AssertionError("preflight must refuse before creating a file"),
        ) as staging:
            error = self.assertRefused(component)
            staging.assert_not_called()
        self.assertIn((pad.Name, "Profile"), error.leftover)
        self.assertEqual(pad.Profile[0], profile)
        self.assertShape(shape, pad.Shape)
        self.assertEqual(files, set(os.listdir(self.directory.name)))
        self.assertEqual(
            backlinks,
            {obj.Name: set(x.Name for x in obj.InList) for obj in self.doc.Objects},
        )

    def test_destination_has_final_identity_and_savepoint(self):
        result = self.runExtract()
        self.assertEqual(result.document.FileName, self.path)
        self.assertFalse(result.document.hasPendingFileChanges())
        self.assertEqual(result.document.getFileChangeState()["canonical_path"], self.path)
        if App.GuiUp:
            import FreeCADGui as Gui

            self.flushGui()
            self.assertFalse(
                Gui.getDocument(result.document).Modified,
                result.document.getFileChangeState(),
            )
        self.assertEqual(
            sorted(os.listdir(self.directory.name)),
            sorted([os.path.basename(self.source_path), os.path.basename(self.path)]),
        )

    def test_refuse_unadapted_native_feature_reference(self):
        box = self.assembly.newObject("Part::Box", "Outside")
        cut = self.assembly.newObject("Part::Cut", "OutsideCut")
        cut.Base = self.pad
        cut.Tool = box
        self.doc.recompute()
        with patch(
            "ExtractComponent.tempfile.mkstemp",
            side_effect=AssertionError("preflight must refuse before creating a file"),
        ) as staging:
            error = self.assertRefused()
            staging.assert_not_called()
        self.assertIn((cut.Name, "Base"), error.leftover)
        self.assertEqual(cut.Base, self.pad)

    def test_refuse_shared_ownership(self):
        group = self.doc.addObject("App::FeaturePython", "AlsoOwnsBody")
        group.addProperty("App::PropertyLinkList", "Group")
        group.Group = [self.body]
        self.assertTrue(self.assertRefused().leftover)

    def test_refuse_owned_python_state_with_document_references(self):
        self.part.addProperty("App::PropertyPythonObject", "Payload")
        self.part.Payload = {"profile": self.sketch}
        error = self.assertRefused()
        self.assertIn((self.part.Name, "Payload"), error.leftover)
        self.assertEqual(self.part.Payload["profile"], self.sketch)
        self.assertFalse(os.path.exists(self.path))

    def test_refuse_slotted_python_reference(self):
        class BaseState:
            __slots__ = ("__reference",)

            def __init__(self, reference):
                self.__reference = reference

            def dumps(self):
                return {}

        class State(BaseState):
            __slots__ = ("cycle",)

            def __init__(self, reference):
                super().__init__(reference)
                self.cycle = self

        reader = self.doc.addObject("App::FeaturePython", "SlottedReader")
        reader.addProperty("App::PropertyPythonObject", "Payload")
        # dumps() hides the slot, so a check of the serialized form accepts it.
        reader.Payload = State(self.part)
        error = self.assertRefused()
        self.assertIn((reader.Name, "Payload"), error.leftover)

    def test_iterator_slots_are_inspected(self):
        import ExtractComponent

        owned = self.part

        class State:
            __slots__ = iter(("hidden",))

            def __init__(self, hidden):
                self.hidden = hidden

            def dumps(self):
                return {}

        value = State(owned)
        self.assertTrue(ExtractComponent._containsOwned(value, [owned]))
        other = App.newDocument("SlotIterator")
        reader = other.addObject("App::FeaturePython", "Reader")
        reader.addProperty("App::PropertyPythonObject", "Payload")
        reader.Payload = value
        error = self.assertRefused()
        self.assertIn((reader.FullName, "Payload"), error.leftover)
        self.assertFalse(os.path.exists(self.path))

    def test_slots_iterable_is_not_called(self):
        import ExtractComponent

        calls = []

        class Slots:
            def __iter__(self):
                calls.append(True)
                yield "hidden"

        class State:
            __slots__ = Slots()

            def dumps(self):
                return {}

        calls.clear()
        value = State()
        value.hidden = 1
        self.assertFalse(ExtractComponent._containsOwned(value, [self.part]))
        self.assertFalse(ExtractComponent._python_state_blocks(value, [self.part]))
        self.assertEqual(calls, [])

    def test_refuse_frozenset_python_reference(self):
        class Holder:
            def __init__(self, refs):
                self.refs = refs

            def dumps(self):
                return {"hidden": True}

        reader = self.doc.addObject("App::FeaturePython", "FrozenSetReader")
        reader.addProperty("App::PropertyPythonObject", "Payload")
        reader.Payload = Holder(frozenset([self.part]))
        error = self.assertRefused()
        self.assertIn((reader.Name, "Payload"), error.leftover)

    def test_refuse_uninspectable_python_state(self):
        calls = []

        class State:
            @property
            def __dict__(self):
                calls.append(True)
                return {}

            def dumps(self):
                return {}

        reader = self.doc.addObject("App::FeaturePython", "OpaqueReader")
        reader.addProperty("App::PropertyPythonObject", "Payload")
        reader.Payload = State()
        error = self.assertRefused()
        self.assertIn((reader.Name, "Payload"), error.leftover)
        self.assertEqual(calls, [])

    def test_refuse_flexible_assembly_link(self):
        definition = App.newDocument("LinkedAssembly")
        subassembly = Assembly.createAssembly(definition)
        definition.recompute()
        definition.saveAs(os.path.join(self.directory.name, "LinkedAssembly.FCStd"))
        flexible = self.assembly.newObject("Assembly::AssemblyLink", "Flexible")
        flexible.LinkedObject = subassembly
        flexible.Rigid = False
        self.doc.recompute()
        with patch("ExtractComponent.App.newDocument") as new_document:
            error = self.assertRefused(flexible)
            new_document.assert_not_called()
        self.assertIn("AssemblyLink", str(error))
        self.assertFalse(flexible.Rigid)

    def test_refuse_nested_flexible_assembly_link(self):
        definition = App.newDocument("NestedLinkedAssembly")
        subassembly = Assembly.createAssembly(definition)
        definition.recompute()
        definition.saveAs(os.path.join(self.directory.name, "NestedLinkedAssembly.FCStd"))
        flexible = self.part.newObject("Assembly::AssemblyLink", "NestedFlexible")
        flexible.LinkedObject = subassembly
        flexible.Rigid = False
        self.doc.recompute()
        with patch("ExtractComponent.App.newDocument") as new_document:
            error = self.assertRefused()
            new_document.assert_not_called()
        self.assertIn("AssemblyLink", str(error))
        self.assertFalse(flexible.Rigid)

    def test_refuse_frozen_source_before_writing(self):
        self.doc.RecomputesFrozen = True
        self.pad.Length = 9
        try:
            with patch("ExtractComponent.App.newDocument") as new_document:
                self.assertIn("Unfreeze", str(self.assertRefused()))
                new_document.assert_not_called()
            self.assertFalse(os.path.exists(self.path))
            self.assertIsNotNone(self.doc.LocalPart)
        finally:
            self.doc.RecomputesFrozen = False

    def test_refuse_unwritable_destination_before_creating_document(self):
        locked = os.path.join(self.directory.name, "locked")
        os.mkdir(locked)
        os.chmod(locked, 0o555)
        path = os.path.join(locked, "Component.FCStd")
        if os.access(locked, os.W_OK):
            os.chmod(locked, 0o755)
            self.skipTest("process can write a mode-0555 directory")
        try:
            with patch("ExtractComponent.App.newDocument") as new_document:
                error = self.assertRefused(path=path)
                new_document.assert_not_called()
            self.assertIn("writable", str(error).lower())
            self.assertFalse(os.path.exists(path))
        finally:
            if os.path.isdir(locked):
                os.chmod(locked, 0o755)

    def test_failed_extraction_keeps_redo_history(self):
        import AssemblyApp

        self.doc.openTransaction("Rename component")
        self.part.Label = "Before extract"
        self.doc.commitTransaction()
        self.doc.undo()
        self.assertGreater(self.doc.RedoCount, 0)
        self.assertEqual(self.part.Label, "Component label")
        redos = list(self.doc.RedoNames)
        undos = list(self.doc.UndoNames)
        undo_count = self.doc.UndoCount
        original = AssemblyApp._extractionReferences

        def fail(owners, pairs, apply=False):
            report = original(owners, pairs, apply)
            if apply:
                raise RuntimeError("replacement rejected")
            return report

        with patch.object(AssemblyApp, "_extractionReferences", side_effect=fail):
            # The binding re-raises the callback error and drops ApplyFailed, so
            # the rollback status is not visible and the file stays.
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        self.assertKeptUnverified(raised.exception, "replacement rejected")
        self.assertIsInstance(raised.exception.__cause__, RuntimeError)
        self.assertEqual(list(self.doc.RedoNames), redos)
        self.assertEqual(list(self.doc.UndoNames), undos)
        self.assertEqual(self.doc.UndoCount, undo_count)
        self.assertEqual(self.part.Label, "Component label")
        self.assertIsNotNone(self.doc.getObject("LocalPart"))

    def test_refuse_missing_external_component_file(self):
        definition = App.newDocument("ExternalDefinition")
        box = definition.addObject("Part::Box", "Box")
        definition.recompute()
        definition.saveAs(os.path.join(self.directory.name, "External.FCStd"))
        child = self.part.newObject("App::Link", "MissingComponent")
        child.LinkedObject = box
        self.doc.recompute()
        os.unlink(definition.FileName)
        error = self.assertRefused()
        self.assertIn((child.Name, box.FullName), error.leftover)
        self.assertEqual(child.LinkedObject, box)
        self.assertFalse(os.path.exists(self.path))

    def test_refuse_references_from_other_document(self):
        other = App.newDocument("OtherDocument")
        other.saveAs(os.path.join(self.directory.name, "Other.FCStd"))
        link = other.addObject("App::Link", "Link")
        link.LinkedObject = self.part
        self.assertTrue(self.assertRefused().leftover)
        self.assertEqual(link.LinkedObject, self.part)

    def test_refuse_invalid_path(self):
        self.assertRefused(
            path=os.path.join(self.directory.name, "missing", "Part.FCStd")
        )
        self.assertRefused(path=os.path.join(self.directory.name, "Part.txt"))

    def test_write_failure_does_not_change_source(self):
        import ExtractComponent

        names = [obj.Name for obj in self.doc.Objects]
        mkstemp = ExtractComponent.tempfile.mkstemp

        def racing_writer(*args, **kwargs):
            with open(self.path, "wb") as stream:
                stream.write(b"concurrent writer")
            return mkstemp(*args, **kwargs)

        with patch("ExtractComponent.tempfile.mkstemp", side_effect=racing_writer):
            with self.assertRaises((ExtractionError, OSError)):
                self.runExtract()
        self.assertEqual(names, [obj.Name for obj in self.doc.Objects])
        with open(self.path, "rb") as stream:
            self.assertEqual(stream.read(), b"concurrent writer")
        self.assertEqual(set(App.listDocuments()) - self.before, {self.doc.Name})

    def test_replacement_failure_rolls_back_and_keeps_unverified_file(self):
        import AssemblyApp
        from ExtractComponent import _owned

        original = AssemblyApp._extractionReferences
        sketch, binder, subbinder, attachment = self.makeExternalGeometry()
        reader = self.doc.addObject("App::FeaturePython", "Reader")
        reader.addProperty("App::PropertyLength", "Value")
        reader.setExpression("Value", "Pad.Length")
        other_link = self.assembly.newObject("App::Link", "OtherLink")
        other_link.LinkedObject = self.part
        self.doc.recompute()
        self.doc.clearUndos()
        names = [obj.Name for obj in self.doc.Objects]
        owned_names = tuple(obj.Name for obj in _owned(self.part))
        geometry = {
            obj.Name: obj.Shape.copy()
            for obj in (self.part, self.pad, sketch, binder, subbinder)
        }
        placement = attachment.Placement
        external = sketch.ExternalGeometry
        constraints = sketch.ConstraintCount
        expressions = reader.ExpressionEngine
        files = {path.name: path.read_bytes() for path in Path(self.directory.name).iterdir()}
        file_state = self.doc.getFileChangeState()
        mutated = []

        def fail(owners, pairs, apply=False):
            report = original(owners, pairs, apply)
            if apply:
                self.assertNotEqual(other_link.LinkedObject, self.part)
                self.assertNotEqual(sketch.ExternalGeometry, external)
                self.assertTrue(report["redirected"])
                # Fail after native reference mutation AND actual deletions.
                for name in owned_names:
                    # Removing a Body also removes its origin automatically.
                    if self.doc.getObject(name) is not None:
                        self.doc.removeObject(name)
                self.assertIsNone(self.doc.getObject("LocalPart"))
                self.assertIsNone(self.doc.getObject("Pad"))
                mutated.append(True)
                raise RuntimeError("replacement rejected")
            return report

        with patch.object(AssemblyApp, "_extractionReferences", side_effect=fail):
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        self.assertKeptUnverified(raised.exception, "replacement rejected")
        self.assertIsInstance(raised.exception.__cause__, RuntimeError)
        self.assertEqual(mutated, [True])
        self.assertEqual(set(names), {obj.Name for obj in self.doc.Objects})
        self.assertEqual(other_link.LinkedObject, self.doc.LocalPart)
        self.assertEqual(sketch.ExternalGeometry, external)
        self.assertEqual(sketch.ConstraintCount, constraints)
        self.assertEqual(reader.ExpressionEngine, expressions)
        self.assertEqual(self.doc.Pad.Profile[0], self.doc.Profile)
        self.assertEqual(self.doc.Body.Tip, self.doc.Pad)
        self.assertIn(self.doc.LocalPart, self.assembly.Group)
        self.assertIn(self.doc.Body, self.doc.LocalPart.Group)
        self.assertIn(self.doc.Profile, self.doc.Body.Group)
        self.doc.recompute()
        for name, shape in geometry.items():
            self.assertShape(shape, self.doc.getObject(name).Shape)
        self.assertTrue(attachment.Placement.isSame(placement, 1e-7))
        self.assertFalse(self.doc.HasPendingTransaction)
        self.assertEqual(self.doc.getBookedTransactionID(), 0)
        self.assertEqual(self.doc.RedoCount, 0)
        self.assertEqual(self.doc.getFileChangeState(), file_state)
        after = {
            path.name: path.read_bytes() for path in Path(self.directory.name).iterdir()
        }
        self.assertEqual(set(after) - set(files), {os.path.basename(self.path)})
        for name, data in files.items():
            self.assertEqual(after[name], data)
        self.assertTrue(os.path.isfile(self.path))
        self.assertEqual(self.doc.UndoCount, 0)
        extra = set(App.listDocuments()) - self.before - {self.doc.Name}
        self.assertEqual(len(extra), 1)
        self.assertEqual(
            os.path.realpath(App.getDocument(extra.pop()).FileName),
            os.path.realpath(self.path),
        )
        self.assertEqual(App.ActiveDocument, self.doc)

    def test_settled_check_rejects_later_invalid_object(self):
        import ExtractComponent

        real = ExtractComponent._settled_observation

        def observe(obj):
            name, status, state, valid = real(obj)
            if name == "LocalPartLink":
                return name, "Invalid", set(state) | {"Invalid"}, False
            return name, status, state, valid

        with patch("ExtractComponent._settled_observation", side_effect=observe):
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        self.assertIn("LocalPartLink", str(raised.exception))
        self.assertIn("recompute", str(raised.exception).lower())

    def test_post_commit_recompute_failure_keeps_file_and_undo(self):
        import ExtractComponent

        self.doc.openTransaction("Rename component")
        self.part.Label = "Before extract"
        self.doc.commitTransaction()
        undos_before = list(self.doc.UndoNames)
        self.assertEqual(undos_before, ["Rename component"])
        self.assertEqual(list(self.doc.RedoNames), [])
        real = ExtractComponent._require_settled

        def fail_source(document, objects, message):
            if getattr(document, "Name", None) == self.doc.Name:
                raise ExtractionError("forced post-commit recompute failure")
            return real(document, objects, message)

        with patch("ExtractComponent._require_settled", side_effect=fail_source):
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        message = str(raised.exception)
        link = self.assertKeptCommit(
            message, undos_before, "forced post-commit recompute failure"
        )
        self.assertEqual(link.Label, "Before extract")

    def test_post_commit_visibility_failure_keeps_file_and_undo(self):
        import ExtractComponent

        self.doc.openTransaction("Rename component")
        self.part.Label = "Before extract"
        self.doc.commitTransaction()
        undos_before = list(self.doc.UndoNames)
        self.assertEqual(undos_before, ["Rename component"])
        self.assertEqual(list(self.doc.RedoNames), [])

        def fail_visibility(link, visible):
            raise ExtractionError("forced visibility restore failure")

        with patch(
            "ExtractComponent._restore_link_visibility", side_effect=fail_visibility
        ):
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        message = str(raised.exception)
        link = self.assertKeptCommit(
            message, undos_before, "forced visibility restore failure"
        )
        self.assertEqual(link.Label, "Before extract")

    def test_unverified_rollback_keeps_destination_and_file(self):
        import ExtractComponent

        real_save = ExtractComponent._save_destination
        names = [obj.Name for obj in self.doc.Objects]
        undos = list(self.doc.UndoNames)
        redos = list(self.doc.RedoNames)

        def save(destination, path):
            outcome = real_save(destination, path)
            self.assertTrue(outcome["success"])
            raise ExtractionError(
                "forced pre-commit failure",
                [("Reader", "Payload")],
            )

        with patch("ExtractComponent._save_destination", side_effect=save), patch(
            "ExtractComponent._source_restored", return_value=False
        ):
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        error = raised.exception
        message = str(error)
        self.assertIn(self.path, message)
        self.assertNotIn("rolled back", message)
        self.assertIn("could not be verified", message)
        self.assertIn("were kept", message)
        self.assertIn("forced pre-commit failure", message)
        self.assertIsInstance(error.__cause__, ExtractionError)
        self.assertEqual(str(error.__cause__), "forced pre-commit failure")
        self.assertEqual(error.__cause__.leftover, (("Reader", "Payload"),))
        self.assertEqual(error.leftover, (("Reader", "Payload"),))
        self.assertEqual(names, [obj.Name for obj in self.doc.Objects])
        self.assertEqual(list(self.doc.UndoNames), undos)
        self.assertEqual(list(self.doc.RedoNames), redos)
        self.assertTrue(os.path.isfile(self.path))
        kept = [
            doc
            for doc in App.listDocuments().values()
            if os.path.realpath(doc.FileName or "") == os.path.realpath(self.path)
        ]
        self.assertEqual(len(kept), 1)
        self.assertIn(kept[0].Name, App.listDocuments())

    def test_rollback_failed_keeps_destination_and_file(self):
        import ExtractComponent

        class FakeDocument:
            Name = "FakeSource"

            def commitCompatibilityMutationAsync(self, callback, **kwargs):
                return {
                    "wait": lambda: {
                        "status": "RollbackFailed",
                        "message": "native rollback failed",
                    }
                }

        with self.assertRaises(ExtractionError) as direct:
            ExtractComponent._commit_on_owner(
                FakeDocument(), lambda: None, pump=self.doc
            )
        self.assertEqual(direct.exception.commit_status, "RollbackFailed")
        self.assertIn("native rollback failed", str(direct.exception))

        # Running nothing of the callback: names and links look restored.
        with self.patchSourceCommit("RollbackFailed"):
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        message = self.assertKeptUnverified(raised.exception, "RollbackFailed")
        self.assertNotIn("rolled back", message)
        self.assertEqual(raised.exception.__cause__.commit_status, "RollbackFailed")

    def test_clean_rollback_with_placement_expression_keeps_file(self):
        reader = self.doc.addObject("App::FeaturePython", "ExprReader")
        reader.addProperty("App::PropertyPlacement", "Placement")
        self.doc.recompute()

        def plant():
            destination = next(
                doc
                for doc in App.listDocuments().values()
                if doc.Name != self.doc.Name and doc.Name not in self.before
            )
            reader.setExpression("Placement", f"{destination.Name}#Pad.Placement")

        with self.patchSourceCommit("ApplyFailed", plant):
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        self.assertKeptUnverified(raised.exception, "ApplyFailed")
        self.assertIn("#", reader.ExpressionEngine[0][1])
        self.assertIn("Pad.Placement", reader.ExpressionEngine[0][1])

    def test_clean_rollback_with_hidden_xlink_keeps_file(self):
        reader = self.doc.addObject("App::FeaturePython", "HiddenReader")
        reader.addProperty("App::PropertyXLinkSubHidden", "Reference")
        self.assertEqual(
            reader.getTypeIdOfProperty("Reference"), "App::PropertyXLinkSubHidden"
        )
        self.doc.recompute()
        planted = {}

        def plant():
            destination = self._destination_saved_here()
            target = destination.getObject("Pad")
            self.assertIsNotNone(target)
            names = [obj.Name for obj in self.doc.Objects]
            reader.Reference = (target, [""])
            planted["target"] = target
            planted["dest_name"] = destination.Name
            planted["names_same"] = [obj.Name for obj in self.doc.Objects] == names
            planted["out_docs"] = []
            for item in reader.OutList:
                planted["out_docs"].append(getattr(item.Document, "Name", None))
            planted["engine"] = [
                entry[1] if isinstance(entry, (tuple, list)) and len(entry) > 1 else entry
                for entry in (reader.ExpressionEngine or ())
            ]

        with self.patchSourceCommit("ApplyFailed", plant):
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        self.assertTrue(planted["names_same"])
        self.assertNotIn(planted["dest_name"], planted["out_docs"])
        self.assertFalse(
            any(
                isinstance(text, str) and f"{planted['dest_name']}#" in text
                for text in planted["engine"]
            )
        )
        self.assertKeptUnverified(raised.exception, "ApplyFailed")
        target = planted["target"]
        linked = reader.Reference[0]
        self.assertEqual(linked.Name, target.Name)
        self.assertEqual(linked.Document.Name, target.Document.Name)
        self.assertEqual(
            os.path.realpath(linked.Document.FileName), os.path.realpath(self.path)
        )

    def test_clean_rollback_with_sheet_hiddenref_keeps_file(self):
        import ExtractComponent

        sheet = self.doc.addObject("Spreadsheet::Sheet", "HiddenSheet")
        self.doc.recompute()
        planted = {}

        def plant():
            destination = self._destination_saved_here()
            target = destination.getObject("Pad")
            self.assertIsNotNone(target)
            names = [obj.Name for obj in self.doc.Objects]
            sheet.set("A1", f"=hiddenref({destination.Name}#{target.Name}.Length)")
            text = sheet.getContents("A1")
            planted["text"] = text
            planted["names_doc"] = ExtractComponent._expression_names_document(
                text, destination
            )
            planted["names_same"] = [obj.Name for obj in self.doc.Objects] == names
            planted["out_docs"] = []
            for item in sheet.OutList:
                planted["out_docs"].append(getattr(item.Document, "Name", None))
            planted["engine"] = [
                entry[1] if isinstance(entry, (tuple, list)) and len(entry) > 1 else entry
                for entry in (sheet.ExpressionEngine or ())
            ]
            planted["dest_name"] = destination.Name

        with self.patchSourceCommit("ApplyFailed", plant):
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        self.assertTrue(planted["names_doc"], planted["text"])
        self.assertTrue(planted["names_same"])
        self.assertNotIn(planted["dest_name"], planted["out_docs"])
        self.assertFalse(
            any(
                isinstance(text, str) and f"{planted['dest_name']}#" in text
                for text in planted["engine"]
            )
        )
        self.assertIn("hiddenref", planted["text"])
        self.assertIn(f"{planted['dest_name']}#", planted["text"])
        self.assertKeptUnverified(raised.exception, "ApplyFailed")

    def test_clean_rollback_with_escaped_document_label_keeps_file(self):
        import ExtractComponent

        sheet = self.doc.addObject("Spreadsheet::Sheet", "EscapedSheet")
        self.doc.recompute()
        planted = {}

        def plant():
            destination = self._destination_saved_here()
            target = destination.getObject("Pad")
            self.assertIsNotNone(target)
            names = [obj.Name for obj in self.doc.Objects]
            # App::unquote drops "\>", so set() of the escaped token does not
            # round-trip. Relabel a real label reference; getContents is then
            # the spelling App::quote writes (ObjectIdentifier.cpp:54).
            destination.Label = "ExtPart"
            sheet.set("A1", f"=hiddenref(<<ExtPart>>#{target.Name}.Length)")
            destination.Label = "Ext>Part"
            text = sheet.getContents("A1")
            planted["text"] = text
            planted["label"] = destination.Label
            planted["prop"] = f"{target.Name}.Length"
            planted["names_same"] = [obj.Name for obj in self.doc.Objects] == names
            planted["out_docs"] = []
            for item in sheet.OutList:
                planted["out_docs"].append(getattr(item.Document, "Name", None))
            planted["engine"] = [
                entry[1] if isinstance(entry, (tuple, list)) and len(entry) > 1 else entry
                for entry in (sheet.ExpressionEngine or ())
            ]
            planted["dest_name"] = destination.Name

        with self.patchSourceCommit("ApplyFailed", plant):
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        self.assertTrue(planted["names_same"])
        self.assertEqual(planted["label"], "Ext>Part")
        text = planted["text"]
        self.assertIn(f"=hiddenref(<<Ext\\>Part>>#{planted['prop']})", text)
        self.assertNotIn(f"{planted['dest_name']}#", text)
        self.assertNotIn(planted["dest_name"], planted["out_docs"])
        self.assertFalse(
            any(
                isinstance(item, str) and f"{planted['dest_name']}#" in item
                for item in planted["engine"]
            )
        )
        self.assertKeptUnverified(raised.exception, "ApplyFailed")
        kept = [
            doc
            for doc in App.listDocuments().values()
            if os.path.realpath(doc.FileName or "") == os.path.realpath(self.path)
        ]
        self.assertEqual(kept[0].Label, "Ext>Part")
        self.assertTrue(ExtractComponent._expression_names_document(text, kept[0]), text)

    def test_clean_rollback_with_literal_sheet_text_discards_file(self):
        sheet = self.doc.addObject("Spreadsheet::Sheet", "LiteralSheet")
        self.doc.recompute()
        planted = {}

        def plant():
            destination = self._destination_saved_here()
            names = [obj.Name for obj in self.doc.Objects]
            literal = f"{destination.Name}#Pad.Length"
            sheet.set("A1", literal)
            planted["text"] = sheet.getContents("A1")
            planted["literal"] = literal
            planted["names_same"] = [obj.Name for obj in self.doc.Objects] == names
            planted["dest_name"] = destination.Name
            planted["out_docs"] = []
            for item in sheet.OutList:
                planted["out_docs"].append(getattr(item.Document, "Name", None))
            planted["engine"] = [
                entry[1] if isinstance(entry, (tuple, list)) and len(entry) > 1 else entry
                for entry in (sheet.ExpressionEngine or ())
            ]

        with self.patchSourceCommit("ApplyFailed", plant):
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        self.assertTrue(planted["names_same"])
        text = planted["text"]
        self.assertTrue(text.startswith("'"), text)
        self.assertFalse(text.startswith("="))
        self.assertIn(planted["literal"], text)
        self.assertIn(f"{planted['dest_name']}#", text)
        self.assertNotIn(planted["dest_name"], planted["out_docs"])
        self.assertFalse(
            any(
                isinstance(item, str) and f"{planted['dest_name']}#" in item
                for item in planted["engine"]
            )
        )
        message = str(raised.exception)
        self.assertFalse(os.path.isfile(self.path), message)
        self.assertNotIn("could not be verified", message)
        self.assertIsNotNone(self.doc.getObject("LocalPart"))
        kept = [
            doc
            for doc in App.listDocuments().values()
            if os.path.realpath(doc.FileName or "") == os.path.realpath(self.path)
        ]
        self.assertEqual(kept, [])

    def test_existing_extraction_group_allows_cleanup(self):
        import ExtractComponent

        other = self.doc.addObject("App::Link", "OtherLink")
        other.LinkedObject = self.pad
        self.doc.recompute()
        self.runExtract()
        self.assertIsNotNone(self.doc.getObject("ExtractionReferences"))
        second = self.assembly.newObject("Part::Box", "SecondBox")
        self.doc.recompute()
        second_path = os.path.join(self.directory.name, "Second.FCStd")
        real = ExtractComponent._foreign_inbound_leftovers
        calls = {"n": 0}

        def inbound(source, owned, component):
            calls["n"] += 1
            if calls["n"] >= 2:
                raise ExtractionError("forced second failure")
            return real(source, owned, component)

        with patch(
            "ExtractComponent._foreign_inbound_leftovers", side_effect=inbound
        ):
            with self.assertRaises(ExtractionError) as raised:
                extract(self.assembly, second, second_path)
        self.assertEqual(str(raised.exception), "forced second failure")
        self.assertGreaterEqual(calls["n"], 2)
        self.assertFalse(os.path.exists(second_path))
        self.assertTrue(os.path.isfile(self.path))
        self.assertIsNotNone(self.doc.getObject("ExtractionReferences"))
        self.assertIsNotNone(self.doc.getObject("SecondBox"))
        kept = [
            doc
            for doc in App.listDocuments().values()
            if os.path.realpath(doc.FileName or "") == os.path.realpath(second_path)
        ]
        self.assertEqual(kept, [])

    def test_refuse_class_masquerade_hiding_owned_object(self):
        class Masquerade:
            def __init__(self, hidden):
                self.hidden = hidden

            @property
            def __class__(self):
                return App.DocumentObject

            def dumps(self):
                return {}

        reader = self.doc.addObject("App::FeaturePython", "MasqueradeReader")
        reader.addProperty("App::PropertyPythonObject", "Payload")
        reader.Payload = Masquerade(self.part)
        error = self.assertRefused()
        self.assertIn((reader.Name, "Payload"), error.leftover)
        self.assertFalse(os.path.exists(self.path))
        self.assertIs(reader.Payload.hidden, self.part)

    def test_refuse_dumps_descriptor_without_calling_it(self):
        calls = []

        class State:
            @property
            def dumps(self):
                calls.append(True)
                return {}

        reader = self.doc.addObject("App::FeaturePython", "DescriptorReader")
        reader.addProperty("App::PropertyPythonObject", "Payload")
        reader.Payload = State()
        calls.clear()
        error = self.assertRefused()
        self.assertIn((reader.Name, "Payload"), error.leftover)
        self.assertEqual(calls, [])
        self.assertFalse(os.path.exists(self.path))

    def test_refuse_instance_dumps_shadowing_getstate(self):
        import ExtractComponent

        class State:
            def __getstate__(self):
                return {}

        hidden = State()
        # Save calls this instance dumps via hasAttr/getAttr. It is not a class
        # function, so a clean __getstate__ must not be trusted in its place.
        hidden.dumps = {"untrusted": True}
        reader = self.doc.addObject("App::FeaturePython", "InstanceDumps")
        reader.addProperty("App::PropertyPythonObject", "Payload")
        reader.Payload = hidden
        error = self.assertRefused()
        self.assertIn((reader.Name, "Payload"), error.leftover)
        self.assertFalse(os.path.exists(self.path))

        calls = []

        def dumps():
            calls.append(True)
            return {"component": self.part}

        exposed = State()
        exposed.dumps = dumps
        self.assertIs(
            ExtractComponent._dumps_function(exposed),
            ExtractComponent._UNSAFE_SERIALIZER,
        )
        self.assertEqual(calls, [])

    def test_refuse_getattr_dumps(self):
        owned = self.part
        calls = []

        class State:
            def __getattr__(self, name):
                calls.append(name)
                if name == "dumps":
                    def dumps():
                        return {"component": owned}

                    return dumps
                raise AttributeError(name)

            def __getstate__(self):
                return {}

        reader = self.doc.addObject("App::FeaturePython", "GetattrDumps")
        reader.addProperty("App::PropertyPythonObject", "Payload")
        reader.Payload = State()
        calls.clear()
        error = self.assertRefused()
        self.assertIn((reader.Name, "Payload"), error.leftover)
        self.assertNotIn("dumps", calls)
        self.assertFalse(os.path.exists(self.path))

    def test_refuse_getattribute_dumps(self):
        owned = self.part
        calls = []

        class State:
            def __getattribute__(self, name):
                if name == "dumps":
                    calls.append(name)

                    def dumps():
                        return {"component": owned}

                    return dumps
                return object.__getattribute__(self, name)

            def __getstate__(self):
                return {}

        reader = self.doc.addObject("App::FeaturePython", "GetattributeDumps")
        reader.addProperty("App::PropertyPythonObject", "Payload")
        reader.Payload = State()
        calls.clear()
        error = self.assertRefused()
        self.assertIn((reader.Name, "Payload"), error.leftover)
        self.assertNotIn("dumps", calls)
        self.assertFalse(os.path.exists(self.path))

    def _assert_other_document_serializer_refused(
        self, document_name, payload, lookups, dump_calls
    ):
        import ExtractComponent

        other = App.newDocument(document_name)
        reader = other.addObject("App::FeaturePython", "Reader")
        reader.addProperty("App::PropertyPythonObject", "Payload")
        reader.Payload = payload
        lookups.clear()
        dump_calls.clear()
        stored = reader.Payload
        with self.subTest("serializer"):
            self.assertIs(
                ExtractComponent._dumps_function(stored),
                ExtractComponent._UNSAFE_SERIALIZER,
            )
            self.assertEqual(dump_calls, [])
            self.assertNotIn("dumps", lookups)
        with self.subTest("extract"):
            lookups.clear()
            dump_calls.clear()
            documents = set(App.listDocuments())
            error = self.assertRefused()
            self.assertIn((reader.FullName, "Payload"), error.leftover)
            self.assertEqual(dump_calls, [])
            self.assertNotIn("dumps", lookups)
            self.assertFalse(os.path.exists(self.path))
            self.assertEqual(documents, set(App.listDocuments()))

    def test_refuse_callable_instance_getattribute_dumps(self):
        owned = self.part
        lookups = []
        dump_calls = []

        class Answer:
            def __call__(self, name):
                lookups.append(name)
                if name == "dumps":

                    def dumps():
                        dump_calls.append(True)
                        return {"component": owned}

                    return dumps
                if name == "__dict__":
                    return {}
                raise AttributeError(name)

        class State:
            # Live storage stays empty. Native save calls this as f(name) and
            # would serialize the owned object; plain lookup would not see it.
            __getattribute__ = Answer()

        self._assert_other_document_serializer_refused(
            "CallableGetattribute", State(), lookups, dump_calls
        )

    def test_refuse_staticmethod_getattribute_dumps(self):
        owned = self.part
        lookups = []
        dump_calls = []

        class State:
            @staticmethod
            def __getattribute__(name):
                lookups.append(name)
                if name == "dumps":

                    def dumps():
                        dump_calls.append(True)
                        return {"component": owned}

                    return dumps
                if name == "__dict__":
                    return {}
                raise AttributeError(name)

        self._assert_other_document_serializer_refused(
            "StaticGetattribute", State(), lookups, dump_calls
        )

    def test_plain_python_values_are_not_unsafe_serializers(self):
        import ExtractComponent

        for sample in (None, "text", 1, 1.5, [1, "a"], {"n": 1, "s": "a"}, (1, "a")):
            self.assertIsNone(
                ExtractComponent._dumps_function(sample),
                repr(sample),
            )

        class Proxy:
            def __init__(self):
                self.label = "plain"

        self.assertIsNone(ExtractComponent._dumps_function(Proxy()))

        other = App.newDocument("PlainPythonState")
        reader = other.addObject("App::FeaturePython", "PlainReader")
        reader.addProperty("App::PropertyPythonObject", "AsDict")
        reader.addProperty("App::PropertyPythonObject", "AsList")
        reader.AsDict = {"count": 2, "label": "kept"}
        reader.AsList = [1, 2, "a", "b"]
        self.runExtract()
        self.assertEqual(reader.AsDict, {"count": 2, "label": "kept"})
        self.assertEqual(reader.AsList, [1, 2, "a", "b"])
        self.assertTrue(os.path.isfile(self.path))
        self.assertIsNone(self.doc.getObject("LocalPart"))

    def test_property_getstate_blocks_without_calling(self):
        import ExtractComponent

        calls = []
        owned = self.part

        class State:
            @property
            def __getstate__(self):
                calls.append(len(calls))
                if len(calls) < 3:
                    return {"n": 1}
                return {"component": owned}

        value = State()
        value.n = 1
        calls.clear()
        self.assertTrue(ExtractComponent._python_state_blocks(value, [owned]))
        self.assertEqual(calls, [])

    def test_function_getstate_follows_its_return_value(self):
        import ExtractComponent

        owned = self.part

        class OwnedState:
            def __getstate__(self):
                return {"component": owned}

        class PlainState:
            def __getstate__(self):
                return {"n": 1, "label": "plain"}

        self.assertTrue(ExtractComponent._python_state_blocks(OwnedState(), [owned]))
        self.assertFalse(ExtractComponent._python_state_blocks(PlainState(), [owned]))

    def test_values_without_getstate_do_not_block(self):
        import ExtractComponent

        class Proxy:
            def __init__(self):
                self.n = 1
                self.label = "plain"

        for value in ({"n": 1, "label": "a"}, [1, "a"], Proxy()):
            self.assertFalse(
                ExtractComponent._python_state_blocks(value, [self.part]),
                repr(value),
            )

    def test_object_getattribute_assignment_is_not_c_storage(self):
        import ExtractComponent

        class Proxy:
            __getattribute__ = object.__getattribute__

            def __init__(self):
                self.n = 1
                self.label = "plain"

        value = Proxy()
        self.assertFalse(ExtractComponent._containsOwned(value, [self.part]))
        self.assertIsNone(ExtractComponent._dumps_function(value))

    def test_metaclass_dumps_is_invisible_to_instance_lookup(self):
        import ExtractComponent

        calls = []
        owned = self.part

        class Meta(type):
            # Instance lookup never sees a metaclass method. The raw function
            # is what a class-dict walk would call as dumps(value).
            def dumps(value):
                calls.append(value)
                return {"component": owned}

        class State(metaclass=Meta):
            def __init__(self):
                self.text = "plain"
                self.count = 3

        value = State()
        with self.subTest("function"):
            self.assertIsNone(ExtractComponent._dumps_function(value))
        with self.subTest("blocks"):
            self.assertFalse(ExtractComponent._python_state_blocks(value, [owned]))
        with self.subTest("calls"):
            self.assertEqual(calls, [])

    def test_metaclass_dict_property_cannot_hide_owned_object(self):
        import ExtractComponent

        class Meta(type):
            __dict__ = property(lambda cls: {"__slots__": ()})

        class State(metaclass=Meta):
            pass

        value = State()
        value.hidden = self.part
        self.assertTrue(ExtractComponent._containsOwned(value, [self.part]))

    def test_dict_subclass_instance_dumps_is_unsafe(self):
        import ExtractComponent

        called = []

        class LyingDict(dict):
            def __contains__(self, key):
                return False

        class State:
            def dumps(self):
                return {"text": "class"}

        def evil():
            called.append(True)
            return {"hidden": True}

        value = State()
        value.__dict__ = LyingDict({"dumps": evil, "text": "shadow"})
        self.assertIs(
            ExtractComponent._dumps_function(value),
            ExtractComponent._UNSAFE_SERIALIZER,
        )
        self.assertEqual(called, [])

    def test_plain_class_dumps_allows_extraction(self):
        class Note:
            def __init__(self):
                self.text = "kept"

            def dumps(self):
                return {"text": self.text}

        reader = self.doc.addObject("App::FeaturePython", "NoteReader")
        reader.addProperty("App::PropertyPythonObject", "Payload")
        reader.Payload = Note()
        self.runExtract()
        self.assertEqual(reader.Payload.dumps(), {"text": "kept"})
        self.assertTrue(os.path.isfile(self.path))

    def test_refuse_over_budget_python_state(self):
        from ExtractComponent import _MAX_PYTHON_STATE_NODES

        reader = self.doc.addObject("App::FeaturePython", "WideReader")
        reader.addProperty("App::PropertyPythonObject", "Payload")
        reader.Payload = list(range(_MAX_PYTHON_STATE_NODES + 1))
        error = self.assertRefused()
        self.assertIn((reader.Name, "Payload"), error.leftover)
        self.assertFalse(os.path.exists(self.path))

    def test_refuse_inbound_link_gained_after_preflight(self):
        import ExtractComponent

        other = App.newDocument("OtherDocument")
        other.saveAs(os.path.join(self.directory.name, "Other.FCStd"))
        part = self.part
        added = {}
        real_save = ExtractComponent._save_destination

        def save(destination, path):
            link = other.addObject("App::Link", "LateLink")
            link.LinkedObject = part
            added["link"] = link
            return real_save(destination, path)

        names = [obj.Name for obj in self.doc.Objects]
        with patch("ExtractComponent._save_destination", side_effect=save):
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        link = added["link"]
        self.assertEqual(link.LinkedObject, self.doc.LocalPart)
        self.assertTrue(
            any(
                "LateLink" in owner and prop == "LinkedObject"
                for owner, prop in raised.exception.leftover
            )
        )
        self.assertEqual(names, [obj.Name for obj in self.doc.Objects])
        self.assertIsNone(self.doc.getObject("LocalPartLink"))
        self.assertIsNone(self.doc.getObject("ExtractionReferences"))
        self.assertIn(self.doc.LocalPart, self.assembly.Group)
        self.assertFalse(os.path.exists(self.path))
        self.assertEqual(self.doc.RedoCount, 0)

    def test_list_documents_calls_are_on_the_gui_thread(self):
        import AssemblyApp
        import ExtractComponent
        import traceback

        caller = threading.get_ident()
        # With GUI hooks this is true only on the GUI thread. Without them it
        # is false on every thread, so the caller identity is what still
        # rejects an owner-thread App.listDocuments.
        caller_gui = bool(AssemblyApp._isGuiThread())
        recorded = []
        real = ExtractComponent.App.listDocuments

        def wrapped(*args, **kwargs):
            recorded.append(
                (
                    threading.get_ident(),
                    bool(AssemblyApp._isGuiThread()),
                    "".join(traceback.format_stack(limit=12)),
                )
            )
            return real(*args, **kwargs)

        with patch.object(ExtractComponent.App, "listDocuments", side_effect=wrapped):
            self.runExtract()
        self.assertTrue(recorded)
        off_thread = [
            stack
            for ident, gui, stack in recorded
            if ident != caller or gui != caller_gui
        ]
        self.assertEqual(off_thread, [])
        if caller_gui:
            self.assertTrue(all(gui for _, gui, _ in recorded))

    def test_owner_refuses_inbound_link_after_gui_recheck(self):
        import ExtractComponent

        other = App.newDocument("AfterRecheckDocument")
        other.saveAs(os.path.join(self.directory.name, "AfterRecheck.FCStd"))
        part = self.part
        added = {}
        calls = {"n": 0}
        caller = threading.get_ident()
        real = ExtractComponent._foreign_inbound_leftovers
        names = [obj.Name for obj in self.doc.Objects]
        undos = list(self.doc.UndoNames)
        redos = list(self.doc.RedoNames)

        def wrapped(source, owned, component):
            calls["n"] += 1
            found = real(source, owned, component)
            if (
                calls["n"] == 2
                and threading.get_ident() == caller
                and not found
                and "link" not in added
            ):
                link = other.addObject("App::Link", "AfterRecheckLink")
                link.LinkedObject = part
                added["link"] = link
            return found

        with patch("ExtractComponent._foreign_inbound_leftovers", side_effect=wrapped):
            with self.assertRaises(ExtractionError) as raised:
                self.runExtract()
        link = added["link"]
        self.assertEqual(calls["n"], 2)
        self.assertEqual(link.LinkedObject, self.doc.LocalPart)
        self.assertTrue(
            any("AfterRecheckLink" in str(entry) for entry in raised.exception.leftover)
        )
        self.assertIn("AfterRecheckLink", str(raised.exception))
        self.assertEqual(names, [obj.Name for obj in self.doc.Objects])
        self.assertEqual(list(self.doc.UndoNames), undos)
        self.assertEqual(list(self.doc.RedoNames), redos)
        self.assertIsNone(self.doc.getObject("LocalPartLink"))
        self.assertIsNone(self.doc.getObject("ExtractionReferences"))
        self.assertIn(self.doc.LocalPart, self.assembly.Group)
        # The refusal is raised from inside the source callback. The binding
        # re-raises that exception and drops the ApplyFailed status, so the
        # rolled-back destination cannot be told from a failed rollback.
        message = str(raised.exception)
        self.assertTrue(os.path.isfile(self.path), message)
        self.assertIn(self.path, message)
        self.assertIn("could not be verified", message)
        self.assertIn("were kept", message)
        kept = [
            document
            for document in App.listDocuments().values()
            if os.path.realpath(getattr(document, "FileName", "") or "")
            == os.path.realpath(self.path)
        ]
        self.assertEqual(len(kept), 1)
        self.assertEqual(self.doc.UndoCount, len(undos))
        self.assertEqual(self.doc.RedoCount, len(redos))

    def test_read_only_preflight(self):
        names = [obj.Name for obj in self.doc.Objects]
        before_docs = set(App.listDocuments())
        backlinks = {
            obj.Name: tuple(x.Name for x in obj.InList) for obj in self.doc.Objects
        }
        preflight(self.assembly, self.part, self.path)
        self.assertEqual(names, [obj.Name for obj in self.doc.Objects])
        self.assertEqual(before_docs, set(App.listDocuments()))
        self.assertEqual(
            backlinks,
            {obj.Name: tuple(x.Name for x in obj.InList) for obj in self.doc.Objects},
        )
        self.assertFalse(os.path.exists(self.path))


@unittest.skipUnless(App.GuiUp, "requires the FreeCAD GUI")
class TestExtractComponentGui(TestExtractComponent):
    """Exercise the same persistence/reference cases with actual view providers."""

    def setUp(self):
        super().setUp()
        import FreeCADGui as Gui

        Gui.activeDocument().setEdit(self.assembly.Name)

    def _rgb(self, color):
        channels = tuple(float(channel) for channel in color)
        return channels[:3]

    def _assert_color_close(self, actual, expected):
        # View materials round through an 8-bit channel on the way into the
        # copied document and the Coin scene.
        got = self._rgb(actual)
        want = self._rgb(expected)
        self.assertEqual(len(got), 3)
        for channel, target in zip(got, want):
            self.assertLess(abs(channel - target), 1.0 / 255.0 + 1e-4, (got, want))

    def _assert_colors_close(self, actual, expected):
        self.assertEqual(len(actual), len(expected))
        for item, target in zip(actual, expected):
            self._assert_color_close(item, target)

    def _scene_diffuse_colors(self, view):
        from pivy import coin

        action = coin.SoSearchAction()
        action.setType(coin.SoMaterial.getClassTypeId())
        action.setInterest(coin.SoSearchAction.ALL)
        action.apply(view.RootNode)
        paths = action.getPaths()
        found = []
        for index in range(paths.getLength()):
            material = paths.get(index).getTail()
            found.extend(self._rgb(color.getValue()) for color in material.diffuseColor.getValues())
        return found

    def _assert_link_appearance(self, link, pad, faces, color, transparency):
        self.flushGui()
        self.assertFalse(link.ViewObject.OverrideMaterial)
        self.assertIsNot(link.ViewObject, pad.ViewObject)
        self._assert_color_close(pad.ViewObject.ShapeColor, color)
        self.assertEqual(pad.ViewObject.Transparency, transparency)
        self._assert_colors_close(pad.ViewObject.DiffuseColor, faces)
        self._assert_colors_close(
            [item.DiffuseColor for item in pad.ViewObject.ShapeAppearance], faces
        )
        rendered = self._scene_diffuse_colors(link.ViewObject)
        missing = []
        for face in faces:
            if not any(
                all(abs(channel - target) < 1.0 / 255.0 + 1e-4 for channel, target in zip(color, face))
                for color in rendered
            ):
                missing.append(face)
        self.assertEqual(missing, [], rendered)

    def test_appearance(self):
        from Test.GuiRecompute import close_document_by_name, redo_document, save_document, undo_document

        self.pad.ViewObject.ShapeColor = (0.2, 0.6, 0.8)
        face_count = len(self.pad.Shape.Faces)
        self.pad.ViewObject.DiffuseColor = [
            ((index + 1) / (face_count + 1), 0.15, 0.55, 1.0) for index in range(face_count)
        ]
        self.pad.ViewObject.Transparency = 35
        color = self.pad.ViewObject.ShapeColor
        transparency = self.pad.ViewObject.Transparency
        faces = [self._rgb(item) for item in self.pad.ViewObject.DiffuseColor]
        self.assertEqual(len(faces), face_count)
        self.assertEqual(len(set(tuple(round(channel, 5) for channel in face) for face in faces)), face_count)
        result = self.runExtract()
        self._assert_link_appearance(result.link, result.document.Pad, faces, color, transparency)

        undo_document(self.doc)
        self.flushGui()
        self.assertIsNotNone(self.doc.getObject("LocalPart"))
        self.assertIsNone(self.doc.getObject("LocalPartLink"))
        self._assert_colors_close(self.doc.Pad.ViewObject.DiffuseColor, faces)
        self.assertEqual(self.doc.Pad.ViewObject.Transparency, transparency)
        redo_document(self.doc)
        self.flushGui()
        link = self.doc.getObject("LocalPartLink")
        self.assertIsNotNone(link)
        self._assert_link_appearance(link, result.document.Pad, faces, color, transparency)

        save_document(self.doc)
        save_document(result.document)
        source_path = self.doc.FileName
        destination_path = result.document.FileName
        assembly_name = self.assembly.Name
        close_document_by_name(self.doc.Name)
        close_document_by_name(result.document.Name)
        reopened_source = App.openDocument(source_path)
        reopened_destination = App.openDocument(destination_path)
        self.flushGui()
        assembly = reopened_source.getObject(assembly_name)
        reopened_link = next(obj for obj in assembly.Group if obj.TypeId == "App::Link")
        self._assert_link_appearance(
            reopened_link, reopened_destination.Pad, faces, color, transparency
        )

    def _selection_roots(self, view):
        # Importing pivy loads the Coin SWIG wrappers getSceneGraph() requires.
        from pivy import coin  # noqa: F401

        found = set()
        seen = set()
        pending = [view.getSceneGraph()]
        while pending:
            node = pending.pop()
            # The SWIG pointer is the C++ address; getNodeId() changes on edits.
            address = int(node.this)
            if address in seen:
                continue
            seen.add(address)
            if node.getTypeId().getName() == "SoFCSelectionRoot":
                found.add(address)
            if hasattr(node, "getNumChildren"):
                pending.extend(node.getChild(index) for index in range(node.getNumChildren()))
        return found

    def test_undo_restores_original_view_providers(self):
        import FreeCADGui as Gui
        from Test.GuiRecompute import redo_document, undo_document

        gui_document = Gui.getDocument(self.doc.Name)
        view = gui_document.mdiViewsOfType("Gui::View3DInventor")[0]
        names = ("LocalPart", "Body", "Profile", "Pad")
        providers = {name: gui_document.getObject(name) for name in names}
        self.assertNotIn(None, providers.values())
        before = self._selection_roots(view)
        self.runExtract()
        self.flushGui()
        extracted = len(self._selection_roots(view))

        # Extraction removes the component, its children and their origins in
        # one commit. Undo must restore the original providers and nothing
        # else: a provider recorded after its object made undo create a
        # duplicate for every removed object, whose roots stayed in the scene.
        for _ in range(2):
            undo_document(self.doc)
            self.flushGui()
            for name in names:
                self.assertIsNotNone(self.doc.getObject(name), name)
                self.assertIs(gui_document.getObject(name), providers[name], name)
            self.assertEqual(self._selection_roots(view), before)
            redo_document(self.doc)
            self.flushGui()
            self.assertIsNone(self.doc.getObject("LocalPart"))
            self.assertEqual(len(self._selection_roots(view)), extracted)

    def test_command_and_tree_context(self):
        import FreeCADGui as Gui
        import CommandExtractComponent as command

        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.part)
        self.assertTrue(command.CommandExtractComponent().IsActive())
        workbench = Gui.activeWorkbench()
        from PySide import QtWidgets

        self.assertTrue(
            any(
                "Assembly_ExtractComponent" in commands
                for commands in workbench.getToolbarItems().values()
            )
        )
        self.assertTrue(
            any(
                action.objectName() == "Assembly_ExtractComponent"
                for menu in Gui.getMainWindow().findChildren(QtWidgets.QMenu)
                for action in menu.actions()
            )
        )
        menus = []
        with patch.object(
            workbench,
            "appendContextMenu",
            side_effect=lambda title, cmds: menus.extend(cmds),
        ):
            workbench.ContextMenu("Tree")
        self.assertIn("Assembly_ExtractComponent", menus)
        with patch.object(command, "extractWithDialog") as dialog:
            Gui.runCommand("Assembly_ExtractComponent")
            dialog.assert_called_once_with(self.assembly, self.part)

        with patch.object(
            QtWidgets.QFileDialog, "getSaveFileName", return_value=(self.path, "")
        ), patch.object(
            QtWidgets.QMessageBox,
            "warning",
            side_effect=AssertionError("unexpected extraction refusal"),
        ):
            Gui.runCommand("Assembly_ExtractComponent")
        self.assertIsNone(self.doc.getObject("LocalPart"))
        self.assertTrue(os.path.isfile(self.path))
        self.doc.undo()
        self.assertIsNotNone(self.doc.getObject("LocalPart"))

    def test_component_from_face_selection(self):
        import FreeCADGui as Gui
        import CommandExtractComponent as command

        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(
            self.doc.Name, self.assembly.Name, "LocalPart.Body.Pad.Face1"
        )
        self.assertEqual(command.selectedComponent(), self.part)
        self.assertTrue(command.CommandExtractComponent().IsActive())

    def test_dialog_default_and_cancel(self):
        import CommandExtractComponent as command
        from PySide import QtWidgets

        with patch.object(
            QtWidgets.QFileDialog, "getSaveFileName", return_value=("", "")
        ) as dialog:
            command.extractWithDialog(self.assembly, self.part)
            default_path = dialog.call_args[0][2]
            self.assertEqual(
                default_path, os.path.join(self.directory.name, "Component label.FCStd")
            )
        self.assertIsNotNone(self.doc.getObject("LocalPart"))
        self.assertFalse(os.path.exists(self.path))

    def test_dialog_unsaved_cancel(self):
        import CommandExtractComponent as command
        from PySide import QtWidgets

        self.doc.FileName = ""
        names = [obj.Name for obj in self.doc.Objects]
        with patch.object(
            QtWidgets.QMessageBox, "warning", return_value=QtWidgets.QMessageBox.Cancel
        ), patch.object(QtWidgets.QFileDialog, "getSaveFileName") as dialog:
            self.assertIsNone(command.extractWithDialog(self.assembly, self.part))
            dialog.assert_not_called()
        self.assertEqual(names, [obj.Name for obj in self.doc.Objects])
        self.assertFalse(os.path.exists(self.path))

    def test_dialog_unsaved_save_then_extract(self):
        import CommandExtractComponent as command
        from PySide import QtCore, QtWidgets

        self.doc.FileName = ""
        self.source_path = os.path.join(self.directory.name, "SavedAssembly.FCStd")

        from Test.GuiRecompute import _pump_gui_until

        # Also drive the native GUI Save As dialog on the pre-fix path. Its
        # real asynchronous save succeeds, but that path loses the continuation.
        native_dialog = QtCore.QTimer()

        def accept_native_save():
            for widget in QtWidgets.QApplication.topLevelWidgets():
                if isinstance(widget, QtWidgets.QFileDialog) and widget.isVisible():
                    widget.selectFile(self.source_path)
                    widget.accept()

        native_dialog.timeout.connect(accept_native_save)
        native_dialog.start(10)
        self.addCleanup(native_dialog.stop)

        with patch.object(
            QtWidgets.QMessageBox, "warning", return_value=QtWidgets.QMessageBox.Save
        ) as warning, patch.object(
            QtWidgets.QFileDialog, "getSaveFileName",
            side_effect=[(self.source_path, ""), (self.path, "")],
        ):
            # The real save command runs on the lane; no mocked saveAs return.
            self.assertIsNone(command.extractWithDialog(self.assembly, self.part))
            self.assertIsNotNone(self.doc.getObject("LocalPart"))
            self.assertFalse(os.path.exists(self.path))
            _pump_gui_until(lambda: os.path.isfile(self.source_path), 20.0)
            _pump_gui_until(lambda: self.doc.getObject("LocalPart") is None, 20.0)
            self.assertEqual(warning.call_count, 1)
        self.assertTrue(os.path.isfile(self.source_path))
        self.assertTrue(os.path.isfile(self.path))
        link = next(obj for obj in self.assembly.Group if obj.TypeId == "App::Link")
        self.assertEqual(link.LinkedObject.Document.FileName, self.path)

    def test_dialog_unsaved_save_cancel(self):
        import CommandExtractComponent as command
        from PySide import QtWidgets

        self.doc.FileName = ""
        with patch.object(
            QtWidgets.QMessageBox, "warning", return_value=QtWidgets.QMessageBox.Save
        ), patch.object(
            QtWidgets.QFileDialog, "getSaveFileName", return_value=("", "")
        ) as dialog:
            self.assertIsNone(command.extractWithDialog(self.assembly, self.part))
            self.assertEqual(dialog.call_count, 1)
        self.assertIsNotNone(self.doc.getObject("LocalPart"))
        self.assertFalse(os.path.exists(self.path))

    def test_dialog_unsaved_save_failure(self):
        import CommandExtractComponent as command
        from PySide import QtWidgets
        from Test.GuiRecompute import _pump_gui_until

        self.doc.FileName = ""
        parent_file = os.path.join(self.directory.name, "not-a-directory")
        Path(parent_file).write_text("file")
        missing = os.path.join(parent_file, "Assembly.FCStd")
        with patch.object(
            QtWidgets.QMessageBox, "warning", return_value=QtWidgets.QMessageBox.Save
        ) as warning, patch.object(
            QtWidgets.QFileDialog, "getSaveFileName", return_value=(missing, "")
        ) as dialog:
            command.extractWithDialog(self.assembly, self.part)
            _pump_gui_until(lambda: warning.call_count == 2, 20.0)
            self.assertEqual(dialog.call_count, 1)
        self.assertIsNotNone(self.doc.getObject("LocalPart"))
        self.assertFalse(os.path.exists(self.path))

    def test_dialog_reports_refusal_before_changes(self):
        import CommandExtractComponent as command
        from PySide import QtWidgets

        outside = self.assembly.newObject("Part::Box", "Outside")
        self.pad.setExpression("Length", "Outside.Length")
        self.doc.recompute()
        self.doc.clearUndos()
        names = [obj.Name for obj in self.doc.Objects]
        with patch.object(
            QtWidgets.QFileDialog, "getSaveFileName", return_value=(self.path, "")
        ), patch.object(QtWidgets.QMessageBox, "warning") as warning:
            self.assertIsNone(command.extractWithDialog(self.assembly, self.part))
            warning.assert_called_once()
            self.assertIn(outside.Name, warning.call_args[0][2])
        self.assertEqual(names, [obj.Name for obj in self.doc.Objects])
        self.assertEqual(self.doc.UndoCount, 0)
        self.assertFalse(os.path.exists(self.path))

    def test_preference_roundtrip(self):
        import AssemblyPreferences

        pref = AssemblyPreferences.preferences()
        old = pref.GetBool("PartInNewFile", True)
        try:
            page = AssemblyPreferences.PreferencesPage()
            page.loadSettings()
            page.form.checkBoxPartInNewFile.setChecked(not old)
            page.saveSettings()
            self.assertEqual(pref.GetBool("PartInNewFile"), not old)
            page.loadSettings()
            self.assertEqual(page.form.checkBoxPartInNewFile.isChecked(), not old)
        finally:
            pref.SetBool("PartInNewFile", old)

    def test_detach_preference_roundtrip_and_default(self):
        import AssemblyPreferences

        pref = AssemblyPreferences.preferences()
        old = pref.GetBool("DetachStdPart", False)
        try:
            pref.RemBool("DetachStdPart")
            page = AssemblyPreferences.PreferencesPage()
            page.loadSettings()
            self.assertFalse(page.form.checkBoxDetachStdPart.isChecked())
            page.form.checkBoxDetachStdPart.setChecked(True)
            page.saveSettings()
            self.assertTrue(pref.GetBool("DetachStdPart", False))
            page.loadSettings()
            self.assertTrue(page.form.checkBoxDetachStdPart.isChecked())
        finally:
            pref.SetBool("DetachStdPart", old)

    def test_std_part_offer(self):
        import FreeCADGui as Gui
        import AssemblyPreferences
        import CommandExtractComponent as command
        from PySide import QtCore, QtGui, QtWidgets
        from Test.GuiRecompute import recompute_document

        pref = AssemblyPreferences.preferences()
        old = pref.GetBool("DetachStdPart", False)
        actions = Gui.getMainWindow().findChildren(QtGui.QAction, "Std_Part")
        self.assertTrue(actions)
        try:
            for enabled, gui, count in ((False, True, 0), (True, False, 0), (True, True, 1)):
                with self.subTest(enabled=enabled, gui=gui):
                    pref.SetBool("DetachStdPart", enabled)
                    Gui.activeDocument().activeView().setActiveObject("part", None)
                    recompute_document(self.doc)
                    with patch.object(
                        QtWidgets.QMessageBox, "question",
                        side_effect=AssertionError("Std_Part must not ask a modal question"),
                    ) as question, patch.object(command, "extractWithDialog") as dialog:
                        if gui:
                            actions[0].trigger()
                        else:
                            Gui.runCommand("Std_Part")
                        QtCore.QCoreApplication.processEvents()
                        question.assert_not_called()
                        self.assertEqual(dialog.call_count, count)
                        if count:
                            assembly, part = dialog.call_args[0]
                            self.assertEqual(assembly, self.assembly)
                            self.assertIn(part, self.assembly.Group)
        finally:
            pref.SetBool("DetachStdPart", old)

    def test_std_part_offer_cancel_keeps_local(self):
        import FreeCADGui as Gui
        import AssemblyPreferences
        from PySide import QtCore, QtGui, QtWidgets
        from Test.GuiRecompute import recompute_document

        pref = AssemblyPreferences.preferences()
        old = pref.GetBool("DetachStdPart", False)
        before = set(self.assembly.Group)
        try:
            pref.SetBool("DetachStdPart", True)
            Gui.activeDocument().activeView().setActiveObject("part", None)
            recompute_document(self.doc)
            with patch.object(
                QtWidgets.QFileDialog, "getSaveFileName", return_value=("", "")
            ) as dialog, patch.object(
                QtWidgets.QMessageBox, "question",
                side_effect=AssertionError("Std_Part must not ask a modal question"),
            ):
                Gui.getMainWindow().findChild(QtGui.QAction, "Std_Part").trigger()
                QtCore.QCoreApplication.processEvents()
                self.assertEqual(dialog.call_count, 1)
            added = set(self.assembly.Group) - before
            self.assertEqual(len(added), 1)
            self.assertEqual(next(iter(added)).TypeId, "App::Part")
            self.assertFalse(os.path.exists(self.path))
        finally:
            pref.SetBool("DetachStdPart", old)

    def test_std_part_offer_only_directly_in_assembly(self):
        import AssemblyPreferences
        import CommandExtractComponent as command

        pref = AssemblyPreferences.preferences()
        old = pref.GetBool("DetachStdPart", False)
        child = self.part.newObject("App::Part", "NestedPart")
        try:
            pref.SetBool("DetachStdPart", True)
            with patch.object(command, "extractWithDialog") as dialog:
                command.offer(child)
                dialog.assert_not_called()
        finally:
            pref.SetBool("DetachStdPart", old)

    def test_waits_for_delayed_commit(self):
        import ExtractComponent

        self.assertTrue(ExtractComponent._gui_thread())
        state = {"callback_done": False, "slept": False}
        events = []
        real_commit = ExtractComponent._commit_on_owner
        real_close = ExtractComponent.App.closeDocument
        real_unlink = ExtractComponent.os.unlink
        target = os.path.realpath(self.path)
        started = time.monotonic()

        def commit(document, callback, pump=None, **kwargs):
            if getattr(document, "Name", None) == self.doc.Name:
                # Bind the original callback now. Assigning the wrapper to the
                # same local would make the wrapper call itself.
                def delayed(inner=callback):
                    state["slept"] = True
                    time.sleep(0.8)
                    inner()
                    state["callback_done"] = True

                callback = delayed
            return real_commit(document, callback, pump=pump, **kwargs)

        def close(name):
            events.append(("close", state["callback_done"], name))
            return real_close(name)

        def unlink(path, *args, **kwargs):
            if os.path.realpath(path) == target:
                events.append(("unlink", state["callback_done"], path))
            return real_unlink(path, *args, **kwargs)

        class JumpingClock:
            """ExtractComponent's clock only. Other threads keep the real time module."""

            def __init__(self):
                self.now = 0.0

            def monotonic(self):
                self.now += 1000.0
                return self.now

            def __getattr__(self, name):
                return getattr(time, name)

        try:
            with patch.object(
                ExtractComponent, "time", JumpingClock()
            ), patch(
                "ExtractComponent._commit_on_owner", side_effect=commit
            ), patch("ExtractComponent.App.closeDocument", side_effect=close), patch(
                "ExtractComponent.os.unlink", side_effect=unlink
            ):
                try:
                    result = self.runExtract()
                except ExtractionError as exc:
                    self.fail(f"extraction abandoned the commit: {exc}; cleanup={events}")
        finally:
            # Keep draining owner tasks so a commit that is still running can
            # reach a terminal state before tear-down.
            deadline = time.monotonic() + 8
            while not state["callback_done"] and time.monotonic() < deadline:
                for document in list(App.listDocuments().values()):
                    ExtractComponent._service_owner(document)
                time.sleep(0.02)
        self.assertEqual(events, [])
        self.assertTrue(state["slept"])
        self.assertTrue(state["callback_done"])
        self.assertGreater(time.monotonic() - started, 0.8)
        self.assertTrue(os.path.isfile(self.path))
        self.assertIsNone(self.doc.getObject("LocalPart"))
        link = self.doc.getObject("LocalPartLink")
        self.assertIsNotNone(link)
        self.assertEqual(result.link, link)
        self.assertEqual(
            os.path.realpath(link.LinkedObject.Document.FileName),
            target,
        )

    def test_gui_suite_registered_once(self):
        # TestApp.All() loads every FreeCAD.__unit_test__ entry with no dedupe,
        # and run_gui_tests.discover_gui_suites keeps that list in order.
        self.assertEqual(list(App.__unit_test__).count("TestAssemblyGui"), 1)
