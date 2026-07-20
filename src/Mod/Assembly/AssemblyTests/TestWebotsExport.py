# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regression tests for the Assembly Webots PROTO exporter."""

import os
import tempfile
import unittest

import FreeCAD as App
import Assembly
import JointObject
import Materials

import WebotsExport


class TestWebotsExport(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TestWebotsExport")
        self.assembly = Assembly.createAssembly(self.doc, "RobotAssembly", recompute=False)
        self.temporary_directory = tempfile.TemporaryDirectory()

    def tearDown(self):
        App.closeDocument(self.doc.Name)
        self.temporary_directory.cleanup()

    def _path(self, name="Robot.proto"):
        return os.path.join(self.temporary_directory.name, name)

    def _box(self, name, placement=None):
        box = self.assembly.newObject("Part::Box", name)
        box.Length = 10
        box.Width = 10
        box.Height = 10
        if placement is not None:
            box.Placement = placement
        self.doc.recompute()
        return box

    def _joint(self, joint_type, first, second, label=None):
        joint = Assembly.createJoint(
            self.assembly,
            joint_type,
            Assembly.makeJointReference(first),
            Assembly.makeJointReference(second),
            label=label,
            solve=False,
            presolve=False,
            recompute=False,
        )
        joint.Placement1 = App.Placement()
        joint.Placement2 = App.Placement()
        self.doc.recompute()
        return joint

    def test_box_geometry_material_and_physics_use_si_units(self):
        box = self._box("MetricBox")
        box.Length = 1000
        box.Width = 2000
        box.Height = 3000

        material = Materials.Material()
        uuids = Materials.UUIDs()
        material.addPhysicalModel(uuids.Density)
        material.setPhysicalValue("Density", "500 kg/m^3")
        material.addAppearanceModel(uuids.BasicRendering)
        material.setAppearanceValue("DiffuseColor", "(0.1, 0.2, 0.3, 1.0)")
        material.setAppearanceValue("Transparency", "0.25")
        box.ShapeMaterial = material
        self.doc.recompute()

        filename = self._path("123 metric robot.proto")
        self.assembly.exportAsWebotsPROTO(filename)
        with open(filename, encoding="utf-8") as exported:
            proto = exported.read()

        self.assertIn("#VRML_SIM R2025a utf8", proto)
        self.assertIn("PROTO _123_metric_robot [", proto)
        self.assertIn("IndexedFaceSet", proto)
        self.assertIn("baseColor 0.1 0.2 0.3", proto)
        self.assertIn("transparency 0.25", proto)
        self.assertIn("mass 3000", proto)
        self.assertIn("centerOfMass [\n        0.5 1 1.5", proto)
        self.assertIn("3250 2500 1250", proto)
        self.assertIn("0 0 0", proto)
        self.assertIn("field SFBool enableBoundingObject FALSE", proto)
        self.assertIn("field SFBool enablePhysics FALSE", proto)
        self.assertNotIn("no positive ShapeMaterial density", proto)

    def test_failed_export_does_not_replace_existing_destination(self):
        box = self._box("DegenerateBox")
        filename = self._path()
        with open(filename, "w", encoding="utf-8") as destination:
            destination.write("sentinel")

        box.Height = 0
        self.doc.recompute()
        with self.assertRaises(WebotsExport.WebotsExportError):
            self.assembly.exportAsWebotsPROTO(filename)

        with open(filename, encoding="utf-8") as destination:
            self.assertEqual(destination.read(), "sentinel")

    def test_fixed_link_inertia_uses_parallel_axis_terms(self):
        first = self._box("FirstMass")
        second = self._box("SecondMass", App.Placement(App.Vector(2000, 1000, 0), App.Rotation()))
        for box in (first, second):
            box.Length = 1000
            box.Width = 1000
            box.Height = 1000
        self._joint("Fixed", first, second, "RigidPair")
        self.doc.recompute()

        filename = self._path()
        self.assembly.exportAsWebotsPROTO(filename)
        with open(filename, encoding="utf-8") as exported:
            proto = exported.read()

        self.assertIn("mass 2000", proto)
        self.assertIn("1.5 1 0.5", proto)
        self.assertIn("833.333333333 2333.33333333 2833.33333333", proto)
        self.assertIn("-1000 0 0", proto)

    def test_fixed_collapse_disconnected_forest_names_and_determinism(self):
        self.assembly.Label = 'Røbôt "quoted"'
        first = self._box("First")
        second = self._box("Second", App.Placement(App.Vector(20, 0, 0), App.Rotation()))
        third = self._box("Third", App.Placement(App.Vector(40, 0, 0), App.Rotation()))
        first.Label = 'same "label"'
        second.Label = 'same "label"'
        self._joint("Fixed", first, second, "Fixed connection")

        first_filename = self._path("ünicode robot.proto")
        second_filename = self._path("ünicode robot copy.proto")
        WebotsExport.export([self.assembly], first_filename)
        WebotsExport.export([self.assembly], second_filename)
        with open(first_filename, encoding="utf-8") as exported:
            first_proto = exported.read()
        with open(second_filename, encoding="utf-8") as exported:
            second_proto = exported.read()

        # The filename is the only expected difference between these exports.
        first_body = first_proto[first_proto.index("[") :]
        second_body = second_proto[second_proto.index("[") :]
        self.assertEqual(first_body, second_body)
        self.assertIn("PROTO nicode_robot [", first_proto)
        self.assertIn('field SFString name "Røbôt \\"quoted\\""', first_proto)
        self.assertIn("Disconnected Assembly trees", first_proto)
        self.assertIn("using 1000 kg/m^3", first_proto)
        self.assertNotIn("FixedJoint", first_proto)
        self.assertEqual(first_proto.count("IndexedFaceSet"), 3)

        # Keep the otherwise-isolated fixture alive and explicit in this test.
        self.assertEqual(third.Name, "Third")

    def test_core_articulated_joints_positions_limits_and_sensors(self):
        base = self._box("Base")
        revolute_part = self._box(
            "RevolutePart",
            App.Placement(App.Vector(0, 0, 0), App.Rotation(App.Vector(0, 0, 1), 30)),
        )
        slider_part = self._box("SliderPart", App.Placement(App.Vector(0, 0, 20), App.Rotation()))
        ball_part = self._box(
            "BallPart", App.Placement(App.Vector(30, 0, 0), App.Rotation(10, 20, 30))
        )
        Assembly.createGroundedJoint(self.assembly, base, recompute=False)

        revolute = self._joint("Revolute", base, revolute_part, "Axis")
        revolute.EnableAngleMin = True
        revolute.EnableAngleMax = True
        revolute.AngleMin = -45
        revolute.AngleMax = 45

        slider = self._joint("Slider", base, slider_part, "Axis")
        slider.EnableLengthMin = True
        slider.EnableLengthMax = True
        slider.LengthMin = -10
        slider.LengthMax = 30

        self._joint("Ball", base, ball_part, "Spherical")
        self.doc.recompute()

        filename = self._path()
        self.assembly.exportAsWebotsPROTO(filename)
        with open(filename, encoding="utf-8") as exported:
            proto = exported.read()

        self.assertIn("HingeJoint", proto)
        self.assertIn("SliderJoint", proto)
        self.assertIn("BallJoint", proto)
        self.assertIn("jointParameters2 JointParameters", proto)
        self.assertIn("jointParameters3 JointParameters", proto)
        self.assertIn("position 0.523598775598", proto)
        self.assertIn("position 0.02", proto)
        self.assertIn("minStop -0.785398163397", proto)
        self.assertIn("maxStop 0.785398163397", proto)
        self.assertIn("minStop -0.01", proto)
        self.assertIn("maxStop 0.03", proto)
        self.assertEqual(proto.count("PositionSensor {"), 5)
        self.assertIn(f'name "{revolute.Label}_sensor"', proto)
        self.assertIn(f'name "{slider.Label}_sensor"', proto)
        self.assertIn('name "Spherical_sensor_1"', proto)
        self.assertIn('name "Spherical_sensor_2"', proto)
        self.assertIn('name "Spherical_sensor_3"', proto)
        self.assertNotIn("RotationalMotor", proto)
        self.assertNotIn("LinearMotor", proto)
        # A grounded Robot has no Physics, but all three movable endpoint Solids do.
        self.assertEqual(proto.count("physics Physics {"), 3)

    def test_reversed_joint_reverses_position_and_limits(self):
        root = self._box("Root")
        moving = self._box(
            "Moving", App.Placement(App.Vector(), App.Rotation(App.Vector(0, 0, 1), 30))
        )
        joint = self._joint("Revolute", moving, root, "Reverse")
        joint.EnableAngleMin = True
        joint.EnableAngleMax = True
        joint.AngleMin = -60
        joint.AngleMax = 10
        self.doc.recompute()

        filename = self._path()
        self.assembly.exportAsWebotsPROTO(filename)
        with open(filename, encoding="utf-8") as exported:
            proto = exported.read()

        self.assertIn("position 0.523598775598", proto)
        self.assertIn("minStop -0.174532925199", proto)
        self.assertIn("maxStop 1.0471975512", proto)

    def test_nested_links_preserve_occurrence_transform(self):
        source_box = self.doc.addObject("Part::Box", "NestedSourceBox")
        source_box.Length = 10
        source_box.Width = 10
        source_box.Height = 10
        source_box.Placement = App.Placement(App.Vector(10, 0, 0), App.Rotation())

        source_container = self.doc.addObject("App::Part", "NestedSourceContainer")
        inner_link = self.doc.addObject("App::Link", "InnerLink")
        inner_link.setLink(source_box)
        inner_link.Placement = App.Placement(App.Vector(30, 0, 0), App.Rotation())
        source_container.addObject(inner_link)

        outer_link = self.assembly.newObject("App::Link", "OuterLink")
        outer_link.setLink(source_container)
        outer_link.Placement = App.Placement(App.Vector(100, 0, 0), App.Rotation())
        self.doc.recompute()

        filename = self._path()
        self.assembly.exportAsWebotsPROTO(filename)
        with open(filename, encoding="utf-8") as exported:
            proto = exported.read()

        self.assertIn("0.13 0 0", proto)
        self.assertIn("0.14 0.01 0.01", proto)
        self.assertIn("0.135 0.005 0.005", proto)

    def test_rejects_invalid_selection_joint_graph_and_limits(self):
        first = self._box("First")
        second = self._box("Second")

        with self.assertRaisesRegex(WebotsExport.WebotsExportError, "exactly one"):
            WebotsExport.export([], self._path())
        with self.assertRaisesRegex(WebotsExport.WebotsExportError, "AssemblyObject"):
            WebotsExport.export([first], self._path())

        cylindrical = self._joint("Cylindrical", first, second, "Unsupported")
        with self.assertRaisesRegex(WebotsExport.WebotsExportError, "Unsupported.*Cylindrical"):
            self.assembly.exportAsWebotsPROTO(self._path())
        cylindrical.Suppressed = True

        revolute = self._joint("Revolute", first, second, "OneSided")
        revolute.EnableAngleMin = True
        revolute.AngleMin = -10
        with self.assertRaisesRegex(WebotsExport.WebotsExportError, "one-sided"):
            self.assembly.exportAsWebotsPROTO(self._path())

        revolute.EnableAngleMax = True
        revolute.AngleMax = 200
        with self.assertRaisesRegex(WebotsExport.WebotsExportError, r"\[-pi, pi\]"):
            self.assembly.exportAsWebotsPROTO(self._path())

    def test_rejects_cycles_multiple_grounded_links_and_incomplete_joints(self):
        first = self._box("First")
        second = self._box("Second")
        third = self._box("Third")
        self._joint("Revolute", first, second, "FirstSecond")
        self._joint("Slider", second, third, "SecondThird")
        cycle = self._joint("Ball", third, first, "Cycle")
        with self.assertRaisesRegex(WebotsExport.WebotsExportError, "closed cycle"):
            self.assembly.exportAsWebotsPROTO(self._path())

        cycle.Suppressed = True
        Assembly.createGroundedJoint(self.assembly, first, recompute=False)
        Assembly.createGroundedJoint(self.assembly, third, recompute=False)
        self.doc.recompute()
        with self.assertRaisesRegex(WebotsExport.WebotsExportError, "multiple grounded"):
            self.assembly.exportAsWebotsPROTO(self._path())

        for joint in list(self.assembly.Joints):
            joint.Suppressed = True
        joint_group = next(
            obj for obj in self.assembly.Group if obj.TypeId == "Assembly::JointGroup"
        )
        incomplete = joint_group.newObject("App::FeaturePython", "Incomplete")
        JointObject.Joint(incomplete, 0)
        incomplete.Label = "Incomplete"
        with self.assertRaisesRegex(WebotsExport.WebotsExportError, "Incomplete or incoherent"):
            self.assembly.exportAsWebotsPROTO(self._path())
