# SPDX-License-Identifier: LGPL-2.1-or-later
"""Selection isolation uses temporary scene state, including during joint editing."""

import os
import tempfile
import unittest
from types import SimpleNamespace

import FreeCAD as App
import Part


@unittest.skipUnless(App.GuiUp, "Selection isolation requires the GUI")
class TestIsolateSelectionGui(unittest.TestCase):
    def setUp(self):
        import FreeCADGui as Gui
        import CommandIsolate
        from pivy import coin

        self.Gui, self.command, self.coin = Gui, CommandIsolate, coin
        Gui.activateWorkbench("AssemblyWorkbench")
        self.doc = App.newDocument("IsolateSelection")
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.joints = self.assembly.newObject("Assembly::JointGroup", "Joints")
        self.outer = self.assembly.newObject("App::Part", "Outer")
        self.inner = self.outer.newObject("App::Part", "Inner")
        self.box = self.inner.newObject("Part::Box", "Box")
        self.sibling = self.inner.newObject("Part::Box", "Sibling")
        self.other = self.assembly.newObject("Part::Box", "Other")
        self.outside = self.doc.addObject("Part::Box", "Outside")
        self.other.ViewObject.Transparency = 37
        self.sibling.Visibility = False
        self.other.ViewObject.Selectable = False
        self.doc.recompute()
        Gui.activeDocument().setEdit(self.assembly)
        Gui.Selection.clearSelection()
        self.pref = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        self.previous_mode = self.pref.GetBool("IsolateSelectionTransparent", False)
        self.pref.SetBool("IsolateSelectionTransparent", False)
        Gui.updateGui()

    def tearDown(self):
        self.command.clear_isolation()
        self.pref.SetBool("IsolateSelectionTransparent", self.previous_mode)
        if self.doc.Name in App.listDocuments():
            self.Gui.getDocument(self.doc.Name).resetEdit()
            App.closeDocument(self.doc.Name)

    def selection(self, obj, *subs):
        return [SimpleNamespace(Object=obj, SubElementNames=subs)]

    def isolate(self, obj=None, *subs):
        self.Gui.updateGui()
        self.command.isolate_selection(self.assembly, self.selection(obj or self.box, *subs))
        self.Gui.updateGui()

    def nodes(self, root, type_id):
        search = self.coin.SoSearchAction()
        search.setType(type_id)
        search.setInterest(self.coin.SoSearchAction.ALL)
        search.setSearchingAll(True)
        search.apply(root)
        return [path.getTail() for path in search.getPaths()]

    def isolated_nodes(self, obj):
        return [
            node
            for node in self.nodes(
                self.assembly.ViewObject.RootNode, self.coin.SoSwitch.getClassTypeId()
            )
            if str(node.getName()) == "AssemblyIsolation"
            and node.getChild(node.getNumChildren() - 1) == obj.ViewObject.RootNode
        ]

    def assertHidden(self, obj):
        self.assertEqual(len(self.isolated_nodes(obj)), 1, obj.Name)
        self.assertEqual(
            self.isolated_nodes(obj)[0].whichChild.getValue(), self.coin.SO_SWITCH_NONE
        )

    def assertOriginal(self, obj):
        self.assertEqual(self.isolated_nodes(obj), [], obj.Name)

    def picked_root(self, root):
        # A camera provides the ray aperture needed for picking sketch edges.
        scene = self.coin.SoSeparator()
        camera = self.coin.SoOrthographicCamera()
        camera.position = (5, 5, 100)
        camera.height = 20
        camera.nearDistance = 1
        camera.farDistance = 200
        scene.addChild(camera)
        scene.addChild(root)
        action = self.coin.SoRayPickAction(self.coin.SbViewportRegion(100, 100))
        action.setPoint(self.coin.SbVec2s(50, 50))
        action.apply(scene)
        return action.getPickedPoint() is not None

    def picked(self, obj):
        overrides = self.isolated_nodes(obj)
        return self.picked_root(overrides[0] if overrides else obj.ViewObject.RootNode)

    def picked_scene(self):
        return self.picked_root(self.assembly.ViewObject.RootNode)

    def triangles(self, obj):
        # Traverse the viewer scene, so a detached root cannot masquerade as restored.
        count = [0]
        def triangle(data, action, *vertices):
            if action.getCurPath().containsNode(obj.ViewObject.RootNode):
                count[0] += 1
        action = self.coin.SoCallbackAction()
        action.addTriangleCallback(self.coin.SoShape.getClassTypeId(), triangle, None)
        action.apply(self.Gui.activeDocument().activeView().getSceneGraph())
        return count[0]

    def assertRestoredScene(self, objects, before):
        self.Gui.updateGui()
        for obj in objects:
            self.assertEqual((self.triangles(obj), self.picked(obj)), before[obj.Name], obj.Name)

    def sceneState(self, objects):
        self.Gui.updateGui()
        return {obj.Name: (self.triangles(obj), self.picked(obj)) for obj in objects}

    def state(self):
        return {
            obj.Name: (
                obj.Visibility,
                getattr(obj.ViewObject, "Transparency", None),
                getattr(obj.ViewObject, "Selectable", None),
            )
            for obj in self.doc.Objects
        }

    def test_nested_subelement_and_outside_scope(self):
        self.isolate(self.assembly, "Outer.Inner.Box.Face1")
        self.assertOriginal(self.box)
        self.assertHidden(self.other)
        self.assertOriginal(self.outside)

    def test_selected_container_preserves_descendants(self):
        self.isolate(self.outer)
        self.assertOriginal(self.box)
        self.assertOriginal(self.sibling)
        self.assertFalse(self.sibling.Visibility)
        self.assertGreater(self.triangles(self.sibling), 0)
        self.command.clear_isolation()
        self.assertEqual(self.triangles(self.sibling), 0)

    def test_subassembly_at_depth(self):
        sub = self.inner.newObject("Assembly::AssemblyObject", "Subassembly")
        nested = sub.newObject("App::Part", "Nested")
        child = nested.newObject("Part::Box", "Child")
        self.doc.recompute()
        self.isolate(self.assembly, "Outer.Inner.Subassembly.Nested.Child.Edge1")
        self.assertOriginal(child)
        self.assertHidden(self.box)
        self.assertHidden(self.other)
        self.isolate(sub)
        self.assertOriginal(child)
        self.assertHidden(self.box)

    def test_body_face_selection(self):
        body = self.inner.newObject("PartDesign::Body", "Body")
        feature = body.newObject("PartDesign::Feature", "Feature")
        feature.Shape = Part.makeBox(2, 2, 2)
        self.doc.recompute()
        self.isolate(self.assembly, "Outer.Inner.Body.Feature.Face1")
        self.assertOriginal(body)
        self.assertHidden(self.box)
        self.assertHidden(self.other)
        self.isolate(feature, "Face1")
        self.assertOriginal(body)
        self.assertHidden(self.box)
        self.isolate(self.box)
        self.assertHidden(body)
        self.assertTrue(feature.Visibility)

    def test_link_occurrence_subelement_does_not_change_source(self):
        link = self.inner.newObject("App::Link", "Link")
        link.setLink(self.outside)
        self.doc.recompute()
        self.isolate(self.assembly, "Outer.Inner.Link.Face1")
        self.assertOriginal(link)
        self.assertOriginal(self.outside)
        self.assertHidden(self.box)

    def test_multiple_selection_and_replacement(self):
        self.command.isolate_selection(
            self.assembly,
            self.selection(self.box, "Edge1") + self.selection(self.other, "Vertex1"),
        )
        self.assertOriginal(self.box)
        self.assertOriginal(self.other)
        self.isolate(self.other)
        self.assertHidden(self.box)
        self.assertOriginal(self.other)

    def test_selected_hidden_component_is_revealed(self):
        self.isolate(self.sibling)
        self.assertGreater(self.triangles(self.sibling), 0)
        self.assertTrue(self.picked(self.sibling))
        self.sibling.ViewObject.DisplayMode = "Shaded"
        self.Gui.updateGui()
        self.assertGreater(self.triangles(self.sibling), 0)
        self.sibling.Length = 12
        self.doc.recompute()
        self.Gui.updateGui()
        self.assertGreater(self.triangles(self.sibling), 0)
        self.assertFalse(self.sibling.Visibility)
        self.assertOriginal(self.sibling)
        self.assertHidden(self.box)

    def test_transparency_and_real_ray_pick(self):
        self.pref.SetBool("IsolateSelectionTransparent", True)
        self.other.ViewObject.Selectable = True
        self.isolate()
        materials = self.nodes(
            self.isolated_nodes(self.other)[0], self.coin.SoMaterial.getClassTypeId()
        )
        material = next(m for m in materials if str(m.getName()) == "AssemblyIsolationMaterial")
        self.assertAlmostEqual(material.transparency[0], 0.8, places=5)
        self.assertTrue(material.isOverride())
        self.assertTrue(self.picked(self.box))
        self.assertFalse(self.picked(self.other))
        self.command.clear_isolation()
        self.assertTrue(self.picked(self.other))

    def test_background_link_and_selected_occurrence(self):
        first = self.inner.newObject("App::Link", "FirstOccurrence")
        second = self.inner.newObject("App::Link", "SecondOccurrence")
        first.setLink(self.box)
        second.setLink(self.box)
        self.doc.recompute()
        self.assertTrue(self.picked(first))
        self.assertTrue(self.picked(second))
        for transparent in (False, True):
            self.pref.SetBool("IsolateSelectionTransparent", transparent)
            self.isolate(first, "Face1")
            self.assertTrue(self.picked(first))
            self.assertTrue(self.picked_scene())
            self.assertFalse(self.picked(second))
            # A linked source update must not copy its isolation into the occurrence.
            self.box.Length = self.box.Length.Value + 1
            self.doc.recompute()
            self.Gui.updateGui()
            self.assertTrue(self.picked(first))
            self.assertTrue(self.picked_scene())
            self.assertFalse(self.picked(second))
            self.command.clear_isolation()
            self.assertTrue(self.picked(second))

    def test_linked_part_container_keeps_its_own_scene(self):
        link = self.assembly.newObject("App::Link", "PartOccurrence")
        link.setLink(self.outer)
        self.doc.recompute()
        self.Gui.updateGui()
        self.assertTrue(self.picked(link))
        self.isolate(link, "Inner.Box.Face1")
        self.assertHidden(self.box)
        self.assertTrue(self.picked(link))
        self.box.Length = 12
        self.doc.recompute()
        self.Gui.updateGui()
        self.assertTrue(self.picked(link))
        self.assertHidden(self.box)

    def test_override_state_is_scoped_in_full_scene(self):
        self.sibling.Visibility = True
        self.inner.ViewObject.OverrideMaterial = True
        self.Gui.updateGui()

        def materials():
            seen = {}

            def before_node(data, action, node):
                for obj in (self.box, self.sibling):
                    if node == obj.ViewObject.RootNode:
                        seen[obj.Name] = action.getMaterial()[-1]
                return self.coin.SoCallbackAction.CONTINUE

            action = self.coin.SoCallbackAction()
            action.addPreCallback(self.coin.SoSeparator.getClassTypeId(), before_node, None)
            action.apply(self.assembly.ViewObject.RootNode)
            return seen

        before = materials()
        self.pref.SetBool("IsolateSelectionTransparent", True)
        self.isolate(self.sibling)
        after = materials()
        self.assertAlmostEqual(after[self.box.Name], 0.8, places=5)
        self.assertEqual(after[self.sibling.Name], before[self.sibling.Name])
        self.command.clear_isolation()
        self.assertEqual(materials(), before)

    def test_assembly_link_nested_component(self):
        source = self.doc.addObject("Assembly::AssemblyObject", "SourceAssembly")
        source.newObject("Assembly::JointGroup", "SourceJoints")
        source.newObject("Part::Box", "SourceBox")
        source.newObject("Part::Box", "SourceOther")
        instance = self.inner.newObject("Assembly::AssemblyLink", "AssemblyInstance")
        instance.LinkedObject = source
        self.doc.recompute()
        parts = [obj for obj in instance.Group if obj.isDerivedFrom("App::Link")]
        self.assertEqual(len(parts), 2)
        self.isolate(
            self.assembly,
            "Outer.Inner." + instance.Name + "." + parts[0].Name + ".Face1",
        )
        self.assertOriginal(parts[0])
        self.assertHidden(parts[1])
        self.assertHidden(self.box)

    def test_sketch_and_datum_selection(self):
        sketch = self.inner.newObject("Sketcher::SketchObject", "Sketch")
        datum = self.inner.newObject("PartDesign::Plane", "Datum")
        sketch.addGeometry(Part.LineSegment(App.Vector(0, 0, 0), App.Vector(10, 10, 0)))
        sketch.Visibility = False
        datum.Visibility = False
        self.doc.recompute()
        self.Gui.updateGui()
        self.command.isolate_selection(
            self.assembly, self.selection(sketch) + self.selection(datum)
        )
        self.Gui.updateGui()
        self.assertOriginal(sketch)
        self.assertOriginal(datum)
        self.assertTrue(self.picked(sketch))
        self.assertTrue(self.picked(datum))
        self.assertFalse(sketch.Visibility)
        self.assertFalse(datum.Visibility)
        self.assertHidden(self.box)
        self.isolate()
        self.assertHidden(sketch)
        self.assertHidden(datum)
        self.command.clear_isolation()
        self.assertFalse(self.picked(sketch))
        self.assertFalse(self.picked(datum))

    def test_existing_unpickability_is_preserved(self):
        self.box.ViewObject.Selectable = False
        self.pref.SetBool("IsolateSelectionTransparent", True)
        self.isolate()
        self.assertFalse(self.picked(self.box))
        self.command.clear_isolation()
        self.assertFalse(self.picked(self.box))

    def test_legacy_joint_isolation_remains_available(self):
        self.assembly.ViewObject.isolateComponents([self.box], 2)
        self.assertFalse(self.other.Visibility)
        self.assembly.ViewObject.clearIsolate()
        self.assertTrue(self.other.Visibility)
        self.assertFalse(self.sibling.Visibility)
        self.assertFalse(self.other.ViewObject.Selectable)

    def test_legacy_joint_reference_selection_restores_joint_visibility(self):
        joint = self.joints.newObject("App::FeaturePython", "GroundedJoint")
        joint.addProperty("App::PropertyLink", "ObjectToGround")
        joint.ObjectToGround = self.box
        joint.Visibility = False
        self.doc.recompute()
        self.Gui.Selection.addSelection(joint)
        self.assertTrue(joint.Visibility)
        self.assertFalse(self.other.ViewObject.Selectable)
        self.Gui.Selection.clearSelection()
        self.assertFalse(joint.Visibility)
        self.assertTrue(self.other.Visibility)
        self.assertFalse(self.sibling.Visibility)

    def test_exact_restore_mixed_states_both_modes(self):
        self.inner.Visibility = False
        self.outer.Visibility = False
        for transparent in (False, True):
            self.pref.SetBool("IsolateSelectionTransparent", transparent)
            before = self.state()
            self.isolate()
            self.command.clear_isolation()
            self.assertEqual(self.state(), before)
            for obj in (self.box, self.other, self.sibling):
                self.assertOriginal(obj)

    def test_empty_selection_exits(self):
        self.isolate()
        self.Gui.Selection.clearSelection()
        self.assertTrue(self.command.CommandIsolateSelection().IsActive())
        self.Gui.runCommand("Assembly_IsolateSelection")
        self.Gui.updateGui()
        self.assertOriginal(self.other)
        self.assertFalse(self.command.is_isolated(self.assembly))

    def test_no_undo_or_modified_flag_and_save_original_state(self):
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "isolate.FCStd")
            self.doc.UndoMode = 1
            self.doc.saveAs(path)
            self.doc.clearUndos()
            before = self.state()
            file_state = self.doc.getFileChangeState()
            for transparent in (False, True):
                self.pref.SetBool("IsolateSelectionTransparent", transparent)
                self.Gui.Selection.clearSelection()
                self.Gui.Selection.addSelection(self.assembly, "Outer.Inner.Box.Face1")
                self.Gui.runCommand("Assembly_IsolateSelection")
                self.Gui.updateGui()
                self.assertEqual(self.doc.UndoCount, 0)
                self.assertFalse(self.Gui.activeDocument().Modified)
                self.assertEqual(self.doc.getFileChangeState(), file_state)
                self.doc.save()
                self.assertTrue(self.command.is_isolated(self.assembly))
                self.assertEqual(len(self.isolated_nodes(self.other)), 1)
                self.Gui.runCommand("Assembly_ExitIsolation")
                self.Gui.updateGui()
                self.assertEqual(self.doc.UndoCount, 0)
                self.assertFalse(self.Gui.activeDocument().Modified)
                self.assertEqual(self.state(), before)
            name = self.doc.Name
            App.closeDocument(name)
            self.doc = App.openDocument(path)
            self.assertEqual(self.state(), before)

    def test_joint_edit_keeps_isolation(self):
        import JointObject

        self.isolate()
        panel = JointObject.TaskAssemblyCreateJoint(0)
        self.Gui.Control.showDialog(panel)
        panel.updateIsolation()
        self.Gui.updateGui()
        self.assertEqual(len(self.isolated_nodes(self.other)), 1)
        panel.reject()
        self.Gui.updateGui()
        self.assertTrue(self.command.is_isolated(self.assembly))
        self.assertHidden(self.other)
        self.assertOriginal(self.box)
        self.command.clear_isolation()
        self.assertOriginal(self.other)

    def test_accept_existing_joint_edit_keeps_isolation(self):
        import JointObject

        joint = self.joints.newObject("App::FeaturePython", "Joint")
        JointObject.Joint(joint, 0)
        joint.Reference1 = (self.box, ["Face1", "Vertex1"])
        joint.Reference2 = (self.other, ["Face1", "Vertex1"])
        self.doc.recompute()
        self.isolate()
        panel = JointObject.TaskAssemblyCreateJoint(0, joint)
        self.Gui.Control.showDialog(panel)
        panel.updateIsolation()
        self.Gui.updateGui()
        self.assertHidden(self.other)
        self.assertTrue(panel.accept())
        self.Gui.updateGui()
        self.assertTrue(self.command.is_isolated(self.assembly))
        self.assertHidden(self.other)
        self.command.clear_isolation()
        self.assertOriginal(self.other)

    def test_exit_cancels_pending_scene_refresh(self):
        self.isolate()
        self.doc.openTransaction("Add component")
        added = self.assembly.newObject("Part::Box", "Added")
        self.doc.commitTransaction()
        self.command.clear_isolation()
        self.Gui.updateGui()
        self.assertFalse(self.command.is_isolated())
        self.assertOriginal(self.other)
        self.assertOriginal(added)

    def test_document_switch_exits(self):
        self.isolate()
        another = App.newDocument("AnotherIsolationDocument")
        try:
            self.assertFalse(self.command.is_isolated(self.assembly))
            self.assertOriginal(self.other)
        finally:
            App.closeDocument(another.Name)
            App.setActiveDocument(self.doc.Name)

    def test_document_close_exits(self):
        self.isolate()
        name = self.doc.Name
        App.closeDocument(name)
        self.assertFalse(self.command.is_isolated())
        self.doc = App.newDocument("IsolationAfterClose")

    def test_deleted_component_and_assembly_cleanup(self):
        self.isolate()
        self.doc.removeObject(self.other.Name)
        self.command.clear_isolation()
        self.isolate()
        self.doc.removeObject(self.assembly.Name)
        self.assertFalse(self.command.is_isolated())

    def test_ungroup_and_move_background_restore_scene(self):
        child = self.inner.newObject("Part::Box", "BackgroundChild")
        self.doc.recompute()
        objects = (self.box, child, self.other, self.sibling, self.outside)
        before = self.sceneState(objects)
        for transparent in (False, True):
            self.pref.SetBool("IsolateSelectionTransparent", transparent)
            for destination in (None, self.outer):
                with self.subTest(transparent=transparent, destination=destination):
                    self.inner.addObject(child)
                    self.isolate()
                    self.inner.removeObject(child)
                    if destination is not None:
                        destination.addObject(child)
                    self.Gui.updateGui()
                    self.command.clear_isolation()
                    self.assertRestoredScene(objects, before)

    def test_deactivation_and_reset_edit_exit(self):
        objects = (self.box, self.other)
        before = self.sceneState(objects)
        for double_click in (True, False):
            self.isolate()
            if double_click:
                self.assembly.ViewObject.doubleClicked()
            else:
                self.Gui.activeDocument().resetEdit()
            self.Gui.updateGui()
            self.assertFalse(self.command.is_isolated())
            self.assertRestoredScene(objects, before)
            self.Gui.activeDocument().setEdit(self.assembly)

    def test_hidden_ancestors_save_originals(self):
        self.outer.Visibility = False
        self.inner.Visibility = False
        self.box.Visibility = False
        with tempfile.TemporaryDirectory() as directory:
            path = os.path.join(directory, "hidden.FCStd")
            self.doc.saveAs(path)
            self.doc.clearUndos()
            before = self.state()
            for transparent in (False, True):
                self.pref.SetBool("IsolateSelectionTransparent", transparent)
                self.isolate(self.outer)
                self.assertGreater(self.triangles(self.box), 0)
                self.assertGreater(self.triangles(self.sibling), 0)
                self.assertTrue(self.picked_scene())
                self.assertEqual(self.state(), before)
                self.assertEqual(self.doc.UndoCount, 0)
                self.assertFalse(self.Gui.activeDocument().Modified)
                self.doc.save()
                self.command.clear_isolation()
                self.assertEqual(self.triangles(self.box), 0)
            App.closeDocument(self.doc.Name)
            self.doc = App.openDocument(path)
            self.assertEqual(self.state(), before)

    def test_lifecycle_undo_redo_rebuild_recompute_delete(self):
        child = self.inner.newObject("Part::Box", "LifecycleChild")
        self.other.ViewObject.Selectable = True
        self.doc.recompute()
        objects = (self.box, child, self.other)
        before = self.sceneState(objects)
        self.doc.UndoMode = 1
        for transparent in (False, True):
            self.pref.SetBool("IsolateSelectionTransparent", transparent)
            self.isolate()
            self.doc.openTransaction("Ungroup background")
            self.inner.removeObject(child)
            self.doc.commitTransaction()
            self.Gui.updateGui()
            self.doc.undo()
            self.Gui.updateGui()
            self.doc.redo()
            self.Gui.updateGui()
            self.box.Length = self.box.Length.Value + 1
            self.other.Length = self.other.Length.Value + 1
            child.Length = child.Length.Value + 1
            self.doc.recompute()
            display = "Shaded" if not transparent else "Flat Lines"
            self.box.ViewObject.DisplayMode = display
            self.other.ViewObject.DisplayMode = display
            self.Gui.updateGui()
            self.assertTrue(self.picked(self.box))
            self.assertFalse(self.picked(self.other))
            if self.doc.getObject("Sibling"):
                self.doc.removeObject("Sibling")
            self.Gui.updateGui()
            self.command.clear_isolation()
            self.assertRestoredScene(objects, before)
            self.inner.addObject(child)

    def test_nested_link_joint_edit_reference_modes_restore(self):
        import JointObject

        source = self.doc.addObject("Part::Box", "JointSource")
        nested = self.doc.addObject("App::Link", "NestedLink")
        nested.setLink(source)
        link = self.inner.newObject("App::Link", "JointOccurrence")
        link.setLink(nested)
        for obj in (nested, link):
            obj.ViewObject.OverrideMaterial = True
            material = obj.ViewObject.ShapeAppearance[0]
            material.DiffuseColor = (0.2, 0.4, 0.6)
            material.Transparency = 0.31
            obj.ViewObject.ShapeAppearance = [material]
        self.other.ViewObject.Selectable = True
        joint = self.joints.newObject("App::FeaturePython", "Joint")
        JointObject.Joint(joint, 0)
        joint.Reference1 = (link, ["", ""])
        joint.Reference2 = (self.box, ["Face1", "Vertex1"])
        self.doc.recompute()
        objects = (link, nested, source, self.box, self.other)
        def material_state():
            rendered = {obj.Name: [] for obj in (link, nested)}

            def triangle(data, action, *vertices):
                colors = action.getMaterial()
                material = tuple(tuple(color.getValue()) for color in colors[:4]) + tuple(colors[4:])
                for obj in (link, nested):
                    if action.getCurPath().containsNode(obj.ViewObject.RootNode):
                        rendered[obj.Name].append(material)

            action = self.coin.SoCallbackAction()
            action.addTriangleCallback(self.coin.SoShape.getClassTypeId(), triangle, None)
            action.apply(self.Gui.activeDocument().activeView().getSceneGraph())
            return {
                obj.Name: (obj.ViewObject.OverrideMaterial, tuple(obj.ViewObject.ShapeAppearance),
                           sorted(rendered[obj.Name]))
                for obj in (link, nested)
            }

        for user in (False, True):
            link.Visibility = not user
            before = self.sceneState(objects)
            state = self.state()
            materials = material_state()
            for mode in (2, 0, 1):
                if user:
                    self.isolate(link)
                panel = JointObject.TaskAssemblyCreateJoint(0, joint)
                self.Gui.Control.showDialog(panel)
                panel.jForm.isolateType.setCurrentIndex(mode)
                panel.updateIsolation()
                self.Gui.updateGui()
                self.assertTrue(self.picked(link))
                self.assertGreater(self.triangles(link), 0)
                self.assertFalse(self.picked(self.other))
                panel.reject()
                self.Gui.updateGui()
                if user:
                    self.assertTrue(self.command.is_isolated())
                    self.assertFalse(self.picked(self.other))
                    self.command.clear_isolation()
                self.assertEqual(self.state(), state)
                self.assertEqual(material_state(), materials)
                self.assertRestoredScene(objects, before)

    def test_transparent_preference_page_roundtrip(self):
        import AssemblyPreferences

        page = AssemblyPreferences.PreferencesPage()
        try:
            page.loadSettings()
            self.assertFalse(page.form.isolateTransparent.isChecked())
            page.form.isolateTransparent.setChecked(True)
            page.saveSettings()
            self.assertTrue(self.pref.GetBool("IsolateSelectionTransparent"))
            self.pref.SetBool("IsolateSelectionTransparent", False)
            page.loadSettings()
            self.assertFalse(page.form.isolateTransparent.isChecked())
        finally:
            page.form.deleteLater()

    def test_commands_icons_and_enablement(self):
        from PySide import QtGui, QtWidgets
        from unittest.mock import patch

        for name in ("Assembly_IsolateSelection", "Assembly_ExitIsolation"):
            self.assertIn(name, self.Gui.listCommands())
        workbench = self.Gui.activeWorkbench()
        toolbar_commands = [
            item for items in workbench.getToolbarItems().values() for item in items
        ]
        for name in ("Assembly_IsolateSelection", "Assembly_ExitIsolation"):
            self.assertIn(name, toolbar_commands)
        with patch.object(workbench, "appendContextMenu") as append:
            workbench.ContextMenu("View")
        self.assertTrue(
            any("Assembly_ExitIsolation" in call.args[1] for call in append.call_args_list)
        )
        menu_actions = [
            action.objectName()
            for menu in self.Gui.getMainWindow().findChildren(QtWidgets.QMenu)
            for action in menu.actions()
        ]
        self.assertIn("Assembly_IsolateSelection", menu_actions)
        self.assertIn("Assembly_ExitIsolation", menu_actions)
        isolate = self.command.CommandIsolateSelection()
        exit_isolation = self.command.CommandExitIsolation()
        self.assertFalse(isolate.IsActive())
        self.assertFalse(exit_isolation.IsActive())
        self.Gui.Selection.addSelection(self.assembly, "Outer.Inner.Box.Face1")
        self.assertTrue(isolate.IsActive())
        isolate.Activated()
        self.assertTrue(exit_isolation.IsActive())
        with patch("UtilsAssembly.activeAssembly", return_value=None):
            self.assertTrue(exit_isolation.IsActive())
        self.Gui.Selection.clearSelection()
        exit_isolation.Activated()
        self.assertOriginal(self.other)
        for command in (isolate, exit_isolation):
            resources = command.GetResources()
            self.assertFalse(QtGui.QIcon(":/icons/" + resources["Pixmap"] + ".svg").isNull())
        self.Gui.activeDocument().resetEdit()
        self.assertFalse(isolate.IsActive())
        self.assertFalse(exit_isolation.IsActive())
