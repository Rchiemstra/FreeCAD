# SPDX-License-Identifier: LGPL-2.1-or-later

"""Selected-pair scan and bounded motion integration tests, collected by CI."""

import io
import math
import os
import tempfile
import unittest
import zipfile
from unittest.mock import patch

import FreeCAD as App
import Part
import Assembly


class TestMotionInterference(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("MotionInterference")
        self.assembly = Assembly.createAssembly(self.doc)
        self.first = self.assembly.newObject("Part::Feature", "Arm")
        self.first.Shape = Part.makeBox(1, 1, 1)
        self.second = self.assembly.newObject("Part::Feature", "Base")
        self.second.Shape = Part.makeBox(1, 1, 1)
        self.second.Placement.Base = App.Vector(5, 0, 0)
        self.doc.recompute()

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def scan(self, **options):
        return Assembly.scanSelectedPair(self.assembly, self.first, self.second, **options)

    def sheet(self, clearance="1.5 mm"):
        sheet = self.doc.addObject("Spreadsheet::Sheet", "Clearance")
        for cell, value in {"A1": "Face", "B1": "Tolerance", "C1": "Comment",
                            "A2": "*", "B2": clearance, "C2": "Motion rule"}.items():
            sheet.set(cell, value)
        self.doc.recompute()
        self.assembly.InterferenceClearanceSheet = sheet
        return sheet

    def test_distant_clear_pair_is_measured_on_request(self):
        pruned = self.scan()
        self.assertTrue(pruned["complete"])
        self.assertEqual(pruned["pairs"], [])
        measured = self.scan(measureDistances=True)
        self.assertTrue(measured["complete"])
        self.assertEqual(len(measured["pairs"]), 1)
        pair = measured["pairs"][0]
        self.assertTrue(pair["distanceMeasured"])
        self.assertEqual(pair["detection"]["kind"], "Clear")
        solid = pair["solidDetection"]
        self.assertAlmostEqual(solid["minimumDistance"], 4)
        a, b = (App.Vector(*solid[key]) for key in ("pointOnFirst", "pointOnSecond"))
        self.assertAlmostEqual((a - b).Length, 4)
        self.assertEqual(measured["counts"]["clearPairs"], 1)

    def test_penetration_witness_survives_document_changes(self):
        self.second.Placement.Base = App.Vector(0.5, 0, 0)
        result = self.scan(measureDistances=True)
        self.assertTrue(result["complete"])
        pair = result["pairs"][0]
        self.assertEqual(pair["solidDetection"]["kind"], "Penetration")
        self.assertAlmostEqual(pair["detection"]["overlapVolume"], 0.5)
        self.assertAlmostEqual(pair["detection"]["commonShape"].Volume, 0.5)
        self.second.Placement.Base = App.Vector(9, 0, 0)
        self.assertAlmostEqual(pair["detection"]["commonShape"].Volume, 0.5)
        leaf = result["leaves"][pair["leafIndexB"]]
        self.assertAlmostEqual(leaf["worldShape"].BoundBox.XMin, 0.5)
        App.closeDocument(self.doc.Name)
        self.assertAlmostEqual(pair["detection"]["commonShape"].Volume, 0.5)
        # Keep tearDown independent of the closed document.
        self.doc = App.newDocument("MotionInterferenceCleanup")

    def test_raw_contact_is_not_hidden_by_governing_clearance(self):
        self.second.Shape = Part.makeBox(2, 2, 2, App.Vector(0, -0.5, 0))
        self.second.Placement.Base = App.Vector(1, 0, 0)
        self.assembly.InterferenceClearance = 1.5
        result = self.scan(measureDistances=True)
        self.assertTrue(result["complete"])
        pair = result["pairs"][0]
        self.assertEqual(pair["solidDetection"]["kind"], "Contact")
        self.assertEqual(pair["solidDetection"]["minimumDistance"], 0)
        self.assertEqual(pair["detection"]["kind"], "ClearanceViolation")
        self.assertGreater(pair["detection"]["minimumDistance"], 0)
        self.assertTrue(any(hit["kind"] == "Contact" for hit in pair["faceHits"]))

    def test_clearance_rule_provenance_and_witnesses(self):
        self.second.Placement.Base = App.Vector(2, 0, 0)
        self.sheet()
        result = self.scan(measureDistances=True)
        self.assertTrue(result["complete"])
        pair = result["pairs"][0]
        self.assertEqual(pair["detection"]["kind"], "ClearanceViolation")
        hits = [h for h in pair["faceHits"] if h["kind"] == "ClearanceViolation"]
        self.assertTrue(hits)
        for hit in hits:
            self.assertEqual(hit["appliedClearance"], 1.5)
            self.assertEqual(hit["sourceRows"], [2])
            self.assertEqual(hit["sourceComments"], ["Motion rule"])
            self.assertEqual(hit["ruleKind"], "DefaultStar")
            self.assertTrue(hit["closestPointsValid"])
            self.assertTrue(hit["facePathA"].startswith("Arm.Face"))

    def test_exclusions_suppress_findings_but_retain_measurements(self):
        self.second.Placement.Base = App.Vector(0.5, 0, 0)
        self.assembly.InterferenceExcludedSources = [(self.first, [""]), (self.second, [""])]
        result = self.scan(measureDistances=True)
        self.assertTrue(result["complete"])
        self.assertEqual(result["counts"]["penetrations"], 0)
        self.assertEqual(result["counts"]["excludedViolations"], 1)
        self.assertTrue(result["pairs"][0]["excluded"])
        self.assertEqual(result["pairs"][0]["solidDetection"]["minimumDistance"], 0)
        self.assertTrue(all(h["suppressedByExclusion"]
                            for h in result["pairs"][0]["faceHits"]))

    def test_invalid_sheet_is_incomplete(self):
        self.sheet("invalid")
        result = self.scan(measureDistances=True)
        self.assertFalse(result["complete"])
        self.assertGreater(result["counts"]["invalidRules"], 0)
        self.assertTrue(any(i["diagnostic"] for i in result["componentIssues"]))

    def test_capped_face_results_are_incomplete(self):
        self.second.Placement.Base = App.Vector(2, 0, 0)
        self.assembly.InterferenceClearance = 5
        result = self.scan(measureDistances=True, maxFacePairCandidates=1)
        self.assertFalse(result["complete"])
        self.assertTrue(result["pairs"][0]["faceEnumerationDiagnostic"])
        self.assertAlmostEqual(result["pairs"][0]["solidDetection"]["minimumDistance"], 1)
        self.assertTrue(self.scan(maxFacePairCandidates=0)["complete"])

    def test_cancelled_scan_is_incomplete(self):
        token = Assembly.newInterferenceScanCancellation()
        Assembly.cancelInterferenceScan(token)
        result = self.scan(cancel=token, measureDistances=True)
        self.assertTrue(result["cancelled"])
        self.assertFalse(result["complete"])
        self.assertEqual(result["pairs"], [])

    def test_empty_component_is_not_reported_clear(self):
        self.first.Shape = Part.Shape()
        result = self.scan(measureDistances=True)
        self.assertFalse(result["complete"])
        self.assertGreater(result["counts"]["invalidInputs"], 0)

    def test_hidden_selected_pair_includes_both_occurrences(self):
        self.first.Visibility = False
        self.second.Visibility = False
        result = self.scan(measureDistances=True)
        self.assertTrue(result["complete"])
        self.assertEqual({leaf["occurrenceSubName"] for leaf in result["leaves"]},
                         {"Arm.", "Base."})

    def test_link_occurrences_and_subelement_handles(self):
        linked = self.assembly.newObject("App::Link", "ArmCopy")
        linked.LinkedObject = self.first
        linked.Placement.Base = App.Vector(3, 0, 0)
        self.doc.recompute()
        result = Assembly.scanSelectedPair(
            self.assembly, (self.assembly, "ArmCopy.Face1"),
            (self.assembly, "Base.Face2"), measureDistances=True)
        self.assertTrue(result["complete"])
        self.assertEqual(result["firstOccurrence"], "ArmCopy.")
        pair = result["pairs"][0]
        self.assertEqual(result["leaves"][pair["leafIndexA"]]["sourceId"],
                         self.doc.Name + "#Arm")
        self.assertAlmostEqual(pair["solidDetection"]["minimumDistance"], 1)

    def test_invalid_scope_has_no_all_components_fallback(self):
        unrelated = self.doc.addObject("Part::Feature", "Unrelated")
        for first, second in [(self.first, self.first), (self.first, unrelated),
                              ((self.assembly, "missing.Face1"), self.second)]:
            with self.subTest(first=first, second=second), self.assertRaises(ValueError):
                Assembly.scanSelectedPair(self.assembly, first, second)

    def test_bad_arguments_raise_python_errors(self):
        with self.assertRaises(TypeError):
            Assembly.scanSelectedPair(None, self.first, self.second)
        with self.assertRaises(TypeError):
            Assembly.scanSelectedPair(self.assembly, 123, self.second)
        with self.assertRaises(ValueError):
            self.scan(maxFacePairCandidates=-1)
        with self.assertRaises(ValueError):
            self.scan(cancel=object())
        with self.assertRaises(ValueError):
            Assembly.scanSelectedPair(self.first, self.first, self.second)

    def test_scan_does_not_modify_document(self):
        before = [(obj.Name, obj.Placement.toMatrix().A) for obj in self.doc.Objects
                  if "Placement" in obj.PropertiesList]
        names = [obj.Name for obj in self.doc.Objects]
        self.scan(measureDistances=True)
        self.assertEqual(names, [obj.Name for obj in self.doc.Objects])
        self.assertEqual(before, [(obj.Name, obj.Placement.toMatrix().A)
                                  for obj in self.doc.Objects if "Placement" in obj.PropertiesList])

    def test_part_root_and_component_with_multiple_leaves(self):
        root = self.doc.addObject("App::Part", "PartRoot")
        group = root.newObject("App::Part", "MovingComponent")
        near = group.newObject("Part::Feature", "NearLeaf")
        near.Shape = Part.makeBox(1, 1, 1)
        far = group.newObject("Part::Feature", "FarLeaf")
        far.Shape = Part.makeBox(1, 1, 1, App.Vector(20, 0, 0))
        target = root.newObject("Part::Feature", "Target")
        target.Shape = Part.makeBox(1, 1, 1, App.Vector(2, 0, 0))
        self.doc.recompute()
        result = Assembly.scanSelectedPair(root, group, target, measureDistances=True)
        self.assertTrue(result["complete"])
        self.assertEqual(len(result["pairs"]), 2)
        self.assertEqual(sorted(p["solidDetection"]["minimumDistance"]
                                for p in result["pairs"]), [1, 17])
        self.assertEqual({leaf["occurrenceSubName"] for leaf in result["leaves"]},
                         {"MovingComponent.NearLeaf.", "MovingComponent.FarLeaf.", "Target."})

    def test_clearance_validation_uses_effective_property_value(self):
        # PropertyLength clamps these assignments before the scan receives them.
        for clearance in [-1, float("inf")]:
            with self.subTest(clearance=clearance):
                self.assembly.InterferenceClearance = clearance
                result = self.scan(measureDistances=True)
                self.assertTrue(result["complete"])
                self.assertTrue(math.isfinite(result["clearance"]))
                self.assertGreaterEqual(result["clearance"], 0)
        self.assembly.InterferenceClearance = float("nan")
        result = self.scan(measureDistances=True)
        self.assertFalse(result["complete"])
        self.assertGreater(result["counts"]["invalidInputs"], 0)


class TestMotionSweep(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("MotionSweep")
        self.assembly = Assembly.createAssembly(self.doc)
        self.base = self.assembly.newObject("Part::Feature", "Base")
        self.base.Shape = Part.makeBox(2, 2, 2, App.Vector(-1, -1, -4))
        self.arm = self.assembly.newObject("Part::Feature", "Arm")
        self.arm.Shape = Part.makeBox(1, 1, 1)
        self.obstacle = self.assembly.newObject("Part::Feature", "Obstacle")
        self.obstacle.Shape = Part.makeBox(2, 2, 2, App.Vector(0, -0.5, 6))
        self.doc.recompute()
        Assembly.createGroundedJoint(self.assembly, self.base, recompute=False)
        Assembly.createGroundedJoint(self.assembly, self.obstacle, recompute=False)
        self.joint = self.make_joint("Slider", self.base, self.arm)
        self.doc.recompute()

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def make_joint(self, kind, first, second):
        with patch("JointObject.solveIfAllowed"):
            joint = Assembly.createJoint(
                self.assembly, kind, Assembly.makeJointReference(first, "Face1"),
                Assembly.makeJointReference(second, "Face1"), solve=False,
                presolve=False, recompute=False)
            joint.Detach1 = joint.Detach2 = True
            joint.Placement1 = joint.Placement2 = App.Placement()
        return joint

    def sweep(self, **options):
        options.setdefault("interval", (0, 10))
        options.setdefault("step", 1)
        return Assembly.sweepJoint(self.assembly, self.joint, self.arm, self.obstacle, **options)

    def state(self):
        def content(obj):
            # Compare serialized values without ZIP entry timestamps.
            with zipfile.ZipFile(io.BytesIO(obj.dumpContent())) as archive:
                return {name: archive.read(name) for name in archive.namelist()}

        return dict(
            objects=[o.Name for o in self.doc.Objects],
            content={o.Name: content(o) for o in self.doc.Objects},
            properties={o.Name: {p: (o.getPropertyStatus(p), o.getPropertyByName(p))
                                 for p in o.PropertiesList
                                 if o.getTypeIdOfProperty(p) == "App::PropertyPlacement"}
                        for o in self.doc.Objects},
            flags={o.Name: {p: o.getPropertyStatus(p) for p in o.PropertiesList}
                   for o in self.doc.Objects},
            states={o.Name: o.State for o in self.doc.Objects},
            undo=self.doc.UndoCount, redo=self.doc.RedoCount,
            file=self.doc.getFileChangeState())

    def assert_restored(self, before):
        after = self.state()
        # Placements have no structural Python equality; compare their exact matrix entries.
        for state in (before, after):
            for properties in state["properties"].values():
                for name, (flags, value) in list(properties.items()):
                    if isinstance(value, App.Placement):
                        properties[name] = (flags, value.toMatrix().A)
        self.assertEqual(before, after)

    def test_slider_contact_threshold_minimum_and_witness(self):
        result = self.sweep(clearance=1.5)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertEqual(len(result["samples"]), 11)
        contact = result["firstContact"]
        self.assertAlmostEqual(contact["coordinate"], 5)
        self.assertAlmostEqual(contact["previousClearSample"]["coordinate"], 4)
        self.assertAlmostEqual(contact["resolution"], 1)
        self.assertAlmostEqual(result["firstClearanceViolation"]["coordinate"], 4)
        self.assertAlmostEqual(result["minimumClearance"]["value"], 0)
        self.assertAlmostEqual(result["minimumClearance"]["coordinate"], 5)
        pair = contact["scan"]["pairs"][0]
        self.assertEqual(pair["solidDetection"]["kind"], "Contact")
        self.assertEqual(pair["detection"]["kind"], "ClearanceViolation")
        self.assertAlmostEqual(contact["placements"][self.arm.Name].Base.z, 5)
        self.arm.Placement.Base = App.Vector(0, 0, 50)
        leaf = contact["scan"]["leaves"][pair["leafIndexA"]]
        self.assertAlmostEqual(leaf["worldShape"].BoundBox.ZMin, 5)

    def revolute(self):
        self.doc.removeObject(self.joint.Name)
        self.arm.Shape = Part.makeBox(10, 1, 1, App.Vector(0, -0.5, 0))
        self.obstacle.Shape = Part.makeBox(2, 2, 2, App.Vector(5, 5, 0))
        self.joint = self.make_joint("Revolute", self.base, self.arm)
        self.doc.recompute()

    def test_revolute_contact_between_clear_endpoints_and_degrees(self):
        self.revolute()
        result = self.sweep(interval=(0, "1.5707963267948966 rad"), step="9 deg")
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertEqual(result["unit"], "deg")
        self.assertAlmostEqual(result["samples"][-1]["coordinate"], 90)
        self.assertFalse(result["samples"][0]["contact"])
        self.assertFalse(result["samples"][-1]["contact"])
        self.assertAlmostEqual(result["firstContact"]["coordinate"], 36)
        self.assertAlmostEqual(result["firstContact"]["resolution"], 9)

    def test_signed_unwrapped_angles_and_descending_interval(self):
        self.revolute()
        for interval in ((-200, 200), (200, -200)):
            result = self.sweep(interval=interval, step=40)
            self.assertEqual(result["completeness"], "complete", result["reason"])
            self.assertAlmostEqual(result["samples"][0]["coordinate"], interval[0])
            self.assertAlmostEqual(result["samples"][-1]["coordinate"], interval[1])
            self.assertEqual(len(result["samples"]), 11)

    def test_enabled_slider_limits_and_explicit_intersection(self):
        self.joint.EnableLengthMin = self.joint.EnableLengthMax = True
        self.joint.LengthMin, self.joint.LengthMax = 0, 2
        for interval in (None, (-5, 10)):
            result = self.sweep(interval=interval)
            self.assertEqual(result["completeness"], "complete", result["reason"])
            self.assertEqual(result["interval"], (0, 2))
            self.assertIsNone(result["firstContact"])
            self.assertEqual(result["message"], "no finding at the sampled poses")
            self.assertAlmostEqual(result["minimumClearance"]["value"], 3)

    def test_enabled_revolute_limits_exclude_contact(self):
        self.revolute()
        self.joint.EnableAngleMin = self.joint.EnableAngleMax = True
        self.joint.AngleMin, self.joint.AngleMax = 0, 20
        result = self.sweep(interval=(0, 90), step=5)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertEqual(result["interval"], (0, 20))
        self.assertIsNone(result["firstContact"])

    def test_missing_bounds_and_one_sided_limit(self):
        for interval in (None, (None, 10), (0, None)):
            with self.subTest(interval=interval), self.assertRaisesRegex(ValueError, "Missing bound"):
                self.sweep(interval=interval)
        self.joint.EnableLengthMin = True
        self.joint.LengthMin = 1
        result = self.sweep(interval=(None, 3))
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertAlmostEqual(result["samples"][0]["coordinate"], 1)
        with self.assertRaisesRegex(ValueError, "Missing bound"):
            self.sweep(interval=None)

    def test_bad_ranges_units_and_clearance(self):
        for options in (dict(step=0), dict(step=-1), dict(step=float("nan")),
                        dict(interval=(0, float("inf"))), dict(clearance=-1),
                        dict(clearance="1 deg"), dict(step=0.0001)):
            with self.subTest(options=options), self.assertRaises((ValueError, TypeError)):
                self.sweep(**options)
        self.joint.EnableLengthMin = self.joint.EnableLengthMax = True
        self.joint.LengthMin, self.joint.LengthMax = 2, 3
        with self.assertRaisesRegex(ValueError, "does not intersect"):
            self.sweep(interval=(4, 5))

    def test_exclusions_suppress_events_preserve_minimum(self):
        self.assembly.InterferenceExcludedSources = [(self.arm, [""]), (self.obstacle, [""])]
        result = self.sweep(clearance=1.5)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertIsNone(result["firstContact"])
        self.assertIsNone(result["firstClearanceViolation"])
        self.assertAlmostEqual(result["minimumClearance"]["value"], 0)

    def test_reject_additional_joint_and_closed_loop(self):
        for kind in ("Revolute", "Gears", "Belt"):
            extra = self.make_joint(kind, self.base, self.obstacle)
            with self.subTest(kind=kind), self.assertRaisesRegex(ValueError, "additional moving joints"):
                self.sweep()
            self.doc.removeObject(extra.Name)
        self.make_joint("Fixed", self.base, self.arm)
        with self.assertRaisesRegex(ValueError, "closed loops"):
            self.sweep()

    def test_fixed_member_moves_with_joint(self):
        member = self.assembly.newObject("Part::Feature", "FixedMember")
        member.Shape = Part.makeBox(1, 1, 1, App.Vector(10, 0, 0))
        self.make_joint("Fixed", self.arm, member)
        self.doc.recompute()
        result = self.sweep(interval=(0, 2))
        self.assertEqual(result["completeness"], "complete", result["reason"])

    def test_cancel_before_and_during_sweep(self):
        for before_start in (True, False):
            token = Assembly.SweepCancellation()
            if before_start:
                token.cancel()
            result = self.sweep(cancel=token, progress=lambda sample: token.cancel())
            self.assertEqual(result["completeness"], "cancelled", result["reason"])
            self.assertEqual(len(result["samples"]), 0 if before_start else 1)

    def test_cancel_at_contact_retains_event_pose(self):
        token = Assembly.SweepCancellation()

        def progress(sample):
            self.assertAlmostEqual(self.arm.Placement.Base.z, sample["coordinate"])
            if sample["contact"]:
                token.cancel()

        before = self.state()
        result = self.sweep(cancel=token, progress=progress)
        self.assertEqual(result["completeness"], "cancelled")
        self.assertAlmostEqual(result["firstContact"]["coordinate"], 5)
        self.assert_restored(before)

    def test_contact_at_start_has_no_preceding_clear_sample(self):
        result = self.sweep(interval=(5, 6))
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertAlmostEqual(result["firstContact"]["coordinate"], 5)
        self.assertIsNone(result["firstContact"]["previousClearSample"])
        self.assertIsNone(result["firstContact"]["resolution"])

    def test_exact_restoration_success_cancel_error_and_existing_history(self):
        self.doc.UndoMode = 1
        self.doc.openTransaction("Existing edit")
        self.arm.Label = "Moving arm"
        self.doc.commitTransaction()
        self.doc.openTransaction("Redo edit")
        self.obstacle.Label = "Target"
        self.doc.commitTransaction()
        self.doc.undo()
        # Include pre-existing touched state and a nonzero initial coordinate.
        self.arm.Placement.Base = App.Vector(0, 0, 1.25)
        self.assembly.InterferenceClearance = 0.25
        self.arm.touch()

        def fail(sample):
            raise RuntimeError("Injected failure after a solved pose")

        for mode in ("success", "cancel", "error"):
            token = Assembly.SweepCancellation()
            progress = fail if mode == "error" else (
                (lambda sample: token.cancel()) if mode == "cancel" else None)
            before = self.state()
            result = self.sweep(clearance=1.5, cancel=token, progress=progress)
            self.assertEqual(result["completeness"],
                             {"success": "complete", "cancel": "cancelled", "error": "incomplete"}[mode],
                             result["reason"])
            if mode == "error":
                self.assertIn("Injected failure", result["reason"])
            self.assert_restored(before)

    def test_incomplete_scan_and_solver_error_restore(self):
        before = self.state()
        result = self.sweep(clearance=5, maxFacePairCandidates=1)
        self.assertEqual(result["completeness"], "incomplete")
        self.assertIn("Static scan incomplete", result["reason"])
        self.assert_restored(before)
        with patch("Assembly.MotionInterference._driver", side_effect=RuntimeError("Driver failed")):
            result = self.sweep()
        self.assertEqual(result["completeness"], "incomplete")
        self.assertIn("Driver failed", result["reason"])
        self.assert_restored(before)

    def test_invalid_clearance_rules_make_sweep_incomplete(self):
        sheet = self.doc.addObject("Spreadsheet::Sheet", "InvalidRules")
        for cell, value in {"A1": "Face", "B1": "Tolerance",
                            "A2": "*", "B2": "invalid"}.items():
            sheet.set(cell, value)
        self.doc.recompute()
        self.assembly.InterferenceClearanceSheet = sheet
        before = self.state()
        result = self.sweep()
        self.assertEqual(result["completeness"], "incomplete")
        self.assertIn("Static scan incomplete", result["reason"])
        self.assert_restored(before)

    def test_active_transaction_is_refused_without_changes(self):
        self.doc.UndoMode = 1
        self.doc.openTransaction("Caller owns edits")
        self.arm.Label = "Caller edit"
        before = self.state()
        result = self.sweep()
        self.assertEqual(result["completeness"], "incomplete")
        self.assertIn("transaction", result["reason"])
        self.assert_restored(before)
        self.doc.abortTransaction()

    def test_zero_interval_and_no_duplicate_zero_start(self):
        for interval, count in (((0, 0), 1), ((0, 2), 3)):
            result = self.sweep(interval=interval)
            self.assertEqual(result["completeness"], "complete", result["reason"])
            self.assertEqual(len(result["samples"]), count)

    def test_clean_document_remains_clean(self):
        with tempfile.TemporaryDirectory() as directory:
            self.doc.saveAs(os.path.join(directory, "Sweep.FCStd"))
            before = self.state()
            result = self.sweep()
            self.assertEqual(result["completeness"], "complete", result["reason"])
            self.assert_restored(before)
            self.assertEqual(self.doc.getFileChangeState()["state"], "clean")

    def test_existing_simulation_frames_and_undo_solve_survive(self):
        from Assembly.MotionInterference import _driver

        self.arm.Placement.Base = App.Vector(0, 0, 2)
        self.assertEqual(self.assembly.solve(True), 0)
        created = []
        sim = _driver(self.doc, self.joint, 0, 3, 3, "mm", created)
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.assembly.updateForFrame(2)
        frames = self.assembly.numberOfFrames()
        before = self.state()
        result = self.sweep()
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assert_restored(before)
        self.assertEqual(self.assembly.numberOfFrames(), frames)
        self.assembly.updateForFrame(frames - 1)
        self.assertAlmostEqual(self.arm.Placement.Base.z, 3)
        self.assembly.undoSolve()
        self.assertAlmostEqual(self.arm.Placement.Base.z, 2)

    def test_solver_failure_and_unexpected_motion_are_incomplete(self):
        from Assembly.MotionInterference import _driver

        for formula, diagnostic in (("unknown_function(time)", "end endpoint"),
                                    ("100*time", "missing sample")):
            def driver(*args):
                sim = _driver(*args)
                sim.Group[0].Formula = formula
                return sim

            before = self.state()
            with patch("Assembly.MotionInterference._driver", side_effect=driver):
                result = self.sweep()
            self.assertEqual(result["completeness"], "incomplete", result["reason"])
            self.assertIn(diagnostic, result["reason"])
            self.assert_restored(before)
