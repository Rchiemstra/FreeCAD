# SPDX-License-Identifier: LGPL-2.1-or-later
"""Component extraction integration tests, shared by headless and GUI CI."""

import os
import tempfile
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
        self.before = set(App.listDocuments())
        self.directory = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.directory.name, "Component.FCStd")
        self.source_path = os.path.join(self.directory.name, "Assembly.FCStd")
        self.doc = App.newDocument("ExtractionTest")
        self.doc.UndoMode = 1
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
        names = [obj.Name for obj in self.doc.Objects]
        with patch("ExtractComponent.os.link", side_effect=OSError("write denied")):
            with self.assertRaises(OSError):
                self.runExtract()
        self.assertEqual(names, [obj.Name for obj in self.doc.Objects])
        self.assertFalse(os.path.exists(self.path))
        self.assertEqual(set(App.listDocuments()) - self.before, {self.doc.Name})

    def test_replacement_failure_rolls_back_and_removes_file(self):
        import AssemblyApp

        original = AssemblyApp._extractionReferences
        names = [obj.Name for obj in self.doc.Objects]

        def fail(owners, pairs, apply=False):
            if apply:
                raise RuntimeError("replacement rejected")
            return original(owners, pairs)

        with patch.object(AssemblyApp, "_extractionReferences", side_effect=fail):
            with self.assertRaisesRegex(RuntimeError, "replacement rejected"):
                self.runExtract()
        self.assertEqual(names, [obj.Name for obj in self.doc.Objects])
        self.assertFalse(os.path.exists(self.path))
        self.assertEqual(self.doc.UndoCount, 0)
        self.assertEqual(set(App.listDocuments()) - self.before, {self.doc.Name})

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

        Gui.activateWorkbench("AssemblyWorkbench")
        Gui.activeDocument().setEdit(self.assembly.Name)

    def test_appearance(self):
        self.pad.ViewObject.ShapeColor = (0.2, 0.6, 0.8)
        self.pad.ViewObject.Transparency = 35
        color = self.pad.ViewObject.ShapeColor
        result = self.runExtract()
        self.assertEqual(result.document.Pad.ViewObject.ShapeColor, color)
        self.assertEqual(result.document.Pad.ViewObject.Transparency, 35)
        self.assertFalse(result.link.ViewObject.OverrideMaterial)
        self.assertEqual(
            result.component.ViewObject.ShapeAppearance[0].DiffuseColor,
            result.link.LinkedObject.ViewObject.ShapeAppearance[0].DiffuseColor,
        )

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
        import FreeCADGui as Gui
        import CommandExtractComponent as command
        from PySide import QtWidgets

        self.doc.FileName = ""
        self.source_path = os.path.join(self.directory.name, "SavedAssembly.FCStd")

        def save():
            self.doc.saveAs(self.source_path)
            return True

        with patch.object(
            QtWidgets.QMessageBox, "warning", return_value=QtWidgets.QMessageBox.Save
        ), patch.object(
            QtWidgets.QFileDialog, "getSaveFileName", return_value=(self.path, "")
        ), patch.object(Gui, "getDocument") as gui_document:
            gui_document.return_value.saveAs.side_effect = save
            result = command.extractWithDialog(self.assembly, self.part)
            gui_document.return_value.saveAs.assert_called_once_with()
        self.assertIsNotNone(result)
        self.assertTrue(os.path.isfile(self.source_path))
        self.assertTrue(os.path.isfile(self.path))
        self.assertIn(result.link, self.assembly.Group)

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

    def test_std_part_offer(self):
        import FreeCADGui as Gui
        import CommandExtractComponent as command
        from PySide import QtCore, QtWidgets

        with patch.object(
            QtWidgets.QMessageBox, "question", return_value=QtWidgets.QMessageBox.Yes
        ), patch.object(command, "extractWithDialog") as dialog:
            Gui.runCommand("Std_Part")
            QtCore.QCoreApplication.processEvents()
            self.assertEqual(dialog.call_count, 1)
            assembly, part = dialog.call_args[0]
            self.assertEqual(assembly, self.assembly)
            self.assertIn(part, self.assembly.Group)

    def test_std_part_offer_only_directly_in_assembly(self):
        import CommandExtractComponent as command
        from PySide import QtWidgets

        child = self.part.newObject("App::Part", "NestedPart")
        with patch.object(QtWidgets.QMessageBox, "question") as question:
            command.offer(child)
            question.assert_not_called()
