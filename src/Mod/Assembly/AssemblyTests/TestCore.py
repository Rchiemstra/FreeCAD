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
import os
import tempfile
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
    def _presolve_joint(self, joint_type, fixed, moving, moving_first=False):
        joint = self.jointgroup.newObject("App::FeaturePython", "PresolveJoint")
        JointObject.Joint(joint, JointObject.JointTypes.index(joint_type))
        parts = (moving, fixed) if moving_first else (fixed, moving)
        joint.Reference1 = (parts[0], ["", ""])
        joint.Reference2 = (parts[1], ["", ""])
        return joint

    def _presolve_parts(self):
        self.assembly.Type = "Assembly"
        fixed = self.assembly.newObject("Part::Box", "FixedPart")
        moving = self.assembly.newObject("Part::Box", "MovingPart")
        ground = self.jointgroup.newObject("App::FeaturePython", "GroundedJoint")
        JointObject.GroundedJoint(ground, fixed)
        return fixed, moving

    def test_presolve_keeps_free_axial_placement(self):
        fixed, moving = self._presolve_parts()
        for kind in ("Slider", "Cylindrical"):
            for rotation in (App.Rotation(), App.Rotation(15, 25, 35)):
                for distance in (-35, 35):
                    for moving_first in (False, True):
                        for flipped in (False, True):
                            with self.subTest(
                                kind=kind,
                                rotation=rotation,
                                distance=distance,
                                moving_first=moving_first,
                                flipped=flipped,
                            ):
                                fixed.Placement = App.Placement(App.Vector(10, 20, 30), rotation)
                                twist = App.Rotation(
                                    App.Vector(0, 0, 1), 37 if kind == "Cylindrical" else 0
                                )
                                if flipped:
                                    twist = twist * App.Rotation(App.Vector(1, 0, 0), 180)
                                moving.Placement = fixed.Placement * App.Placement(
                                    App.Vector(0, 0, distance), twist
                                )
                                before = moving.Placement
                                joint = self._presolve_joint(kind, fixed, moving, moving_first)
                                try:
                                    self.assertTrue(joint.Proxy.matchJCS(joint))
                                    self.assertTrue(moving.Placement.isSame(before, 1e-6))
                                finally:
                                    self.doc.removeObject(joint.Name)

    def test_presolve_aligns_constrained_axes_with_offset_connectors(self):
        fixed, moving = self._presolve_parts()
        fixed.Placement = App.Placement(App.Vector(10, 20, 30), App.Rotation(15, 25, 35))
        fixed_before = fixed.Placement
        connector1 = App.Placement(App.Vector(2, 3, 4), App.Rotation(10, 20, 30))
        connector2 = App.Placement(App.Vector(5, 6, 7), App.Rotation(30, 20, 10))
        fixed_jcs = fixed.Placement * connector1
        for kind in ("Slider", "Cylindrical"):
            with self.subTest(kind=kind):
                twist = App.Rotation(App.Vector(0, 0, 1), 37)
                swing = App.Rotation(App.Vector(1, 0, 0), 25)
                moving.Placement = (
                    fixed_jcs
                    * App.Placement(App.Vector(8, -9, 35), swing * twist)
                    * connector2.inverse()
                )
                before = moving.Placement
                joint = self._presolve_joint(kind, fixed, moving)
                try:
                    joint.Detach1 = True
                    joint.Detach2 = True
                    joint.Placement1 = connector1
                    joint.Placement2 = connector2
                    self.assertTrue(joint.Proxy.matchJCS(joint))
                    relative = fixed_jcs.inverse() * (moving.Placement * connector2)
                    self.assertTrue(relative.Base.isEqual(App.Vector(0, 0, 35), 1e-6))
                    expected = twist if kind == "Cylindrical" else App.Rotation()
                    self.assertTrue(relative.Rotation.isSame(expected, 1e-6))
                    self.assertTrue(fixed.Placement.isSame(fixed_before, 1e-6))
                    joint.Proxy.undoPreSolve(joint)
                    self.assertTrue(moving.Placement.isSame(before, 1e-6))
                finally:
                    self.doc.removeObject(joint.Name)

    def test_axial_presolve_on_expression_recompute(self):
        fixed, moving = self._presolve_parts()
        variables = self.doc.addObject("App::VarSet", "Offsets")
        variables.addProperty("App::PropertyLength", "ConnectorOffset")
        for kind in ("Slider", "Cylindrical"):
            with self.subTest(kind=kind):
                variables.ConnectorOffset = 1
                joint = self._presolve_joint(kind, fixed, moving)
                try:
                    joint.setExpression("Offset2.Base.x", "Offsets.ConnectorOffset")
                    self.doc.recompute()
                    twist = App.Rotation(App.Vector(0, 0, 1), 37 if kind == "Cylindrical" else 0)
                    moving.Placement = App.Placement(App.Vector(), twist) * App.Placement(
                        App.Vector(-1, 0, 35), App.Rotation()
                    )
                    variables.ConnectorOffset = 2
                    with patch.object(
                        joint.Proxy, "preSolve", wraps=joint.Proxy.preSolve
                    ) as presolve:
                        self.doc.recompute()
                    self.assertGreater(presolve.call_count, 0)
                    self.assertAlmostEqual(moving.Placement.Base.z, 35, places=6)
                    self.assertTrue(moving.Placement.Rotation.isSame(twist, 1e-6))
                    relative = fixed.Placement.inverse() * (moving.Placement * joint.Placement2)
                    self.assertAlmostEqual(relative.Base.x, 0, places=6)
                    self.assertAlmostEqual(relative.Base.y, 0, places=6)
                    self.assertTrue(fixed.Placement.isSame(App.Placement(), 1e-6))
                finally:
                    self.doc.removeObject(joint.Name)

    def test_slider_chain_expression_recompute_keeps_distinct_offsets(self):
        fixed, moving = self._presolve_parts()
        parts = [fixed, moving] + [self.assembly.newObject("Part::Box", "Panel") for _ in range(2)]
        variables = self.doc.addObject("App::VarSet", "Offsets")
        variables.addProperty("App::PropertyLength", "ConnectorOffset")
        for previous, part in zip(parts, parts[1:]):
            joint = self._presolve_joint("Slider", previous, part)
            joint.setExpression("Offset2.Base.x", "Offsets.ConnectorOffset")
        self.doc.recompute()
        distances = (0, 35, -20, 80)
        for part, distance in zip(parts, distances):
            part.Placement.Base = App.Vector(0, 0, distance)
        variables.ConnectorOffset = 2
        self.doc.recompute()
        for index, (part, distance) in enumerate(zip(parts, distances)):
            self.assertTrue(part.Placement.Base.isEqual(App.Vector(-2 * index, 0, distance), 1e-6))
        before = [part.Placement for part in parts]
        unrelated = self.doc.addObject("App::FeaturePython", "Unrelated")
        self.doc.removeObject(unrelated.Name)
        self.doc.recompute()
        for part, placement in zip(parts, before):
            self.assertTrue(part.Placement.isSame(placement, 1e-6))

    def test_fixed_and_revolute_presolve_still_match_connectors(self):
        fixed, moving = self._presolve_parts()
        fixed.Placement = App.Placement(App.Vector(10, 20, 30), App.Rotation(15, 25, 35))
        for kind in ("Fixed", "Revolute"):
            with self.subTest(kind=kind):
                moving.Placement = fixed.Placement * App.Placement(
                    App.Vector(8, -9, 35), App.Rotation(20, 30, 40)
                )
                joint = self._presolve_joint(kind, fixed, moving)
                try:
                    self.assertTrue(joint.Proxy.matchJCS(joint))
                    self.assertTrue(moving.Placement.isSame(fixed.Placement, 1e-6))
                finally:
                    self.doc.removeObject(joint.Name)

    def test_component_count_for_link_array(self):
        source = self.doc.addObject("Part::Box", "ArraySource")
        array = self.assembly.newObject("App::Link", "Array")
        array.LinkedObject = source
        array.ElementCount = 3
        array.ShowElement = False
        self.doc.recompute()
        self.assertEqual(len(array.ElementList), 0)
        self.assertEqual(UtilsAssembly.number_of_components_in(self.assembly), 3)

        array.ShowElement = True
        self.doc.recompute()
        self.assertEqual(UtilsAssembly.number_of_components_in(self.assembly), 3)
        array.ElementList[1].Suppressed = True
        self.doc.recompute()
        self.assertEqual(UtilsAssembly.number_of_components_in(self.assembly), 2)
        array.ElementList[1].Suppressed = False
        self.doc.recompute()
        self.assertEqual(UtilsAssembly.number_of_components_in(self.assembly), 3)

    def test_generated_array_is_one_component(self):
        source = self.doc.addObject("Part::Box", "ArraySource")
        array = self.assembly.newObject("Part::LinkArrayLinear", "Array")
        array.LinkedObject = source
        array.Occurrences = 3
        self.doc.recompute()
        self.assertEqual(UtilsAssembly.number_of_components_in(self.assembly), 1)
        self.assertEqual(UtilsAssembly.getSubMovingParts(array, False), [array])
        self.assertEqual(UtilsAssembly.getObject((array, ["1.Face1"])), array)

    def test_suppressed_link_elements_are_not_movable(self):
        source = self.doc.addObject("Part::Box", "ArraySource")
        array = self.assembly.newObject("App::Link", "Array")
        array.LinkedObject = source
        array.ElementCount = 3
        self.doc.recompute()
        element = array.ElementList[1]
        element.Suppressed = True
        self.doc.recompute()
        self.assertNotIn(element, UtilsAssembly.getMovablePartsWithin(array))
        element.Suppressed = False
        self.doc.recompute()
        self.assertIn(element, UtilsAssembly.getMovablePartsWithin(array))

    def test_assembly_link_synchronizes_element_suppression(self):
        source = self.doc.addObject("Part::Box", "ArraySource")
        array = self.assembly.newObject("App::Link", "Array")
        array.LinkedObject = source
        array.ElementCount = 3
        array.ElementList[1].Suppressed = True
        parent = self.doc.addObject("Assembly::AssemblyObject", "ParentAssembly")
        instance = parent.newObject("Assembly::AssemblyLink", "Instance")
        instance.LinkedObject = self.assembly
        self.doc.recompute()
        local_array = next(obj for obj in instance.Group if obj.TypeId == "App::Link")
        self.assertTrue(local_array.ElementList[1].Suppressed)
        array.ElementList[1].Suppressed = False
        instance.touch()
        self.doc.recompute()
        self.assertFalse(local_array.ElementList[1].Suppressed)

    def test_assembly_link_maps_generated_array_joint(self):
        self._check_generated_array_joint("Face1")

    def test_assembly_link_maps_generated_array_whole_element_joint(self):
        self._check_generated_array_joint("")

    def _check_generated_array_joint(self, sub):
        source = self.doc.addObject("Part::Box", "ArraySource")
        array = self.assembly.newObject("Part::LinkArrayLinear", "Array")
        array.LinkedObject = source
        array.Occurrences = 3
        array.ShowElement = True
        self.doc.recompute()
        joint = self.jointgroup.newObject("App::FeaturePython", "Joint")
        JointObject.Joint(joint, 0)
        joint.Reference1 = (array.ElementList[1], [sub])
        joint.Reference2 = (array.ElementList[2], ["Face1"])
        parent = self.doc.addObject("Assembly::AssemblyObject", "ParentAssembly")
        instance = parent.newObject("Assembly::AssemblyLink", "Instance")
        instance.LinkedObject = self.assembly
        instance.Rigid = False
        self.doc.recompute()
        local_array = next(obj for obj in instance.Group if obj.TypeId == "App::Link")
        local_group = next(obj for obj in instance.Group if obj.TypeId == "Assembly::JointGroup")
        local_joint = local_group.Group[0]
        self.assertEqual(local_joint.Reference1, (local_array, ["1." + sub]))
        self.assertIsNotNone(local_array.getSubObject("1." + sub))

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

    def test_api_rejects_components_outside_the_assembly(self):
        """A joint on an object outside the assembly is ignored by the solver.

        The joint was created and the solve reported success while the
        component never moved, so the API must refuse it.
        """
        member = self.assembly.newObject("Part::Box", "ApiMemberBox")
        outsider = self.doc.addObject("Part::Box", "ApiOutsideBox")
        self.doc.recompute()

        with self.assertRaisesRegex(Assembly.JointCreationError, "ApiOutsideBox"):
            Assembly.createGroundedJoint(self.assembly, outsider, recompute=False)
        with self.assertRaisesRegex(Assembly.JointCreationError, "ApiOutsideBox"):
            Assembly.createJoint(
                self.assembly,
                "Fixed",
                Assembly.makeJointReference(member, "Face6", "Vertex7"),
                Assembly.makeJointReference(outsider, "Face6", "Vertex7"),
                solve=False,
                presolve=False,
                recompute=False,
            )
        self.assertIsNone(self.doc.getObject("GroundedJoint"))
        self.assertIsNone(self.doc.getObject("Joint"))

    def test_api_accepts_components_nested_in_the_assembly(self):
        """Components inside a Part within the assembly are members too."""
        part = self.assembly.newObject("App::Part", "ApiNestedPart")
        nested = part.newObject("Part::Box", "ApiNestedBox")
        self.doc.recompute()

        joint = Assembly.createGroundedJoint(self.assembly, nested, recompute=False)

        self.assertEqual(joint.ObjectToGround, nested)

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

    def test_rack_pinion_with_slider_offset(self):
        """Rack and pinion joint whose rack slider has a yaw offset, see
        github.com/freecad/freecad/issues/17563"""
        operation = "Rack and pinion with slider offset"
        _msg("  Test '{}'".format(operation))

        ground = self.assembly.newObject("Part::Box", "Ground")
        ground.Length = 200
        ground.Width = 200
        ground.Height = 10

        rack = self.assembly.newObject("Part::Box", "Rack")
        rack.Length = 10
        rack.Width = 100
        rack.Height = 10
        rack.Placement.Base = App.Vector(50, 0, 10)

        pinion = self.assembly.newObject("Part::Cylinder", "Pinion")
        pinion.Radius = 10
        pinion.Height = 10
        pinion.Placement.Base = App.Vector(30, 50, 10)
        self.doc.recompute()

        # Attach to real faces, but specify the JCS explicitly so the test does
        # not depend on OpenCASCADE's orientation of those faces.
        along_rack = App.Rotation(App.Vector(1, 0, 0), -90)
        yaw_offset = App.Rotation(App.Vector(0, 0, 1), -90)

        slider = self.jointgroup.newObject("App::FeaturePython", "Slider")
        JointObject.Joint(slider, JointObject.JointTypes.index("Slider"))
        slider.Detach1 = True
        slider.Detach2 = True
        slider.Reference1 = (ground, ["Face1"])
        slider.Reference2 = (rack, ["Face1"])
        slider.Placement1 = App.Placement(App.Vector(), along_rack)
        slider.Placement2 = App.Placement(App.Vector(), along_rack * yaw_offset)

        rackPinion = self.jointgroup.newObject("App::FeaturePython", "RackPinion")
        JointObject.Joint(rackPinion, JointObject.JointTypes.index("RackPinion"))
        rackPinion.Detach1 = True
        rackPinion.Reference1 = (rack, ["Face1"])
        rackPinion.Reference2 = (pinion, ["Face1"])
        rackPinion.Placement1 = App.Placement(App.Vector(), along_rack)
        rackPinion.Distance = 10

        self.doc.recompute()

        slider_axis = UtilsAssembly.getJcsGlobalPlc(
            slider.Placement2, slider.Reference2
        ).Rotation.multVec(App.Vector(0, 0, 1))
        rack_axis = UtilsAssembly.getJcsGlobalPlc(
            rackPinion.Placement1, rackPinion.Reference1
        ).Rotation.multVec(App.Vector(0, 0, 1))
        self.assertLess(slider_axis.cross(rack_axis).Length, 1e-7)

        # The rack and pinion joint must reach the solver: it is silently dropped when
        # the rack cannot be identified from its slider. Look for its pitchRadius field
        # rather than its type name, which the solver derives from the compiler-specific
        # typeid name and is only readable with MSVC.
        with tempfile.TemporaryDirectory() as temp_dir:
            fileName = os.path.join(temp_dir, "rackPinion.asmt")
            self.assembly.exportAsASMT(fileName)
            with open(fileName) as asmt:
                content = asmt.read()
        self.assertIn(
            "pitchRadius",
            content,
            "'{}' failed - joint not exported; slider state: {}, rack-pinion state: {}; "
            "exported assembly: {}".format(
                operation, slider.State, rackPinion.State, content[-2500:]
            ),
        )

    def test_api_distance_joint_moves_component(self):
        """A distance joint through the API moves the free component."""
        base = self.assembly.newObject("Part::Box", "DistBase")
        base.Length = 30
        base.Width = 20
        base.Height = 10
        lid = self.assembly.newObject("Part::Box", "DistLid")
        lid.Length = 20
        lid.Width = 12
        lid.Height = 8
        lid.Placement = App.Placement(App.Vector(40, 5, 30), App.Rotation(App.Vector(0, 1, 0), 15))
        self.doc.recompute()
        initial = App.Placement(lid.Placement)

        Assembly.createGroundedJoint(self.assembly, base, recompute=False)
        Assembly.createJoint(
            self.assembly,
            "Distance",
            Assembly.makeJointReference(base, "Face6"),
            Assembly.makeJointReference(lid, "Face6"),
            Distance=5,
            solve=True,
        )

        self.assertFalse(
            lid.Placement.isSame(initial, 1e-6),
            "distance joint left the lid unmoved at {}".format(lid.Placement),
        )

    def test_api_revolute_on_circular_edges_is_coaxial(self):
        """A revolute on two circular edges makes those edges coaxial.

        makeJointReference stores [element, element]. That is the workbench
        reference: the element plus the vertex or element used for the
        placement. Edge2 of a Part::Cylinder is the bottom rim, and its local
        center is the component origin, so the joint placement is the identity.
        """
        base = self.assembly.newObject("Part::Box", "Base")
        base.Length = 30
        base.Width = 20
        base.Height = 10
        lid = self.assembly.newObject("Part::Box", "Lid")
        lid.Length = 20
        lid.Width = 12
        lid.Height = 8
        lid.Placement = App.Placement(App.Vector(40, 5, 30), App.Rotation())
        pin = self.assembly.newObject("Part::Cylinder", "Pin")
        pin.Radius = 3
        pin.Height = 18
        pin.Placement = App.Placement(App.Vector(10, -15, 0), App.Rotation())
        pin2 = self.assembly.newObject("Part::Cylinder", "Pin2")
        pin2.Radius = 3
        pin2.Height = 16
        pin2.Placement = App.Placement(
            App.Vector(-10, 10, 5), App.Rotation(App.Vector(1, 0, 0), 25)
        )
        self.doc.recompute()

        ref1 = Assembly.makeJointReference(pin, "Edge2")
        ref2 = Assembly.makeJointReference(pin2, "Edge2")
        self.assertEqual(list(ref1[1]), ["Edge2", "Edge2"])
        self.assertEqual(list(ref2[1]), ["Edge2", "Edge2"])

        Assembly.createGroundedJoint(self.assembly, base, recompute=False)
        Assembly.createJoint(
            self.assembly,
            "Distance",
            Assembly.makeJointReference(base, "Face6"),
            Assembly.makeJointReference(lid, "Face6"),
            Distance=5,
            solve=True,
            recompute=False,
        )
        joint = Assembly.createJoint(
            self.assembly, "Revolute", ref1, ref2, solve=True, recompute=False
        )

        expected1 = UtilsAssembly.findPlacement(ref1, False)
        expected2 = UtilsAssembly.findPlacement(ref2, False)
        self.assertTrue(
            joint.Placement1.isSame(expected1, 1e-6),
            "Placement1 {} != edge placement {}".format(joint.Placement1, expected1),
        )
        self.assertTrue(
            joint.Placement2.isSame(expected2, 1e-6),
            "Placement2 {} != edge placement {}".format(joint.Placement2, expected2),
        )
        center1, axis1 = _shape_circle(pin, "Edge2")
        center2, axis2 = _shape_circle(pin2, "Edge2")
        self.assertTrue(
            _axes_coaxial(center1, axis1, center2, axis2),
            "pins are not coaxial: {} axis {} vs {} axis {}".format(center1, axis1, center2, axis2),
        )

    def test_api_revolute_on_offset_rim_stores_circle_center(self):
        """A circular edge whose center is not the origin is not stored at the origin."""
        pin = self.assembly.newObject("Part::Cylinder", "TopPin")
        pin.Radius = 3
        pin.Height = 18
        pin.Placement = App.Placement(App.Vector(10, -15, 0), App.Rotation())
        pin2 = self.assembly.newObject("Part::Cylinder", "TopPin2")
        pin2.Radius = 3
        pin2.Height = 16
        pin2.Placement = App.Placement(
            App.Vector(-10, 10, 5), App.Rotation(App.Vector(1, 0, 0), 25)
        )
        anchor = self.assembly.newObject("Part::Box", "TopAnchor")
        self.doc.recompute()

        ref1 = Assembly.makeJointReference(pin, "Edge3")
        ref2 = Assembly.makeJointReference(pin2, "Edge3")
        Assembly.createGroundedJoint(self.assembly, anchor, recompute=False)
        joint = Assembly.createJoint(
            self.assembly, "Revolute", ref1, ref2, solve=True, recompute=False
        )

        self.assertGreater(joint.Placement1.Base.z, 1.0, joint.Placement1)
        self.assertGreater(joint.Placement2.Base.z, 1.0, joint.Placement2)
        center1, axis1 = _shape_circle(pin, "Edge3")
        center2, axis2 = _shape_circle(pin2, "Edge3")
        self.assertTrue(
            _axes_coaxial(center1, axis1, center2, axis2),
            "top rims are not coaxial: {} axis {} vs {} axis {}".format(
                center1, axis1, center2, axis2
            ),
        )

    def _fixed_distance_fixture(self, distance):
        base = self.assembly.newObject("Part::Box", "DistanceBase")
        lid = self.assembly.newObject("Part::Box", "DistanceLid")
        lid.Placement.Base = App.Vector(30, 0, 0)
        self.doc.recompute()
        Assembly.createGroundedJoint(self.assembly, base, recompute=False)
        refs = (Assembly.makeJointReference(base, "Face1"),
                Assembly.makeJointReference(lid, "Face1"))
        Assembly.createJoint(self.assembly, "Fixed", *refs, solve=True)
        joint = Assembly.createJoint(self.assembly, "Distance", *refs,
                                     Distance=distance, solve=False, presolve=False)
        return joint

    def test_api_satisfied_fixed_distance_solves(self):
        joint = self._fixed_distance_fixture(0)
        self.assertEqual(self.assembly.solve(False), 0)
        self.assertIsNone(UtilsAssembly.jointRelationError(joint))

    def test_api_contradictory_fixed_distance_fails(self):
        joint = self._fixed_distance_fixture(100)
        status = self.assembly.solve(False)
        details = [(j.Name, j.JointType, UtilsAssembly.jointRelationError(j))
                   for j in self.assembly.Joints if hasattr(j, "JointType")]
        self.assertEqual(status, -3, (UtilsAssembly.__file__, details))
        self.assertTrue(UtilsAssembly.violatedJointLabels(self.assembly))

    def test_recompute_conflicting_assembly_reports_error(self):
        self._fixed_distance_fixture(100)
        prefs = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        previous = prefs.GetBool("SolveOnRecompute", True)
        try:
            prefs.SetBool("SolveOnRecompute", True)
            self.assembly.touch()
            self.doc.recompute()
            self.assertIn("Invalid", self.assembly.State)
            self.assertEqual(self.assembly.solve(False), -3)
            prefs.SetBool("SolveOnRecompute", False)
            self.assembly.touch()
            self.doc.recompute()
            self.assertNotIn("Invalid", self.assembly.State)
        finally:
            prefs.SetBool("SolveOnRecompute", previous)

    def test_validator_failure_never_reports_success(self):
        self._fixed_distance_fixture(0)
        validators = (
            patch.dict("sys.modules", {"UtilsAssembly": None}),
            patch("UtilsAssembly.violatedJointLabels", side_effect=RuntimeError("C1 validator error")),
            patch("UtilsAssembly.violatedJointLabels", return_value=None),
        )
        for validator in validators:
            with self.subTest(validator=validator), validator:
                self.assertEqual(self.assembly.solve(False), -7)
        self.assertEqual(self.assembly.solve(False), 0)

    def _solve_placement_snapshot(self):
        # Include joint connector placements, link elements, and touched states.
        return {
            obj.Name: (
                {name: tuple(getattr(obj, name).toMatrix().A)
                 for name in obj.PropertiesList
                 if obj.getTypeIdOfProperty(name) == "App::PropertyPlacement"},
                list(obj.State),
            )
            for obj in self.doc.Objects
        }

    def _displace_solve_lid(self):
        lid = self.doc.getObject("DistanceLid")
        placement = lid.Placement
        placement.Base += App.Vector(3, 2, 1)
        lid.Placement = placement
        return lid

    def _placement_solve_fixture(self):
        # Keep the movable success/validator controls free of redundant
        # Fixed+Distance constraints, which can make MbD drop grounding axes.
        redundant = self._fixed_distance_fixture(0)
        self.doc.removeObject(redundant.Name)
        self.assertEqual(self.assembly.solve(False), 0)

    def test_rejected_relations_preserve_placements(self):
        self._fixed_distance_fixture(100)
        self._displace_solve_lid()
        before = self._solve_placement_snapshot()
        self.assertEqual(self.assembly.solve(False), -3)
        self.assertEqual(self._solve_placement_snapshot(), before)

    def test_validator_exception_preserves_placements(self):
        self._placement_solve_fixture()
        self._displace_solve_lid()
        before = self._solve_placement_snapshot()
        with patch("UtilsAssembly.violatedJointLabels",
                   side_effect=RuntimeError("C2 validator")) as validator:
            self.assertEqual(self.assembly.solve(False), -7)
            validator.assert_called_once()
        self.assertEqual(self._solve_placement_snapshot(), before)

    def test_validator_non_list_preserves_placements_and_file_state(self):
        self._placement_solve_fixture()
        self._displace_solve_lid()
        with tempfile.TemporaryDirectory() as directory:
            self.doc.saveAs(os.path.join(directory, "atomic.FCStd"))
            before = self._solve_placement_snapshot()
            self.assertFalse(self.doc.hasPendingFileChanges())
            with patch("UtilsAssembly.violatedJointLabels", return_value=None) as validator:
                self.assertEqual(self.assembly.solve(False), -7)
                validator.assert_called_once()
            self.assertEqual(self._solve_placement_snapshot(), before)
            self.assertFalse(self.doc.hasPendingFileChanges())

    def test_successful_solve_commits_placements(self):
        self._placement_solve_fixture()
        lid = self._displace_solve_lid()
        before = lid.Placement
        # The validator must see the candidate, rather than the displaced input.
        self.assertTrue(UtilsAssembly.violatedJointLabels(self.assembly))
        with tempfile.TemporaryDirectory() as directory:
            self.doc.saveAs(os.path.join(directory, "accepted.FCStd"))
            self.assertFalse(self.doc.hasPendingFileChanges())
            self.assertEqual(self.assembly.solve(False), 0)
            self.assertTrue(self.doc.hasPendingFileChanges())
        self.assertFalse(lid.Placement.isSame(before))
        self.assertEqual(UtilsAssembly.violatedJointLabels(self.assembly), [])

    def test_rejected_solve_tracks_link_group_normalization(self):
        source = self.doc.addObject("Part::Box", "LinkSource")
        base = self.assembly.newObject("Part::Box", "LinkGround")
        group = self.assembly.newObject("App::Link", "LinkGroup")
        group.LinkedObject = source
        group.ElementCount = 2
        self.doc.recompute()
        Assembly.createGroundedJoint(self.assembly, base, recompute=False)
        group.Placement = App.Placement(App.Vector(43, 17, 9), App.Rotation())
        positions = [group.Placement * e.Placement for e in group.ElementList]
        with tempfile.TemporaryDirectory() as directory:
            self.doc.saveAs(os.path.join(directory, "normalization.FCStd"))
            with patch("UtilsAssembly.violatedJointLabels", return_value=None):
                self.assertEqual(self.assembly.solve(False), -7)
            self.assertTrue(group.Placement.isIdentity())
            for element, position in zip(group.ElementList, positions):
                self.assertTrue(element.Placement.isSame(position))
            self.assertTrue(self.doc.hasPendingFileChanges())

    def test_rejected_solve_tracks_grounding_creation(self):
        part = self.assembly.newObject("Part::Box", "LockedPart")
        self.doc.recompute()
        part.setEditorMode("Placement", 1)
        self.assertEqual([o for o in self.jointgroup.Group if hasattr(o, "ObjectToGround")], [])
        with tempfile.TemporaryDirectory() as directory:
            self.doc.saveAs(os.path.join(directory, "grounding.FCStd"))
            with patch("UtilsAssembly.violatedJointLabels", return_value=None):
                self.assertEqual(self.assembly.solve(False), -7)
            self.assertEqual(len([o for o in self.jointgroup.Group if hasattr(o, "ObjectToGround")]), 1)
            self.assertTrue(self.doc.hasPendingFileChanges())

    def test_rejected_solve_tracks_grounding_deletion(self):
        self._placement_solve_fixture()
        part = self.doc.getObject("DistanceBase")
        part.setEditorMode("Placement", 0)
        self.assertEqual(len([o for o in self.jointgroup.Group if hasattr(o, "ObjectToGround")]), 1)
        with tempfile.TemporaryDirectory() as directory:
            self.doc.saveAs(os.path.join(directory, "ungrounding.FCStd"))
            with patch("UtilsAssembly.violatedJointLabels", return_value=None):
                self.assertNotEqual(self.assembly.solve(False), 0)
            self.assertEqual([o for o in self.jointgroup.Group if hasattr(o, "ObjectToGround")], [])
            self.assertTrue(self.doc.hasPendingFileChanges())

    def _rejected_swapped_joint(self, kind):
        ground = self.assembly.newObject("Part::Box", "Ground")
        rack = self.assembly.newObject("Part::Box", "Rack")
        pinion = self.assembly.newObject("Part::Cylinder", "Pinion")
        self.doc.recompute()
        Assembly.createGroundedJoint(self.assembly, ground, recompute=False)

        def joint(kind, first, second, rotation):
            obj = self.jointgroup.newObject("App::FeaturePython", kind)
            JointObject.Joint(obj, JointObject.JointTypes.index(kind))
            obj.Detach1 = obj.Detach2 = True
            obj.Reference1 = (first, ["Face1"])
            obj.Reference2 = (second, ["Face1"])
            obj.Placement1 = obj.Placement2 = App.Placement(App.Vector(), rotation)
            return obj

        # Reverse the rack/screw endpoints so solver preparation must swap them.
        with patch("JointObject.solveIfAllowed"):
            axis = App.Rotation(App.Vector(0, 0, 1), App.Vector(1, 0, 0))
            joint("Slider", ground, rack, axis if kind == "RackPinion" else App.Rotation())
            joint("Revolute", ground, pinion, App.Rotation())
            coupling = joint(kind, pinion, rack, App.Rotation())
            coupling.Placement2 = App.Placement(App.Vector(), axis) if kind == "RackPinion" else App.Placement()
            coupling.Distance = 10
        references = (coupling.Reference1, coupling.Reference2)
        before = self._solve_placement_snapshot()
        with patch("UtilsAssembly.violatedJointLabels", return_value=None):
            self.assertNotEqual(self.assembly.solve(False), 0)
        self.assertEqual((coupling.Reference1, coupling.Reference2), references)
        self.assertEqual(self._solve_placement_snapshot(), before)

    def test_rejected_rack_pinion_restores_references(self):
        self._rejected_swapped_joint("RackPinion")

    def test_rejected_screw_restores_references(self):
        self._rejected_swapped_joint("Screw")

    def test_placement_callback_defers_nested_solve(self):
        # Production assemblies carry this discriminator; the bare C++ fixture
        # otherwise makes solveIfAllowed() return without requesting a solve.
        self.assembly.Type = "Assembly"
        self._placement_solve_fixture()
        lid = self._displace_solve_lid()
        assembly = self.assembly
        requests = []

        class Observer:
            def slotChangedObject(self, obj, prop):
                if obj == lid and prop == "Placement" and not requests:
                    requests.append(True)
                    JointObject.solveIfAllowed(assembly)

        observer = Observer()
        preferences = JointObject.Preferences.preferences()
        previous = preferences.GetBool("SolveInJointCreation", True)
        preferences.SetBool("SolveInJointCreation", True)
        App.addDocumentObserver(observer)
        try:
            with patch("UtilsAssembly.violatedJointLabels", return_value=[]) as validator:
                self.assertEqual(assembly.solve(False), 0)
                self.assertEqual(requests, [True])
                validator.assert_called_once()
            self.assertIn("Touched", assembly.State)
        finally:
            App.removeDocumentObserver(observer)
            preferences.SetBool("SolveInJointCreation", previous)

    def test_rejected_solve_preserves_rigid_group_preparation(self):
        self._placement_solve_fixture()
        lid = self.doc.getObject("DistanceLid")
        buddy = self.assembly.newObject("Part::Box", "RigidBuddy")
        buddy.Placement.Base = App.Vector(0, 0, 20)
        group = self.jointgroup.newObject("App::FeaturePython", "RigidGroup")
        JointObject.RigidGroupJoint(group, [lid, buddy])
        self.assertEqual(self.assembly.solve(False), 0)
        original = (lid.Placement, buddy.Placement)
        self._displace_solve_lid()
        before = self._solve_placement_snapshot()
        with patch("UtilsAssembly.violatedJointLabels", side_effect=RuntimeError("C2 rigid")):
            self.assertEqual(self.assembly.solve(False), -7)
        self.assertEqual(self._solve_placement_snapshot(), before)
        # A retry must use the cache from before the rejected preparation.
        self.assertEqual(self.assembly.solve(False), 0)
        self.assertTrue(lid.Placement.isSame(original[0], 1e-7))
        self.assertTrue(buddy.Placement.isSame(original[1], 1e-7))

    def _recompute_solve_failure(self, message):
        prefs = App.ParamGet("User parameter:BaseApp/Preferences/Mod/Assembly")
        previous = prefs.GetBool("SolveOnRecompute", True)
        try:
            prefs.SetBool("SolveOnRecompute", True)
            self.assembly.touch()
            self.doc.recompute()
        finally:
            prefs.SetBool("SolveOnRecompute", previous)
        self.assertIn("Invalid", self.assembly.State)
        self.assertIn(message, self.assembly.getStatusString())

    def _assert_explicit_solve_recovers(self):
        self.assertIn("Invalid", self.assembly.State)
        old_error = self.assembly.getStatusString()
        self.assertEqual(self.assembly.solve(False), 0)
        self.assertNotIn("Invalid", self.assembly.State)
        self.assertNotEqual(self.assembly.getStatusString(), old_error)

    def test_explicit_solve_clears_recompute_validation_error(self):
        self._fixed_distance_fixture(0)
        with patch("UtilsAssembly.violatedJointLabels", side_effect=RuntimeError("C1 validator error")):
            self._recompute_solve_failure("validation could not run")
            self.assertEqual(self.assembly.solve(False), -7)
            self.assertIn("Invalid", self.assembly.State)
        # The validator is restored, with no intervening recompute.
        self._assert_explicit_solve_recovers()

    def test_explicit_solve_clears_recompute_conflict_error(self):
        joint = self._fixed_distance_fixture(100)
        self._recompute_solve_failure("conflicting constraints")
        with patch("JointObject.solveIfAllowed"):
            joint.Distance = 0
        # Repair the constraints, with no intervening recompute.
        self._assert_explicit_solve_recovers()

    def test_explicit_solve_clears_recompute_solver_error(self):
        carrier = self.assembly.newObject("Part::Box", "Carrier")
        gears = [self.assembly.newObject("Part::Box", name) for name in ("Gear1", "Gear2")]
        self.doc.recompute()
        Assembly.createGroundedJoint(self.assembly, carrier, recompute=False)
        carrier_ref = Assembly.makeJointReference(carrier, "Face1")
        refs = [Assembly.makeJointReference(gear, "Face1") for gear in gears]
        for ref in refs:
            Assembly.createJoint(self.assembly, "Revolute", carrier_ref, ref,
                                 solve=False, recompute=False)
        joint = Assembly.createJoint(self.assembly, "Gears", *refs, Distance=10, Distance2=0,
                                     solve=False, presolve=False, recompute=False)
        # A zero gear radius makes MbD throw, exercising SolverFailure (-1).
        self.assertEqual(self.assembly.solve(False), -1)
        self._recompute_solve_failure("Assembly solver failed")
        joint.Distance2 = 10
        self._assert_explicit_solve_recovers()

    def test_explicit_solve_preserves_unrelated_recompute_error(self):
        self._fixed_distance_fixture(0)
        with patch("UtilsAssembly.violatedJointLabels", return_value=None):
            self._recompute_solve_failure("validation could not run")
        self.assembly.addProperty("App::PropertyFloat", "Unrelated")
        self.assembly.addProperty("App::PropertyFloat", "Divisor")
        self.assembly.Divisor = 1
        self.assembly.setExpression("Unrelated", "1 / Divisor")
        self.assembly.Divisor = 0
        self.doc.recompute()
        self.assertIn("Invalid", self.assembly.State)
        error = self.assembly.getStatusString()
        self.assertNotIn("validation could not run", error)
        self.assertEqual(self.assembly.solve(False), 0)
        self.assertIn("Invalid", self.assembly.State)
        self.assertEqual(self.assembly.getStatusString(), error)

    def test_api_flipped_fixed_joint_solves(self):
        """Native Fixed accepts coincident frames with any two axes reversed."""
        for axis in (App.Vector(1, 0, 0), App.Vector(0, 1, 0), App.Vector(0, 0, 1)):
            with self.subTest(axis=axis):
                base = self.assembly.newObject("Part::Box", "FlippedBase")
                lid = self.assembly.newObject("Part::Box", "FlippedLid")
                self.doc.recompute()
                Assembly.createGroundedJoint(self.assembly, base, recompute=False)
                joint = Assembly.createJoint(
                    self.assembly,
                    "Fixed",
                    Assembly.makeJointReference(base, "Face6"),
                    Assembly.makeJointReference(lid, "Face6"),
                    solve=False,
                    presolve=False,
                )
                flip = App.Placement(App.Vector(), App.Rotation(axis, 180))
                lid.Placement = (
                    base.Placement * joint.Placement1 * flip * joint.Placement2.inverse()
                )
                self.assertEqual(self.assembly.solve(False), 0)
                self.assertIsNone(UtilsAssembly.jointRelationError(joint))
                if axis.z == 0:
                    # Presolve deliberately preserves opposite connector Z axes.
                    joint.Proxy.preSolve(joint, False)
                    self.assertEqual(self.assembly.solve(False), 0)
                    self.assertIsNone(UtilsAssembly.jointRelationError(joint))

    def test_api_disconnected_ungrounded_joint_group_solves(self):
        """Unsatisfied joints in an ungrounded island are excluded by MbD."""
        self._fixed_distance_fixture(0)
        base = self.assembly.newObject("Part::Box", "FloatingBase")
        lid = self.assembly.newObject("Part::Box", "FloatingLid")
        lid.Placement.Base = App.Vector(30, 0, 0)
        self.doc.recompute()
        refs = (
            Assembly.makeJointReference(base, "Face1"),
            Assembly.makeJointReference(lid, "Face1"),
        )
        for joint_type in ("Fixed", "Distance"):
            Assembly.createJoint(self.assembly, joint_type, *refs, solve=False, presolve=False)
        self.assertFalse(self.assembly.isPartConnected(base))
        self.assertFalse(self.assembly.isPartConnected(lid))
        self.assertEqual(self.assembly.solve(False), 0)
        self.assertEqual(UtilsAssembly.violatedJointLabels(self.assembly), [])

    def test_api_planar_distance_conflicting_rotation_fails(self):
        """Correct separation cannot hide incompatible grounded plane normals."""
        for angle, expected_status in ((0, 0), (180, 0), (90, -3)):
            with self.subTest(angle=angle):
                base = self.assembly.newObject("Part::Box", "PlanarBase")
                lid = self.assembly.newObject("Part::Box", "PlanarLid")
                lid.Placement.Rotation = App.Rotation(App.Vector(1, 0, 0), angle)
                self.doc.recompute()
                for part in (base, lid):
                    Assembly.createGroundedJoint(self.assembly, part, recompute=False)
                refs = (
                    Assembly.makeJointReference(base, "Face6"),
                    Assembly.makeJointReference(lid, "Face6"),
                )
                plc1 = UtilsAssembly.getJcsGlobalPlc(
                    UtilsAssembly.findPlacement(refs[0], True), refs[0]
                )
                plc2 = UtilsAssembly.getJcsGlobalPlc(
                    UtilsAssembly.findPlacement(refs[1], True), refs[1]
                )
                axis = plc1.Rotation.multVec(App.Vector(0, 0, 1))
                joint = Assembly.createJoint(
                    self.assembly,
                    "Distance",
                    *refs,
                    Distance=(plc2.Base - plc1.Base).dot(axis),
                    solve=False,
                    presolve=False,
                )
                self.assertEqual(self.assembly.solve(False), expected_status)
                self.assertEqual(
                    UtilsAssembly.jointRelationError(joint) is None, expected_status == 0
                )

    def test_api_parallel_and_perpendicular_joints_hold(self):
        """A single parallel or perpendicular joint solves and the relation holds."""
        for joint_type, rotation, limit in (
            ("Parallel", 40, 1.0 - 1e-4),
            ("Perpendicular", 80, 1e-4),
        ):
            with self.subTest(joint_type=joint_type):
                base = self.assembly.newObject("Part::Box", joint_type + "Base")
                base.Length = 30
                base.Width = 20
                base.Height = 10
                lid = self.assembly.newObject("Part::Box", joint_type + "Lid")
                lid.Length = 20
                lid.Width = 12
                lid.Height = 8
                lid.Placement = App.Placement(
                    App.Vector(40, 5, 30), App.Rotation(App.Vector(0, 1, 0), rotation)
                )
                self.doc.recompute()
                Assembly.createGroundedJoint(self.assembly, base, recompute=False)
                Assembly.createJoint(
                    self.assembly,
                    joint_type,
                    Assembly.makeJointReference(base, "Face6"),
                    Assembly.makeJointReference(lid, "Face6"),
                    solve=True,
                    recompute=False,
                )
                status = self.assembly.solve(False)
                alignment = abs(
                    _plane_normal(base, "Face6").dot(_plane_normal(lid, "Face6"))
                )
                self.assertEqual(status, 0, joint_type)
                if joint_type == "Parallel":
                    self.assertGreater(alignment, limit, alignment)
                else:
                    self.assertLess(alignment, limit, alignment)

    def test_api_contradictory_parallel_and_perpendicular_fails(self):
        """Parallel and perpendicular on the same faces must not solve as success."""
        base = self.assembly.newObject("Part::Box", "ClashBase")
        base.Length = 30
        base.Width = 20
        base.Height = 10
        lid = self.assembly.newObject("Part::Box", "ClashLid")
        lid.Length = 20
        lid.Width = 12
        lid.Height = 8
        lid.Placement = App.Placement(App.Vector(40, 5, 30), App.Rotation())
        self.doc.recompute()

        Assembly.createGroundedJoint(self.assembly, base, recompute=False)
        Assembly.createJoint(
            self.assembly,
            "Parallel",
            Assembly.makeJointReference(base, "Face6"),
            Assembly.makeJointReference(lid, "Face6"),
            solve=True,
        )
        Assembly.createJoint(
            self.assembly,
            "Perpendicular",
            Assembly.makeJointReference(base, "Face6"),
            Assembly.makeJointReference(lid, "Face6"),
            solve=True,
        )

        violated = UtilsAssembly.violatedJointLabels(self.assembly)
        before = self._solve_placement_snapshot()
        status = self.assembly.solve(False)
        normal_base = _plane_normal(base, "Face6")
        normal_lid = _plane_normal(lid, "Face6")
        alignment = abs(normal_base.dot(normal_lid))
        self.assertEqual(
            status,
            -3,
            "contradictory Parallel and Perpendicular reported status {}; "
            "violated {}; face alignment {} (1 = parallel, 0 = perpendicular)".format(
                status, violated, alignment
            ),
        )
        self.assertIn("Perpendicular", violated)
        self.assertEqual(self._solve_placement_snapshot(), before)
        self.assertEqual(UtilsAssembly.violatedJointLabels(self.assembly), violated)


def _shape_circle(obj, element):
    """Circle center and axis in the shape frame, which already includes Placement."""
    curve = obj.Shape.getElement(element).Curve
    return curve.Location, curve.Axis


def _axes_coaxial(center1, axis1, center2, axis2, tolerance=1e-5):
    direction1 = App.Vector(axis1)
    direction2 = App.Vector(axis2)
    if direction1.Length < 1e-12 or direction2.Length < 1e-12:
        return False
    direction1.normalize()
    direction2.normalize()
    if direction1.cross(direction2).Length > 1e-4:
        return False
    return (center2 - center1).cross(direction1).Length <= tolerance


def _plane_normal(obj, element):
    """Face normal in the shape frame, which already includes Placement."""
    return obj.Shape.getElement(element).normalAt(0, 0)
