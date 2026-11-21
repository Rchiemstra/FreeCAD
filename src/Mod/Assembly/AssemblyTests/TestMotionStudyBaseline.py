# SPDX-License-Identifier: LGPL-2.1-or-later

"""M0 reference fixtures (genuine joints with Detach1/Detach2 = False) and baseline checks."""

import math
import unittest

import FreeCAD as App
import Part
import Assembly
import AssemblyApp
import UtilsAssembly
from Assembly.MotionInterference import _driver


class TestMotionStudyBaseline(unittest.TestCase):
    def setUp(self):
        self.docs = []

    def tearDown(self):
        for name in reversed(self.docs):
            if name in App.listDocuments():
                App.closeDocument(name)

    def new_doc(self, name):
        doc = App.newDocument(name)
        self.docs.append(doc.Name)
        return doc

    def assert_genuine_joint(self, joint):
        self.assertFalse(joint.Detach1, f"{joint.Name} Detach1 must be False")
        self.assertFalse(joint.Detach2, f"{joint.Name} Detach2 must be False")

    def test_m0_slider_clash_baseline(self):
        doc = self.new_doc("MS01SliderClash")
        assembly = Assembly.createAssembly(doc, "Assembly")

        base = assembly.newObject("Part::Feature", "Base")
        base.Shape = Part.makeBox(2, 2, 2, App.Vector(-1, -1, -4))
        slider = assembly.newObject("Part::Feature", "Slider")
        slider.Shape = Part.makeBox(1, 1, 1, App.Vector(-0.5, -0.5, 0))
        obstacle = assembly.newObject("Part::Feature", "Obstacle")
        obstacle.Shape = Part.makeBox(2, 2, 2, App.Vector(-1, -1, 5))
        doc.recompute()

        Assembly.createGroundedJoint(assembly, base, recompute=False)
        Assembly.createGroundedJoint(assembly, obstacle, recompute=False)
        joint = Assembly.createJoint(
            assembly,
            "Slider",
            Assembly.makeJointReference(base, "Face6"),
            Assembly.makeJointReference(slider, "Face6"),
        )
        self.assert_genuine_joint(joint)
        self.assertTrue(joint.Placement1.Base.isEqual(App.Vector(0, 0, -2), 1e-6))
        self.assertTrue(joint.Placement2.Base.isEqual(App.Vector(0, 0, 1), 1e-6))

        solve_rc = assembly.solve()
        self.assertEqual(solve_rc, 0)
        self.assertAlmostEqual(UtilsAssembly.getJointDistance(joint), 3.0, places=6)

        initial_scan = Assembly.scanSelectedPair(assembly, slider, obstacle, measureDistances=True)
        self.assertTrue(initial_scan["complete"])
        self.assertEqual(len(initial_scan["pairs"]), 1)
        self.assertEqual(initial_scan["pairs"][0]["solidDetection"]["kind"], "Clear")
        self.assertAlmostEqual(
            initial_scan["pairs"][0]["solidDetection"]["minimumDistance"], 4.0, places=6
        )

        sweep = Assembly.sweepJoint(
            assembly,
            joint,
            slider,
            obstacle,
            interval=(3, 11),
            step=1,
            clearance=1.5,
        )
        self.assertEqual(sweep["completeness"], "complete", sweep["reason"])
        self.assertEqual(len(sweep["samples"]), 9)
        contact_coords = [s["coordinate"] for s in sweep["samples"] if s["contact"]]
        self.assertEqual(contact_coords, [7.0, 8.0, 9.0, 10.0])
        self.assertAlmostEqual(sweep["firstContact"]["coordinate"], 7.0, places=2)
        self.assertLessEqual(sweep["firstContact"]["resolution"], 0.01)
        self.assertAlmostEqual(sweep["firstClearanceViolation"]["sampledCoordinate"], 6.0)
        self.assertAlmostEqual(sweep["firstClearanceViolation"]["coordinate"], 5.5, delta=0.01)
        self.assertAlmostEqual(sweep["minimumClearance"]["value"], 0.0, places=6)

        created = []
        sim = _driver(doc, joint, 3.0, 8.5, 1, "mm", created)
        try:
            self.assertEqual(assembly.generateSimulation(sim), 0)
            assembly.updateForFrame(assembly.numberOfFrames() - 1)
            self.assertAlmostEqual(slider.Placement.Base.z, 5.5, places=6)
            clash_scan = Assembly.scanSelectedPair(
                assembly, slider, obstacle, measureDistances=True
            )
            self.assertTrue(clash_scan["complete"])
            self.assertEqual(len(clash_scan["pairs"]), 1)
            self.assertEqual(clash_scan["pairs"][0]["solidDetection"]["kind"], "Penetration")
            self.assertAlmostEqual(
                clash_scan["pairs"][0]["detection"]["overlapVolume"], 1.0, places=6
            )
        finally:
            for name in reversed(created):
                if doc.getObject(name):
                    doc.removeObject(name)

    def test_m0_rotational_minimum_gap_baseline(self):
        doc = self.new_doc("MS01RotationalMinGap")
        assembly = Assembly.createAssembly(doc, "Assembly")

        base = assembly.newObject("Part::Feature", "Base")
        base.Shape = Part.makeBox(4, 4, 2, App.Vector(-2, -2, -1)).cut(
            Part.makeBox(2, 2, 2, App.Vector(-1, -1, -1))
        )
        base_top_face = next(
            f"Face{i}"
            for i, face in enumerate(base.Shape.Faces, 1)
            if abs(face.CenterOfGravity.z - 1.0) < 1e-6
        )

        arm = assembly.newObject("Part::Feature", "Arm")
        arm.Shape = Part.makeCompound(
            [
                Part.makeBox(1, 1, 1, App.Vector(-0.5, -0.5, 0)),
                Part.makeSphere(0.25, App.Vector(5, 0, 1.5)),
            ]
        )

        target_angle = math.radians(37.0)
        obstacle = assembly.newObject("Part::Feature", "Obstacle")
        obstacle.Shape = Part.makeSphere(
            0.25,
            App.Vector(6.0 * math.cos(target_angle), 6.0 * math.sin(target_angle), 1.5),
        )
        doc.recompute()

        Assembly.createGroundedJoint(assembly, base, recompute=False)
        Assembly.createGroundedJoint(assembly, obstacle, recompute=False)
        joint = Assembly.createJoint(
            assembly,
            "Revolute",
            Assembly.makeJointReference(base, base_top_face),
            Assembly.makeJointReference(arm, "Face6"),
        )
        self.assert_genuine_joint(joint)
        self.assertTrue(joint.Placement1.Base.isEqual(App.Vector(0, 0, 1), 1e-6))
        self.assertTrue(joint.Placement2.Base.isEqual(App.Vector(0, 0, 1), 1e-6))

        solve_rc = assembly.solve()
        self.assertEqual(solve_rc, 0)

        initial_scan = Assembly.scanSelectedPair(assembly, arm, obstacle, measureDistances=True)
        self.assertTrue(initial_scan["complete"])
        self.assertEqual(len(initial_scan["pairs"]), 1)
        self.assertEqual(initial_scan["pairs"][0]["solidDetection"]["kind"], "Clear")
        expected_initial_gap = math.sqrt(61.0 - 60.0 * math.cos(target_angle)) - 0.5
        self.assertAlmostEqual(
            initial_scan["pairs"][0]["solidDetection"]["minimumDistance"],
            expected_initial_gap,
            places=5,
        )

        sweep = Assembly.sweepJoint(
            assembly,
            joint,
            arm,
            obstacle,
            interval=(0, 80),
            step=20,
            refinementTolerance="0.002 deg",
            refinementPoseBudget=32,
        )
        self.assertEqual(sweep["completeness"], "complete", sweep["reason"])
        self.assertIsNone(sweep["firstContact"])
        minimum = sweep["minimumClearance"]
        self.assertTrue(minimum["converged"])
        self.assertGreater(sweep["sampledMinimumClearance"]["value"] - 0.5, 0.04)
        self.assertAlmostEqual(minimum["coordinate"], 37.0, delta=0.002)
        self.assertAlmostEqual(minimum["value"], 0.5, delta=0.002)

    def test_m0_nested_assembly_link_baseline(self):
        doc = self.new_doc("MS01NestedAssemblyLink")

        shared = doc.addObject("Part::Feature", "SharedBlock")
        shared.Shape = Part.makeBox(1, 1, 1, App.Vector(-0.5, -0.5, -1))

        sub_asm = Assembly.createAssembly(doc, "SubAssembly")
        sub_base = sub_asm.newObject("Part::Feature", "SubBase")
        sub_base.Shape = Part.makeCompound(
            [
                Part.makeBox(1, 1, 1, App.Vector(-0.5, -0.5, -1)),
                Part.makeBox(1, 1, 1, App.Vector(3.5, -0.5, -1)),
            ]
        )
        sub_link_a = sub_asm.newObject("App::Link", "SubLinkA")
        sub_link_a.setLink(shared)
        sub_link_b = sub_asm.newObject("App::Link", "SubLinkB")
        sub_link_b.setLink(shared)
        doc.recompute()

        Assembly.createGroundedJoint(sub_asm, sub_base, recompute=False)
        fix_a = Assembly.createJoint(
            sub_asm,
            "Fixed",
            Assembly.makeJointReference(sub_base, "Face6"),
            Assembly.makeJointReference(sub_link_a, "Face6"),
        )
        fix_b = Assembly.createJoint(
            sub_asm,
            "Fixed",
            Assembly.makeJointReference(sub_base, "Face12"),
            Assembly.makeJointReference(sub_link_b, "Face6"),
        )
        self.assert_genuine_joint(fix_a)
        self.assert_genuine_joint(fix_b)
        self.assertEqual(sub_asm.solve(), 0)
        self.assertTrue(sub_link_a.Placement.Base.isEqual(App.Vector(0, 0, 0), 1e-6))
        self.assertTrue(sub_link_b.Placement.Base.isEqual(App.Vector(4, 0, 0), 1e-6))

        main_asm = Assembly.createAssembly(doc, "MainAssembly")
        main_base = main_asm.newObject("Part::Feature", "MainBase")
        main_base.Shape = Part.makeBox(2, 2, 2, App.Vector(-1, -1, -4))
        obstacle = main_asm.newObject("Part::Feature", "MainObstacle")
        obstacle.Shape = Part.makeBox(2, 2, 2, App.Vector(3, -1, 4))
        asm_link = main_asm.newObject("Assembly::AssemblyLink", "SubAsmLink")
        asm_link.LinkedObject = sub_asm
        asm_link.Rigid = False
        doc.recompute()

        local_base = next(
            o for o in asm_link.Group if o.TypeId == "App::Link" and o.LinkedObject == sub_base
        )
        local_link_a = next(
            o for o in asm_link.Group if o.TypeId == "App::Link" and o.LinkedObject == sub_link_a
        )
        local_link_b = next(
            o for o in asm_link.Group if o.TypeId == "App::Link" and o.LinkedObject == sub_link_b
        )
        self.assertEqual(len(asm_link.Joints), 2)
        for child_joint in asm_link.Joints:
            self.assert_genuine_joint(child_joint)

        Assembly.createGroundedJoint(main_asm, main_base, recompute=False)
        Assembly.createGroundedJoint(main_asm, obstacle, recompute=False)
        slider_joint = Assembly.createJoint(
            main_asm,
            "Slider",
            Assembly.makeJointReference(main_base, "Face6"),
            Assembly.makeJointReference(local_link_a, "Face6"),
        )
        self.assert_genuine_joint(slider_joint)

        solve_rc = main_asm.solve()
        self.assertEqual(solve_rc, 0)

        inventory = AssemblyApp._motionInterferenceLeaves(main_asm)
        shared_source_id = f"{doc.Name}#SharedBlock"
        repeated_leaves = [l for l in inventory["leaves"] if l["sourceId"] == shared_source_id]
        self.assertEqual(len(repeated_leaves), 2)
        self.assertEqual(
            {l["path"] for l in repeated_leaves},
            {f"SubAsmLink.{local_link_a.Name}.", f"SubAsmLink.{local_link_b.Name}."},
        )
        self.assertTrue(all(l["component"] == "SubAsmLink." for l in repeated_leaves))

        initial_scan = Assembly.scanSelectedPair(
            main_asm, asm_link, obstacle, measureDistances=True
        )
        self.assertTrue(initial_scan["complete"])
        self.assertEqual(len(initial_scan["pairs"]), 3)
        self.assertEqual(initial_scan["counts"]["penetrations"], 0)

        with self.assertRaisesRegex(ValueError, "Nested or flexible assemblies are not supported"):
            Assembly.sweepJoint(main_asm, slider_joint, asm_link, obstacle, interval=(2, 8), step=1)

        created = []
        sim = _driver(doc, slider_joint, 2.0, 6.5, 3, "mm", created)
        UtilsAssembly.getSimulationGroup(main_asm).Group = [sim]
        doc.recompute()
        try:
            all_result = Assembly.sweepSimulation(
                main_asm, sim, scope="all", refinementPoseBudget=0
            )
            self.assertEqual(all_result["completeness"], "incomplete")
            self.assertIn("Flexible assembly SubAsmLink is not supported", all_result["reason"])

            self.assertEqual(main_asm.generateSimulation(sim), 0)
            main_asm.updateForFrame(main_asm.numberOfFrames() - 1)
            self.assertTrue(local_link_a.Placement.Base.isEqual(App.Vector(0, 0, 4.5), 1e-6))
            self.assertTrue(local_link_b.Placement.Base.isEqual(App.Vector(4, 0, 4.5), 1e-6))

            clash_scan = Assembly.scanSelectedPair(
                main_asm, asm_link, obstacle, measureDistances=True
            )
            self.assertTrue(clash_scan["complete"])
            self.assertEqual(len(clash_scan["pairs"]), 3)
            self.assertEqual(clash_scan["counts"]["penetrations"], 2)
            by_first_path = {
                clash_scan["leaves"][p["leafIndexA"]]["occurrenceSubName"]: p
                for p in clash_scan["pairs"]
            }
            pair_base = by_first_path[f"SubAsmLink.{local_base.Name}."]
            pair_a = by_first_path[f"SubAsmLink.{local_link_a.Name}."]
            pair_b = by_first_path[f"SubAsmLink.{local_link_b.Name}."]
            self.assertEqual(pair_base["solidDetection"]["kind"], "Penetration")
            self.assertAlmostEqual(pair_base["detection"]["overlapVolume"], 0.5, places=6)
            self.assertEqual(pair_a["solidDetection"]["kind"], "Clear")
            self.assertAlmostEqual(pair_a["solidDetection"]["minimumDistance"], 2.5, places=6)
            self.assertEqual(pair_b["solidDetection"]["kind"], "Penetration")
            self.assertAlmostEqual(pair_b["detection"]["overlapVolume"], 0.5, places=6)
        finally:
            for name in reversed(created):
                if doc.getObject(name):
                    doc.removeObject(name)

    def test_m0_closed_loop_four_bar_baseline(self):
        doc = self.new_doc("MS01ClosedLoop")
        assembly = Assembly.createAssembly(doc, "Assembly")

        base = assembly.newObject("Part::Feature", "Base")
        base.Shape = Part.makeCompound(
            [
                Part.makeBox(2, 2, 1, App.Vector(-1, -1, -1)),
                Part.makeBox(2, 2, 1, App.Vector(19, -1, -1)),
            ]
        )
        arm = assembly.newObject("Part::Feature", "Arm")
        arm.Shape = Part.makeCompound(
            [
                Part.makeBox(2, 2, 1, App.Vector(-1, -1, -1)),
                Part.makeBox(2, 2, 1, App.Vector(9, -1, 1)),
            ]
        )
        arm.Placement = App.Placement(App.Vector(0, 0, 0), App.Rotation(App.Vector(0, 0, 1), 90))

        coupler = assembly.newObject("Part::Feature", "Coupler")
        coupler.Shape = Part.makeCompound(
            [
                Part.makeBox(2, 2, 1, App.Vector(-1, -1, 1)),
                Part.makeBox(2, 2, 1, App.Vector(19, -1, 1)),
                Part.makeSphere(1.0, App.Vector(10, 0, 4)),
            ]
        )
        coupler.Placement = App.Placement(App.Vector(0, 10, 0), App.Rotation())

        rocker = assembly.newObject("Part::Feature", "Rocker")
        rocker.Shape = Part.makeCompound(
            [
                Part.makeBox(2, 2, 1, App.Vector(-1, -1, -1)),
                Part.makeBox(2, 2, 1, App.Vector(9, -1, 1)),
            ]
        )
        rocker.Placement = App.Placement(
            App.Vector(20, 0, 0), App.Rotation(App.Vector(0, 0, 1), 90)
        )

        obstacle_angle = math.radians(110.0)
        obstacle = assembly.newObject("Part::Feature", "Obstacle")
        obstacle.Shape = Part.makeSphere(
            1.0,
            App.Vector(
                10.0 + 10.0 * math.cos(obstacle_angle),
                10.0 * math.sin(obstacle_angle),
                4.0,
            ),
        )
        doc.recompute()

        Assembly.createGroundedJoint(assembly, base, recompute=False)
        Assembly.createGroundedJoint(assembly, obstacle, recompute=False)

        j_crank = Assembly.createJoint(
            assembly,
            "Revolute",
            Assembly.makeJointReference(base, "Face6"),
            Assembly.makeJointReference(arm, "Face6"),
            solve=False,
            presolve=False,
            recompute=False,
        )
        j_coupler_left = Assembly.createJoint(
            assembly,
            "Revolute",
            Assembly.makeJointReference(arm, "Face12"),
            Assembly.makeJointReference(coupler, "Face6"),
            solve=False,
            presolve=False,
            recompute=False,
        )
        j_coupler_right = Assembly.createJoint(
            assembly,
            "Revolute",
            Assembly.makeJointReference(coupler, "Face12"),
            Assembly.makeJointReference(rocker, "Face12"),
            solve=False,
            presolve=False,
            recompute=False,
        )
        j_rocker = Assembly.createJoint(
            assembly,
            "Revolute",
            Assembly.makeJointReference(base, "Face12"),
            Assembly.makeJointReference(rocker, "Face6"),
            solve=False,
            presolve=False,
            recompute=False,
        )
        doc.recompute()

        for joint in (j_crank, j_coupler_left, j_coupler_right, j_rocker):
            self.assert_genuine_joint(joint)

        solve_rc = assembly.solve()
        self.assertEqual(solve_rc, 0)

        initial_scan = Assembly.scanSelectedPair(assembly, coupler, obstacle, measureDistances=True)
        self.assertTrue(initial_scan["complete"])
        self.assertEqual(len(initial_scan["pairs"]), 1)
        self.assertEqual(initial_scan["pairs"][0]["solidDetection"]["kind"], "Clear")
        expected_initial_gap = 20.0 * math.sin(math.radians(10.0)) - 2.0
        self.assertAlmostEqual(
            initial_scan["pairs"][0]["solidDetection"]["minimumDistance"],
            expected_initial_gap,
            places=5,
        )

        with self.assertRaisesRegex(ValueError, "additional moving joints|closed loops"):
            Assembly.sweepJoint(assembly, j_crank, coupler, obstacle, interval=(90, 120), step=5)

        created = []
        sim = _driver(doc, j_crank, 0, 1, 10, "deg", created)
        sim.Group[0].Formula = "pi/2+pi/6*time"
        sim.aTimeStart, sim.bTimeEnd, sim.cTimeStepOutput = 0.0, 1.0, 0.1
        UtilsAssembly.getSimulationGroup(assembly).Group = [sim]
        doc.recompute()
        try:
            sim_result = Assembly.sweepSimulation(
                assembly,
                sim,
                coupler,
                obstacle,
                refinementTolerance=1e-5,
                refinementPoseBudget=32,
            )
            self.assertEqual(sim_result["completeness"], "complete", sim_result["reason"])
            event = sim_result["firstContact"]
            self.assertIsNotNone(event)
            expected_angle = 110.0 - math.degrees(2.0 * math.asin(0.1))
            actual_angle = event["drivenCoordinates"][j_crank.Name]["value"]
            self.assertAlmostEqual(actual_angle, expected_angle, delta=0.002)
        finally:
            for name in reversed(created):
                if doc.getObject(name):
                    doc.removeObject(name)
