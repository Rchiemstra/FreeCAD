# SPDX-License-Identifier: LGPL-2.1-or-later
# /****************************************************************************
#                                                                           *
#    Copyright (c) 2023 Ondsel <development@ondsel.com>                     *
#                                                                           *
#    This file is part of FreeCAD.                                          *
#                                                                           *
#    FreeCAD is free software: you can redistribute it and/or modify it     *
#    under the terms of the GNU Lesser General Public License as            *
#    published by the Free Software Foundation, either version 2.1 of the   *
#    License, or (at your option) any later version.                        *
#                                                                           *
#    FreeCAD is distributed in the hope that it will be useful, but         *
#    WITHOUT ANY WARRANTY; without even the implied warranty of             *
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
#    Lesser General Public License for more details.                        *
#                                                                           *
#    You should have received a copy of the GNU Lesser General Public       *
#    License along with FreeCAD. If not, see                                *
#    <https://www.gnu.org/licenses/>.                                       *
#                                                                           *
# ***************************************************************************/

import FreeCAD as App
import Assembly
import Part
import unittest
from types import SimpleNamespace
from unittest.mock import MagicMock, patch

import UtilsAssembly
import JointObject


def _msg(text, end="\n"):
    """Write messages to the console including the line ending."""
    App.Console.PrintMessage(text + end)


class AssemblyTestBase(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        """setUpClass()...
        This method is called upon instantiation of this test class.  Add code and objects here
        that are needed for the duration of the test() methods in this class.  In other words,
        set up the 'global' test environment here; use the `setUp()` method to set up a 'local'
        test environment.
        This method does not have access to the class `self` reference, but it
        is able to call static methods within this same class.
        """
        pass

    @classmethod
    def tearDownClass(cls):
        """tearDownClass()...
        This method is called prior to destruction of this test class.  Add code and objects here
        that cleanup the test environment after the test() methods in this class have been executed.
        This method does not have access to the class `self` reference.  This method
        is able to call static methods within this same class.
        """
        pass

    # Setup and tear down methods called before and after each unit test
    def setUp(self):
        """setUp()...
        This method is called prior to each `test()` method.  Add code and objects here
        that are needed for multiple `test()` methods.
        """
        doc_name = self.__class__.__name__
        if App.ActiveDocument:
            if App.ActiveDocument.Name != doc_name:
                App.newDocument(doc_name)
        else:
            App.newDocument(doc_name)
        App.setActiveDocument(doc_name)
        self.doc = App.ActiveDocument

        self.assembly = App.ActiveDocument.addObject("Assembly::AssemblyObject", "Assembly")
        if self.assembly:
            self.jointgroup = self.assembly.newObject("Assembly::JointGroup", "Joints")

        _msg("  Temporary document '{}'".format(self.doc.Name))

    def tearDown(self):
        """tearDown()...
        This method is called after each test() method. Add cleanup instructions here.
        Such cleanup instructions will likely undo those in the setUp() method.
        """
        App.closeDocument(self.doc.Name)


class TestCore(AssemblyTestBase):
    def test_create_assembly(self):
        """Create an assembly."""
        operation = "Create Assembly Object"
        _msg("  Test '{}'".format(operation))
        self.assertTrue(self.assembly, "'{}' failed".format(operation))

    def test_create_jointGroup(self):
        """Create a joint group in an assembly."""
        operation = "Create JointGroup Object"
        _msg("  Test '{}'".format(operation))
        self.assertTrue(self.jointgroup, "'{}' failed".format(operation))

    def test_api_create_assembly(self):
        """Create an assembly through the public Assembly API."""
        operation = "Create Assembly Object through API"
        _msg("  Test '{}'".format(operation))

        assembly = Assembly.createAssembly(self.doc, "ApiAssembly", recompute=False)

        self.assertTrue(assembly.isDerivedFrom("Assembly::AssemblyObject"))
        self.assertEqual(assembly.Type, "Assembly")
        joint_groups = [obj for obj in assembly.OutList if obj.TypeId == "Assembly::JointGroup"]
        self.assertEqual(len(joint_groups), 1, "'{}' failed".format(operation))

    def test_create_joint(self):
        """Create a joint in an assembly."""
        operation = "Create Joint Object"
        _msg("  Test '{}'".format(operation))

        joint = self.jointgroup.newObject("App::FeaturePython", "testJoint")
        self.assertTrue(joint, "'{}' failed (FeaturePython creation failed)".format(operation))
        JointObject.Joint(joint, 0)

        self.assertTrue(hasattr(joint, "JointType"), "'{}' failed".format(operation))

    def test_create_grounded_joint(self):
        """Create a grounded joint in an assembly."""
        operation = "Create Grounded Joint Object"
        _msg("  Test '{}'".format(operation))

        groundedjoint = self.jointgroup.newObject("App::FeaturePython", "testJoint")
        self.assertTrue(
            groundedjoint, "'{}' failed (FeaturePython creation failed)".format(operation)
        )

        box = self.assembly.newObject("Part::Box", "Box")

        JointObject.GroundedJoint(groundedjoint, box)

        self.assertTrue(
            hasattr(groundedjoint, "ObjectToGround"),
            "'{}' failed: No attribute 'ObjectToGround'".format(operation),
        )
        self.assertTrue(
            groundedjoint.ObjectToGround == box,
            "'{}' failed: ObjectToGround not set correctly.".format(operation),
        )

    def test_api_create_grounded_joint(self):
        """Create a grounded joint through the public Assembly API."""
        operation = "Create Grounded Joint through API"
        _msg("  Test '{}'".format(operation))

        box = self.assembly.newObject("Part::Box", "ApiGroundedBox")
        groundedjoint = Assembly.createGroundedJoint(
            self.assembly, box, label="ApiGroundedJoint", recompute=False
        )

        self.assertTrue(hasattr(groundedjoint, "ObjectToGround"))
        self.assertEqual(groundedjoint.ObjectToGround, box)
        self.assertEqual(groundedjoint.Label, "ApiGroundedJoint")

    def test_toggle_grounded_joint(self):
        """test grounding and ungrounding a part, added because of github.com/freecad/freecad/issues/28440"""
        operation = "Toggle Grounded Joint"
        _msg("  Test '{}'".format(operation))

        box = self.assembly.newObject("Part::Box", "Box")

        # ground the part
        groundedjoint = self.jointgroup.newObject("App::FeaturePython", "GroundedJoint")
        JointObject.GroundedJoint(groundedjoint, box)
        self.doc.recompute()

        # verify grounded
        self.assertTrue(
            hasattr(groundedjoint, "ObjectToGround"),
            "'{}' failed: No attribute 'ObjectToGround'".format(operation),
        )
        self.assertEqual(
            groundedjoint.ObjectToGround,
            box,
            "'{}' failed: ObjectToGround not set correctly".format(operation),
        )

        # unground the part
        self.doc.removeObject(groundedjoint.Name)
        self.doc.recompute()

        # verify no grounded joints remain in this part
        for joint in self.jointgroup.Group:
            if hasattr(joint, "ObjectToGround"):
                self.assertNotEqual(
                    joint.ObjectToGround,
                    box,
                    "'{}' failed: part still grounded after toggle".format(operation),
                )

    def test_find_placement(self):
        """Test find placement of joint."""
        operation = "Find placement"
        _msg("  Test '{}'".format(operation))

        joint = self.jointgroup.newObject("App::FeaturePython", "testJoint")
        JointObject.Joint(joint, 0)

        L = 2
        W = 3
        H = 7
        box = self.assembly.newObject("Part::Box", "Box")
        box.Length = L
        box.Width = W
        box.Height = H
        box.Placement = App.Placement(App.Vector(10, 20, 30), App.Rotation(15, 25, 35))

        # Step 0 : box with placement. No element selected
        ref = [self.assembly, [box.Name + ".", box.Name + "."]]
        plc = joint.Proxy.findPlacement(joint, ref)
        targetPlc = App.Placement(App.Vector(), App.Rotation())
        self.assertTrue(plc.isSame(targetPlc, 1e-6), "'{}' failed - Step 0".format(operation))

        # Step 1 : box with placement. Face + Vertex
        ref = [self.assembly, [box.Name + ".Face6", box.Name + ".Vertex7"]]
        plc = joint.Proxy.findPlacement(joint, ref)
        targetPlc = App.Placement(App.Vector(L, W, H), App.Rotation())
        self.assertTrue(plc.isSame(targetPlc, 1e-6), "'{}' failed - Step 1".format(operation))

        # Step 2 : box with placement. Edge + Vertex
        ref = [self.assembly, [box.Name + ".Edge8", box.Name + ".Vertex8"]]
        plc = joint.Proxy.findPlacement(joint, ref)
        targetPlc = App.Placement(App.Vector(L, W, 0), App.Rotation(0, -90, 270))
        self.assertTrue(plc.isSame(targetPlc, 1e-6), "'{}' failed - Step 2".format(operation))

        # Step 3 : box with placement. Vertex
        ref = [self.assembly, [box.Name + ".Vertex3", box.Name + ".Vertex3"]]
        plc = joint.Proxy.findPlacement(joint, ref)
        targetPlc = App.Placement(App.Vector(0, W, H), App.Rotation())
        _msg("  plc '{}'".format(plc))
        _msg("  targetPlc '{}'".format(targetPlc))
        self.assertTrue(plc.isSame(targetPlc, 1e-6), "'{}' failed - Step 3".format(operation))

        # Step 4 : box with placement. Face
        ref = [self.assembly, [box.Name + ".Face2", box.Name + ".Face2"]]
        plc = joint.Proxy.findPlacement(joint, ref)
        targetPlc = App.Placement(App.Vector(L, W / 2, H / 2), App.Rotation(0, -90, 180))
        _msg("  plc '{}'".format(plc))
        _msg("  targetPlc '{}'".format(targetPlc))
        self.assertTrue(plc.isSame(targetPlc, 1e-6), "'{}' failed - Step 4".format(operation))

    def test_api_create_fixed_joint_from_explicit_references(self):
        """Create a fixed joint from component-rooted API references."""
        operation = "Create Fixed Joint through API"
        _msg("  Test '{}'".format(operation))

        box1 = self.assembly.newObject("Part::Box", "ApiFixedBox1")
        box2 = self.assembly.newObject("Part::Box", "ApiFixedBox2")
        self.doc.recompute()

        ref1 = Assembly.makeJointReference(box1, "Face6", "Vertex7")
        ref2 = Assembly.makeJointReference(box2, "Face6", "Vertex7")

        joint = Assembly.createJoint(
            self.assembly,
            "Fixed",
            ref1,
            ref2,
            label="ApiFixed",
            solve=False,
            presolve=False,
            recompute=False,
        )

        self.assertEqual(joint.JointType, "Fixed")
        self.assertEqual(joint.Label, "ApiFixed")
        self.assertEqual(joint.Reference1[0], box1)
        self.assertEqual(list(joint.Reference1[1]), ["Face6", "Vertex7"])
        self.assertEqual(joint.Reference2[0], box2)
        self.assertEqual(list(joint.Reference2[1]), ["Face6", "Vertex7"])

    def test_api_create_cylindrical_joint_from_explicit_references(self):
        """Create a cylindrical joint from component-rooted API references."""
        operation = "Create Cylindrical Joint through API"
        _msg("  Test '{}'".format(operation))

        cylinder1 = self.assembly.newObject("Part::Cylinder", "ApiCylinder1")
        cylinder2 = self.assembly.newObject("Part::Cylinder", "ApiCylinder2")
        self.doc.recompute()

        ref1 = Assembly.makeJointReference(cylinder1, "Edge1")
        ref2 = Assembly.makeJointReference(cylinder2, "Edge1")

        joint = Assembly.createJoint(
            self.assembly,
            "Cylindrical",
            ref1,
            ref2,
            solve=False,
            presolve=False,
            recompute=False,
        )

        self.assertEqual(joint.JointType, "Cylindrical")
        self.assertEqual(joint.Reference1[0], cylinder1)
        self.assertEqual(list(joint.Reference1[1]), ["Edge1", "Edge1"])

    def test_api_invalid_joint_inputs(self):
        """Reject invalid headless joint creation inputs."""
        operation = "Reject invalid Joint API inputs"
        _msg("  Test '{}'".format(operation))

        box1 = self.assembly.newObject("Part::Box", "ApiInvalidBox1")
        box2 = self.assembly.newObject("Part::Box", "ApiInvalidBox2")
        ref1 = Assembly.makeJointReference(box1, "Face6", "Vertex7")
        ref2 = Assembly.makeJointReference(box2, "Face6", "Vertex7")

        with self.assertRaises(Assembly.JointCreationError):
            Assembly.createJoint(self.assembly, "InvalidType", ref1, ref2, recompute=False)

        with self.assertRaises(Assembly.JointCreationError):
            Assembly.createJoint(
                self.assembly, "Fixed", [box1, ["Face6"]], ref2, recompute=False
            )

        with self.assertRaises(Assembly.JointCreationError):
            Assembly.makeJointReference(box1, "Face6?")

    def test_api_removes_joint_after_constructor_failure(self):
        """A failed joint constructor must not leave an invalid group member."""
        box1 = self.assembly.newObject("Part::Box", "ApiConstructorFailureBox1")
        box2 = self.assembly.newObject("Part::Box", "ApiConstructorFailureBox2")
        ref1 = Assembly.makeJointReference(box1, "Face6", "Vertex7")
        ref2 = Assembly.makeJointReference(box2, "Face6", "Vertex7")

        with patch.object(JointObject, "Joint", side_effect=RuntimeError("constructor failed")):
            with self.assertRaisesRegex(RuntimeError, "constructor failed"):
                Assembly.createJoint(self.assembly, "Fixed", ref1, ref2, recompute=False)

        self.assertIsNone(self.doc.getObject("Joint"))

    def test_api_removes_joint_after_connector_failure(self):
        """A failed connector assignment must not leave an invalid group member."""
        box1 = self.assembly.newObject("Part::Box", "ApiConnectorFailureBox1")
        box2 = self.assembly.newObject("Part::Box", "ApiConnectorFailureBox2")
        ref1 = Assembly.makeJointReference(box1, "Face6", "Vertex7")
        ref2 = Assembly.makeJointReference(box2, "Face6", "Vertex7")
        original_set_connectors = JointObject.Joint.setJointConnectors

        def fail_nonempty_connectors(proxy, joint, refs, **kwargs):
            if refs:
                raise RuntimeError("connector failed")
            return original_set_connectors(proxy, joint, refs, **kwargs)

        with patch.object(JointObject.Joint, "setJointConnectors", new=fail_nonempty_connectors):
            with self.assertRaisesRegex(RuntimeError, "connector failed"):
                Assembly.createJoint(self.assembly, "Fixed", ref1, ref2, recompute=False)

        self.assertIsNone(self.doc.getObject("Joint"))

    def test_api_removes_grounded_joint_after_constructor_failure(self):
        """A failed grounded-joint constructor must not leave a group member."""
        box = self.assembly.newObject("Part::Box", "ApiGroundedConstructorFailureBox")

        with patch.object(
            JointObject, "GroundedJoint", side_effect=RuntimeError("grounded constructor failed")
        ):
            with self.assertRaisesRegex(RuntimeError, "grounded constructor failed"):
                Assembly.createGroundedJoint(self.assembly, box, recompute=False)

        self.assertIsNone(self.doc.getObject("GroundedJoint"))

    def test_joint_migration_allows_deferred_view_object(self):
        """A GUI replay may create the document object before its view object."""
        class DeferredJoint:
            ViewObject = None

            def __init__(self):
                self.extensions = set()

            def hasExtension(self, extension):
                return extension in self.extensions

            def addExtension(self, extension):
                self.extensions.add(extension)

        joint = DeferredJoint()
        proxy = JointObject.Joint.__new__(JointObject.Joint)
        original_app = JointObject.App
        JointObject.App = SimpleNamespace(GuiUp=True)
        try:
            proxy.migrationScript5(joint)
        finally:
            JointObject.App = original_app

        self.assertIn("App::SuppressibleExtensionPython", joint.extensions)

    def test_joint_deferred_view_provider_is_attached_on_recompute(self):
        """A delayed GUI object receives the joint provider and suppressible extension."""
        class DeferredViewObject:
            def __init__(self):
                self.Proxy = None
                self.extensions = set()

            def hasExtension(self, extension):
                return extension in self.extensions

            def addExtension(self, extension):
                self.extensions.add(extension)

        joint = SimpleNamespace(ViewObject=DeferredViewObject())
        proxy = JointObject.Joint.__new__(JointObject.Joint)
        original_app = JointObject.App
        JointObject.App = SimpleNamespace(GuiUp=True)
        try:
            proxy.ensureViewProvider(joint)
        finally:
            JointObject.App = original_app

        self.assertIsInstance(joint.ViewObject.Proxy, JointObject.ViewProviderJoint)
        self.assertIn(
            "Gui::ViewProviderSuppressibleExtensionPython", joint.ViewObject.extensions
        )

    def test_joint_deferred_view_provider_observes_gui_replay(self):
        """The GUI observer installs the provider when native replay creates it."""
        class DeferredViewObject:
            def __init__(self, obj):
                self.Object = obj
                self.Proxy = None
                self.extensions = set()

            def hasExtension(self, extension):
                return extension in self.extensions

            def addExtension(self, extension):
                self.extensions.add(extension)

        document = SimpleNamespace(Name="DeferredViewProviderDocument")
        joint = SimpleNamespace(Document=document, Name="Joint", ViewObject=None)
        gui = SimpleNamespace(
            addDocumentObserver=MagicMock(), removeDocumentObserver=MagicMock()
        )
        original_app = JointObject.App
        original_gui = getattr(JointObject, "Gui", None)
        original_observer = JointObject._deferred_joint_view_provider_observer
        original_pending = dict(JointObject._deferred_joint_view_providers)
        JointObject.App = SimpleNamespace(GuiUp=True)
        JointObject.Gui = gui
        JointObject._deferred_joint_view_providers.clear()
        JointObject._deferred_joint_view_provider_observer = None
        try:
            JointObject.scheduleJointViewProvider(joint)
            observer = JointObject._deferred_joint_view_provider_observer
            self.assertIsNotNone(observer)
            gui.addDocumentObserver.assert_called_once_with(observer)

            view_object = DeferredViewObject(joint)
            joint.ViewObject = view_object
            observer.slotCreatedObject(view_object)
        finally:
            JointObject.App = original_app
            JointObject.Gui = original_gui
            JointObject._deferred_joint_view_providers.clear()
            JointObject._deferred_joint_view_providers.update(original_pending)
            JointObject._deferred_joint_view_provider_observer = original_observer

        self.assertIsInstance(view_object.Proxy, JointObject.ViewProviderJoint)
        self.assertIn("Gui::ViewProviderSuppressibleExtensionPython", view_object.extensions)
        gui.removeDocumentObserver.assert_called_once_with(observer)

    def test_joint_deferred_view_provider_request_can_be_cancelled(self):
        """A failed creation removes its replay request before its name is reused."""
        document = SimpleNamespace(Name="DeferredCancellationDocument")
        joint = SimpleNamespace(Document=document, Name="Joint", ViewObject=None)
        gui = SimpleNamespace(
            addDocumentObserver=MagicMock(), removeDocumentObserver=MagicMock()
        )
        original_app = JointObject.App
        original_gui = getattr(JointObject, "Gui", None)
        JointObject.App = SimpleNamespace(GuiUp=True)
        JointObject.Gui = gui
        JointObject._deferred_joint_view_providers.clear()
        JointObject._deferred_joint_view_provider_observer = None
        try:
            JointObject.scheduleJointViewProvider(joint)
            observer = JointObject._deferred_joint_view_provider_observer
            JointObject.cancelScheduledJointViewProvider(joint)
        finally:
            JointObject.App = original_app
            JointObject.Gui = original_gui
            JointObject._deferred_joint_view_providers.clear()
            JointObject._deferred_joint_view_provider_observer = None

        self.assertNotIn((document.Name, joint.Name), JointObject._deferred_joint_view_providers)
        gui.removeDocumentObserver.assert_called_once_with(observer)

    def test_joint_deferred_view_provider_is_skipped_headless(self):
        """Headless recomputes never access a view object."""
        joint = SimpleNamespace(ViewObject=None)
        proxy = JointObject.Joint.__new__(JointObject.Joint)
        original_app = JointObject.App
        JointObject.App = SimpleNamespace(GuiUp=False)
        try:
            proxy.ensureViewProvider(joint)
        finally:
            JointObject.App = original_app

    def test_api_joint_provider_is_attached_after_native_gui_publication(self):
        """Native compatibility replay publishes a deferred joint ViewObject."""
        if not App.GuiUp:
            self.skipTest("requires a native GUI document publication")

        first = self.assembly.newObject("Part::Box", "DeferredProviderFirst")
        second = self.assembly.newObject("Part::Box", "DeferredProviderSecond")
        self.doc.recompute()
        self.assertFalse(self.doc.mustExecute())
        created = []

        def create_joint():
            joint = Assembly.createJoint(
                self.assembly,
                "Fixed",
                Assembly.makeJointReference(first, "Face6", "Vertex7"),
                Assembly.makeJointReference(second, "Face6", "Vertex7"),
                solve=False,
                presolve=False,
                recompute=False,
            )
            self.assertIsNone(joint.ViewObject)
            created.append(joint)
            return True

        result = self.doc.commitCompatibilityMutation(
            create_joint, structural=True, recompute=False
        )
        self.assertTrue(result.get("committed"), result)
        joint = created[0]
        self.assertIsNotNone(joint.ViewObject)
        self.assertIsInstance(joint.ViewObject.Proxy, JointObject.ViewProviderJoint)
        self.assertTrue(
            joint.ViewObject.hasExtension("Gui::ViewProviderSuppressibleExtensionPython")
        )

    def test_api_solve_false_does_not_move_component(self):
        """Create a joint without forcing a solve."""
        operation = "Create Joint through API without solve"
        _msg("  Test '{}'".format(operation))

        fixed_box = self.assembly.newObject("Part::Box", "ApiFixedGround")
        moving_box = self.assembly.newObject("Part::Box", "ApiMovingNoSolve")
        initial_placement = App.Placement(App.Vector(40, 50, 60), App.Rotation(15, 25, 35))
        moving_box.Placement = initial_placement
        self.doc.recompute()

        Assembly.createGroundedJoint(self.assembly, fixed_box, recompute=False)
        Assembly.createJoint(
            self.assembly,
            "Fixed",
            Assembly.makeJointReference(fixed_box, "Face6", "Vertex7"),
            Assembly.makeJointReference(moving_box, "Face6", "Vertex7"),
            solve=False,
            presolve=False,
            recompute=False,
        )

        self.assertTrue(moving_box.Placement.isSame(initial_placement, 1e-6))

    def test_solve_assembly(self):
        """Test solving an assembly."""
        operation = "Solve assembly"
        _msg("  Test '{}'".format(operation))

        box = self.assembly.newObject("Part::Box", "Box")
        box.Length = 10
        box.Width = 10
        box.Height = 10
        box.Placement = App.Placement(App.Vector(10, 20, 30), App.Rotation(15, 25, 35))

        box2 = self.assembly.newObject("Part::Box", "Box")
        box2.Length = 10
        box2.Width = 10
        box2.Height = 10
        box2.Placement = App.Placement(App.Vector(40, 50, 60), App.Rotation(45, 55, 65))

        ground = self.jointgroup.newObject("App::FeaturePython", "GroundedJoint")
        JointObject.GroundedJoint(ground, box2)

        joint = self.jointgroup.newObject("App::FeaturePython", "testJoint")
        JointObject.Joint(joint, 0)

        refs = [
            [box2, ["Face6", "Vertex7"]],
            [box, ["Face6", "Vertex7"]],
        ]

        joint.Proxy.setJointConnectors(joint, refs)

        self.assertTrue(box.Placement.isSame(box2.Placement, 1e-6), "'{}'".format(operation))
