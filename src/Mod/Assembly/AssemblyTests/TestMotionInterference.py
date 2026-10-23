# SPDX-License-Identifier: LGPL-2.1-or-later

"""Selected-pair scan and bounded motion integration tests, collected by CI."""

import io
import math
import os
import tempfile
import threading
import time
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
        self.assertLessEqual(contact["resolution"], 0.01)
        self.assertAlmostEqual(contact["previousClearSample"]["coordinate"],
                               contact["coordinate"] - contact["resolution"])
        self.assertAlmostEqual(result["firstClearanceViolation"]["sampledCoordinate"], 4)
        self.assertLessEqual(abs(result["firstClearanceViolation"]["coordinate"] - 3.5), 0.01)
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
        self.assertAlmostEqual(result["firstContact"]["sampledCoordinate"], 36)
        expected = math.degrees(math.atan2(5, 7) - math.asin(0.5 / math.sqrt(74)))
        self.assertLessEqual(abs(result["firstContact"]["coordinate"] - expected), 0.09)
        self.assertLessEqual(result["firstContact"]["resolution"], 0.09)

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
            self.assertEqual(result["coverage"], "proven")
            self.assertEqual(result["message"], "no contact proven between Arm and Obstacle")
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
        before = self.state()
        observed = []

        def progress(sample):
            if sample["phase"] == "sampling":
                observed.append(member.Placement.Base.z)
            self.assertAlmostEqual(member.Placement.Base.z, sample["coordinate"])
            self.assertAlmostEqual(member.Placement.Base.z, self.arm.Placement.Base.z)

        result = self.sweep(interval=(0, 2), progress=progress)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertEqual(len(observed), 3)
        self.assertAlmostEqual(observed[-1] - observed[0], 2)
        self.assert_restored(before)

    def test_cancel_during_generation_is_prompt_and_restores(self):
        token = Assembly.SweepCancellation()
        timer = threading.Timer(0.1, token.cancel)
        before = self.state()
        started = time.monotonic()
        timer.start()
        try:
            result = self.sweep(interval=(0, 1000), cancel=token)
        finally:
            timer.cancel()
            timer.join()
        elapsed = time.monotonic() - started
        self.assertEqual(result["completeness"], "cancelled", result["reason"])
        self.assertLess(elapsed, 3, f"Cancellation took {elapsed:.3f}s")
        self.assert_restored(before)

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

    def offset_spheres(self):
        # Translating sphere centres (0,0,s) and (3,0,4.3):
        # d(s) = sqrt(9 + (s-4.3)^2) - 2, minimum 1 at s=4.3.
        self.arm.Shape = Part.makeSphere(1)
        self.obstacle.Shape = Part.makeSphere(1, App.Vector(3, 0, 4.3))
        self.doc.recompute()

    def test_refined_slider_minimum_and_distant_clear_measurement(self):
        self.offset_spheres()
        before = self.state()
        result = self.sweep(interval=(0, 8), step=2, clearance=0.1,
                            refinementTolerance=0.001)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        sampled, minimum = result["sampledMinimumClearance"], result["minimumClearance"]
        self.assertGreater(sampled["value"] - 1, 0.01)
        self.assertAlmostEqual(sampled["value"], math.sqrt(9.09) - 2, places=6)
        self.assertLess(abs(minimum["coordinate"] - 4.3), 0.001)
        self.assertLess(abs(minimum["value"] - 1), 0.001)
        self.assertTrue(minimum["converged"])
        self.assertLessEqual(minimum["bracket"][1] - minimum["bracket"][0], 0.001)
        self.assertTrue(minimum["bracket"][0] <= minimum["coordinate"] <= minimum["bracket"][1])
        pair = minimum["scan"]["pairs"][0]
        self.assertTrue(pair["distanceMeasured"])
        self.assertEqual(pair["solidDetection"]["kind"], "Clear")
        self.assertGreater(minimum["value"], result["clearance"])
        print("ANALYTIC slider sampled=%.9f refined=%.9f coordinate=%.9f" %
              (sampled["value"], minimum["value"], minimum["coordinate"]))
        self.assert_restored(before)

    def test_refined_revolute_nonmonotonic_gap(self):
        self.revolute()
        angle = math.radians(37)
        self.arm.Shape = Part.makeSphere(0.25, App.Vector(5, 0, 0))
        self.obstacle.Shape = Part.makeSphere(
            0.25, App.Vector(6 * math.cos(angle), 6 * math.sin(angle), 0))
        self.doc.recompute()
        result = self.sweep(interval=(0, 80), step=20, refinementTolerance="0.002 deg",
                            refinementPoseBudget=32)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        minimum = result["minimumClearance"]
        self.assertGreater(result["sampledMinimumClearance"]["value"] - 0.5, 0.04)
        self.assertLess(abs(minimum["coordinate"] - 37), 0.002)
        self.assertLess(abs(minimum["value"] - 0.5), 0.002)
        self.assertTrue(minimum["converged"])
        print("ANALYTIC revolute sampled=%.9f refined=%.9f coordinate=%.9f" %
              (result["sampledMinimumClearance"]["value"], minimum["value"], minimum["coordinate"]))

    def test_refinement_only_contact_is_registered(self):
        # d(s) = max(0, sqrt(1.99^2 + (s-4.3)^2) - 2).
        self.arm.Shape = Part.makeSphere(1)
        self.obstacle.Shape = Part.makeSphere(1, App.Vector(1.99, 0, 4.3))
        self.doc.recompute()
        observed = []
        result = self.sweep(interval=(0, 8), step=2, progress=observed.append)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertTrue(all(not sample["contact"] for sample in result["samples"]))
        event = result["firstContact"]
        print("ANALYTIC refinement-only contact=%s minimum=%.9f" %
              (event["coordinate"] if event else None, result["minimumClearance"]["value"]))
        self.assertIsNotNone(event)
        expected = 4.3 - math.sqrt(4 - 1.99 ** 2)
        self.assertLess(abs(event["coordinate"] - expected), 0.02)
        self.assertEqual(event["coordinate"], min(p["coordinate"] for p in observed if p["contact"]))
        for key, flag in (("firstContact", "contact"),
                          ("firstClearanceViolation", "clearanceViolation")):
            event = result[key]
            clear = max(p["coordinate"] for p in observed
                        if not p[flag] and p["coordinate"] < event["coordinate"])
            self.assertAlmostEqual(event["previousClearSample"]["coordinate"], clear)
            self.assertEqual(event["bracket"], (clear, event["coordinate"]))
            self.assertAlmostEqual(event["resolution"], event["coordinate"] - clear)
            self.assertLessEqual(event["resolution"], 0.02)
            self.assertAlmostEqual(event["placements"][self.arm.Name].Base.z, event["coordinate"])
        self.assertIn("Finding", result["message"])
        self.assertIn("sampled + refined", result["message"])

    def test_event_bisection_updates_minimum_and_pose(self):
        # d(s) = sqrt(9 + (s-4.9)^2) - 2; the event probe at 5 improves the minimum.
        self.arm.Shape = Part.makeSphere(1)
        self.obstacle.Shape = Part.makeSphere(1, App.Vector(3, 0, 4.9))
        self.doc.recompute()
        observed = []
        result = self.sweep(interval=(8, 0), step=2, clearance=1.15,
                            refinementPoseBudget=1, progress=observed.append)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        minimum = result["minimumClearance"]
        print("ANALYTIC event-probe minimum=%.9f coordinate=%.9f measured=%.9f" %
              (minimum["value"], minimum["coordinate"],
               min(p["minimumClearance"] for p in observed)))
        self.assertAlmostEqual(minimum["value"], math.sqrt(9.01) - 2, places=7)
        self.assertAlmostEqual(minimum["coordinate"], 5)
        self.assertAlmostEqual(minimum["value"], min(p["minimumClearance"] for p in observed))
        self.assertAlmostEqual(result["sampledMinimumClearance"]["value"], math.sqrt(9.81) - 2)
        self.assertAlmostEqual(minimum["placements"][self.arm.Name].Base.z, 5)
        solid = minimum["scan"]["pairs"][0]["solidDetection"]
        a, b = (App.Vector(*solid[key]) for key in ("pointOnFirst", "pointOnSecond"))
        self.assertAlmostEqual((a - b).Length, minimum["value"])
        self.assertTrue(minimum["bracket"][0] <= 5 <= minimum["bracket"][1])
        event = result["firstClearanceViolation"]
        self.assertAlmostEqual(event["previousClearSample"]["coordinate"], 6)
        self.assertEqual(event["bracket"], (5, 6))
        self.assertAlmostEqual(event["resolution"], 1)

    def test_refined_thin_contact_and_clearance_transition(self):
        self.obstacle.Shape = Part.makeBox(2, 2, 0.01, App.Vector(0, -0.5, 5.37))
        self.doc.recompute()
        result = self.sweep(interval=(0, 6), clearance=0.2, refinementTolerance=0.001)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        for key, expected in (("firstContact", 4.37), ("firstClearanceViolation", 4.17)):
            event = result[key]
            self.assertAlmostEqual(event["sampledCoordinate"], 5)
            self.assertLess(abs(event["coordinate"] - expected), 0.001)
            self.assertTrue(event["converged"])
            self.assertLessEqual(event["bracket"][1] - event["bracket"][0], 0.001)
            self.assertTrue(event["bracket"][0] <= expected <= event["bracket"][1])
            self.assertTrue(event["contact" if key == "firstContact" else "clearanceViolation"])
            print("ANALYTIC %s sampled=%.6f refined=%.9f expected=%.6f" %
                  (key, event["sampledCoordinate"], event["coordinate"], expected))

    def test_maximum_gap_transition_in_both_directions(self):
        for interval in ((0, 4), (4, 0)):
            with self.subTest(interval=interval):
                result = self.sweep(interval=interval, maximumGap="2 mm",
                                    refinementTolerance=0.001)
                self.assertEqual(result["completeness"], "complete", result["reason"])
                event = result["firstGapViolation"]
                self.assertIsNotNone(event)
                if interval[0] == 0:
                    self.assertAlmostEqual(event["coordinate"], 0)
                    self.assertEqual(event["bracket"], (0, 0))
                else:
                    self.assertAlmostEqual(event["sampledCoordinate"], 2)
                    self.assertLess(abs(event["coordinate"] - 3), 0.001)
                    self.assertTrue(event["gapViolation"])
                    self.assertGreater(event["minimumClearance"], 2)
                    print("ANALYTIC maximum gap sampled=%.6f refined=%.9f expected=3" %
                          (event["sampledCoordinate"], event["coordinate"]))
                self.assertTrue(event["converged"])
        self.assertIsNone(self.sweep(interval=(0, 2))["firstGapViolation"])

    def test_refinement_budget_exhausted(self):
        self.offset_spheres()
        result = self.sweep(interval=(0, 8), step=2, refinementTolerance=1e-9,
                            refinementPoseBudget=1)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        minimum = result["minimumClearance"]
        self.assertFalse(minimum["converged"])
        self.assertEqual(minimum["refinementPoses"], 1)
        self.assertGreater(minimum["bracket"][1] - minimum["bracket"][0], 1e-9)

    def test_cancel_and_callback_error_during_refinement_restore(self):
        self.offset_spheres()
        for mode in ("cancel", "error"):
            token = Assembly.SweepCancellation()
            observed = []
            before = self.state()

            def progress(sample):
                if sample["phase"] != "refinement":
                    return
                observed.append(sample["coordinate"])
                with self.assertRaisesRegex(RuntimeError, "temporary motion"):
                    self.arm.Shape = Part.makeBox(2, 2, 2)
                if mode == "cancel":
                    token.cancel()
                else:
                    raise RuntimeError("Refinement callback failure")

            result = self.sweep(interval=(0, 8), step=2, cancel=token, progress=progress)
            self.assertTrue(observed)
            self.assertEqual(result["completeness"],
                             "cancelled" if mode == "cancel" else "incomplete", result["reason"])
            self.assertFalse(result["minimumClearance"]["converged"])
            self.assert_restored(before)

    def test_refinement_and_gap_arguments_validated(self):
        for options in (dict(refinementTolerance=0), dict(refinementTolerance=float("nan")),
                        dict(refinementTolerance="1 deg"), dict(refinementPoseBudget=-1),
                        dict(refinementPoseBudget=1.5), dict(maximumGap=-1),
                        dict(maximumGap=float("inf")), dict(maximumGap="2 deg")):
            with self.subTest(options=options), self.assertRaises(ValueError):
                self.sweep(**options)

    def simulation(self, formula="10*time", start=0, end=1, step=0.1):
        from Assembly.MotionInterference import _driver
        import UtilsAssembly

        created = []
        sim = _driver(self.doc, self.joint, 0, 1, 10,
                      "deg" if self.joint.JointType == "Revolute" else "mm", created)
        sim.Group[0].Formula = formula
        sim.aTimeStart, sim.bTimeEnd, sim.cTimeStepOutput = start, end, step
        UtilsAssembly.getSimulationGroup(self.assembly).Group = [sim]
        self.doc.recompute()
        return sim

    def sweep_simulation(self, sim, **options):
        return Assembly.sweepSimulation(self.assembly, sim, self.arm, self.obstacle, **options)

    def gear_simulation(self, formula="pi/2*time"):
        self.revolute()
        follower = self.assembly.newObject("Part::Feature", "Follower")
        follower.Shape = Part.makeSphere(0.5, App.Vector(5, 0, 0))
        self.doc.recompute()
        dependent = self.make_joint("Revolute", self.base, follower)
        dependent.Label = "Follower pivot"
        gear = self.make_joint("Gears", self.arm, follower)
        gear.Distance = gear.Distance2 = 10
        angle = math.radians(-60)
        self.obstacle.Shape = Part.makeSphere(0.5, App.Vector(5*math.cos(angle), 5*math.sin(angle), 0))
        sim = self.simulation(formula)
        return sim, follower, dependent

    def test_simulation_ideal_gear_contact_and_coordinates(self):
        sim, follower, dependent = self.gear_simulation()
        before = self.state()
        result = Assembly.sweepSimulation(self.assembly, sim, follower, self.obstacle,
                                          refinementTolerance=0.00001, refinementPoseBudget=32)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        event = result["firstContact"]
        expected = 60 - math.degrees(2*math.asin(0.1))
        actual = event["drivenCoordinates"][self.joint.Name]["value"]
        self.assertLess(abs(actual - expected), 0.002)
        self.assertAlmostEqual(actual, 90*event["time"], places=5)
        self.assertIsNotNone(event["previousClearSample"])
        self.assertLess(event["previousClearSample"]["time"], event["time"])
        print("ANALYTIC simulation gear contact=%.9f expected=%.9f frame=%s t=%.9f" %
              (actual, expected, event["frame"], event["time"]))
        self.assert_restored(before)

    def test_simulation_four_bar_contact(self):
        self.doc.removeObject(self.joint.Name)
        self.arm.Placement = App.Placement(App.Vector(), App.Rotation(App.Vector(0,0,1), 90))
        self.arm.Shape = Part.makeBox(10, 0.2, 0.2)
        coupler = self.assembly.newObject("Part::Feature", "Coupler")
        coupler.Shape = Part.makeSphere(1, App.Vector(10,0,0))
        coupler.Placement.Base = App.Vector(0,10,0)
        rocker = self.assembly.newObject("Part::Feature", "Rocker")
        rocker.Shape = Part.makeBox(10,0.2,0.2)
        rocker.Placement = App.Placement(App.Vector(20,0,0), App.Rotation(App.Vector(0,0,1),90))
        self.doc.recompute()
        def pin(first, second, p, q):
            joint = self.make_joint("Revolute", first, second)
            with patch("JointObject.solveIfAllowed"):
                joint.Placement1.Base = App.Vector(*p)
                joint.Placement2.Base = App.Vector(*q)
            return joint
        self.joint = pin(self.base, self.arm, (0,0,0), (0,0,0))
        pin(self.arm, coupler, (10,0,0), (0,0,0))
        pin(coupler, rocker, (20,0,0), (10,0,0))
        pin(self.base, rocker, (20,0,0), (0,0,0))
        angle = math.radians(110)
        self.obstacle.Shape = Part.makeSphere(1, App.Vector(10+10*math.cos(angle),10*math.sin(angle),0))
        sim = self.simulation("pi/2+pi/6*time")
        before = self.state()
        result = Assembly.sweepSimulation(self.assembly, sim, coupler, self.obstacle,
                                          refinementTolerance=0.00001, refinementPoseBudget=32)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        event = result["firstContact"]
        expected = 110 - math.degrees(2*math.asin(0.1))
        actual = event["drivenCoordinates"][self.joint.Name]["value"]
        self.assertLess(abs(actual - expected), 0.002)
        print("ANALYTIC simulation four-bar contact=%.9f expected=%.9f t=%.9f" %
              (actual, expected, event["time"]))
        self.assert_restored(before)

    def test_simulation_dependent_limit_mid_trajectory(self):
        sim, follower, dependent = self.gear_simulation("pi/3*sin(pi*time)")
        dependent.EnableAngleMin = True
        dependent.AngleMin = -30
        self.doc.recompute()
        before = self.state()
        result = self.sweep_simulation(sim)
        self.assertEqual(result["completeness"], "incomplete", result["reason"])
        self.assertIn(dependent.Label, result["reason"])
        self.assertIn("limit", result["reason"])
        self.assertLess(result["samples"][-1]["time"], 0.5)
        self.assert_restored(before)

    def test_simulation_driver_full_turn_limit(self):
        self.revolute()
        sim = self.simulation("2*pi*time", step=1)
        self.joint.EnableAngleMax = True
        self.joint.AngleMax = 90
        self.doc.recompute()
        before = self.state()
        result = self.sweep_simulation(sim, refinementPoseBudget=0)
        print("ANALYTIC driver endpoint=360 deg max=90: %s %s" %
              (result["completeness"], result["reason"]))
        self.assertEqual(result["completeness"], "incomplete", result["reason"])
        self.assertIn(self.joint.Name, result["reason"])
        self.assert_restored(before)

    def test_simulation_dependent_full_turn_limit(self):
        # Equal gears make a -360 degree follower turn for a 360 degree driver.
        sim, follower, dependent = self.gear_simulation("2*pi*time")
        sim.cTimeStepOutput = 1
        dependent.EnableAngleMin = True
        dependent.AngleMin = -90
        self.doc.recompute()
        before = self.state()
        result = self.sweep_simulation(sim, refinementPoseBudget=0)
        print("ANALYTIC dependent endpoint=-360 deg min=-90: %s %s" %
              (result["completeness"], result["reason"]))
        self.assertEqual(result["completeness"], "incomplete", result["reason"])
        self.assertIn(dependent.Name, result["reason"])
        self.assert_restored(before)

    def test_simulation_disconnected_driver_is_incomplete(self):
        from Assembly.MotionInterference import _driver

        loose = [self.assembly.newObject("Part::Feature", name)
                 for name in ("LooseBase", "LooseArm")]
        for obj in loose:
            obj.Shape = Part.makeBox(1, 1, 1)
        disconnected = self.make_joint("Slider", *loose)
        disconnected.Label = "Disconnected slider"
        sim = self.simulation()
        extra = _driver(self.doc, disconnected, 0, 5, 10, "mm", [])
        motion = extra.Group[0]
        sim.Group = list(sim.Group) + [motion]
        # Even a stationary prescription must participate in the solver.
        for formula in ("5*time", "0*time"):
            motion.Formula = formula
            self.doc.recompute()
            before = self.state()
            result = Assembly.sweepSimulation(self.assembly, sim, loose[1], self.obstacle,
                                              refinementPoseBudget=0)
            print("ANALYTIC disconnected driver prescribed=%s stationary=0: %s %s" %
                  (formula, result["completeness"], result["reason"]))
            self.assertEqual(result["completeness"], "incomplete", result["reason"])
            self.assertIn(disconnected.Name, result["reason"])
            self.assert_restored(before)

    def test_simulation_screw_dependent_full_turn_limit(self):
        # A 5 mm screw pitch converts 5 mm of slider travel into one turn.
        follower = self.assembly.newObject("Part::Feature", "ScrewFollower")
        follower.Shape = Part.makeSphere(0.5, App.Vector(5, 0, 0))
        self.doc.recompute()
        dependent = self.make_joint("Revolute", self.base, follower)
        screw = self.make_joint("Screw", self.arm, follower)
        screw.Distance = 5
        dependent.EnableAngleMin = True
        dependent.AngleMin = -90
        sim = self.simulation("5*time", step=1)
        before = self.state()
        result = self.sweep_simulation(sim, refinementPoseBudget=0)
        print("ANALYTIC screw dependent endpoint=-360 deg min=-90: %s %s" %
              (result["completeness"], result["reason"]))
        self.assertEqual(result["completeness"], "incomplete", result["reason"])
        self.assertIn(dependent.Name, result["reason"])
        self.assert_restored(before)

    def test_simulation_tiny_range_requires_distinct_end_frame(self):
        # The endpoint is 5 mm from the start despite the tiny time range.
        sim = self.simulation("1e8*time", end=5e-8, step=1)
        before = self.state()
        result = self.sweep_simulation(sim, refinementPoseBudget=0)
        print("ANALYTIC tiny range end=5e-8 s displacement=5 mm: %s %s" %
              (result["completeness"], result["reason"]))
        self.assertEqual(result["completeness"], "incomplete", result["reason"])
        self.assertIn("end frame", result["reason"])
        self.assert_restored(before)

    def test_simulation_nonmonotonic_minimum_absolute_time_initial_value(self):
        self.offset_spheres()
        sim = self.simulation("initialValue+1.6*(time-2)", start=2, end=6, step=1)
        self.arm.Placement.Base.z = 0.3
        before = self.state()
        result = self.sweep_simulation(sim, refinementTolerance="0.001 s", refinementPoseBudget=32)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        minimum = result["minimumClearance"]
        self.assertLess(abs(minimum["time"]-4.5), 0.001)
        self.assertGreater(result["sampledMinimumClearance"]["value"], 1.1)
        self.assertAlmostEqual(minimum["value"], 1, places=5)
        self.assertLess(abs(minimum["drivenCoordinates"][self.joint.Name]["value"] - 4.3), 0.002)
        self.assertEqual([s["time"] for s in result["samples"]], [2,3,4,5,6])
        self.assertEqual([s["frame"] for s in result["samples"]], [1,2,3,4,5])
        print("ANALYTIC simulation minimum=%.9f expected=1 time=%.9f expected=4.5" %
              (minimum["value"], minimum["time"]))
        self.assert_restored(before)

    def test_simulation_maximum_gap_and_frame_sampled_label(self):
        sim = self.simulation("4-4*time")
        before = self.state()
        result = self.sweep_simulation(sim, maximumGap=2, refinementPoseBudget=0)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        event = result["firstGapViolation"]
        self.assertAlmostEqual(event["time"], 0.3)
        self.assertAlmostEqual(event["previousClearSample"]["time"], 0.2)
        self.assertEqual(result["sampling"], "frame-sampled")
        self.assertIn("frame-sampled", result["message"])
        self.assert_restored(before)
        refined = self.sweep_simulation(sim, maximumGap=2, refinementTolerance=0.00001)
        self.assertEqual(refined["completeness"], "complete", refined["reason"])
        event = refined["firstGapViolation"]
        self.assertLess(abs(event["time"]-0.25), 0.00001)
        self.assertTrue(event["gapViolation"])
        print("ANALYTIC simulation maximum gap time=%.9f expected=0.25" % event["time"])
        self.assert_restored(before)

    def test_simulation_cancel_error_and_solver_failure_restore(self):
        sim = self.simulation()
        # Preserve an already generated frame store as well as document state.
        self.assertEqual(self.assembly.generateSimulation(sim), 0)
        self.assembly.updateForFrame(3)
        frame_count = self.assembly.numberOfFrames()
        for mode in ("cancel", "callback", "solver"):
            token = Assembly.SweepCancellation()
            def progress(sample):
                if mode == "cancel":
                    token.cancel()
                elif mode == "callback":
                    raise RuntimeError("Trajectory callback failed")
            if mode == "solver":
                sim.Group[0].Formula = "unknown_function(time)"
            before = self.state()
            result = self.sweep_simulation(sim, cancel=token, progress=progress)
            self.assertEqual(result["completeness"], "cancelled" if mode == "cancel" else "incomplete")
            self.assertTrue(result["reason"])
            self.assert_restored(before)
            self.assertEqual(self.assembly.numberOfFrames(), frame_count)
        self.assembly.updateForFrame(frame_count-1)
        self.assertAlmostEqual(self.arm.Placement.Base.z, 10)

    def test_simulation_missing_unexpected_frames_are_incomplete(self):
        import AssemblyApp
        sim = self.simulation()
        for times in ([0,0,0.1,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1],
                      [0,0,0.1,0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9,1],
                      [0,0,0.1,0.2,0.3,0.4,0.5,0.6,0.7,0.8,0.9]):
            before = self.state()
            with patch.object(AssemblyApp, "_motionSimulationFrameTimes", return_value=times):
                result = self.sweep_simulation(sim)
            self.assertEqual(result["completeness"], "incomplete")
            self.assertIn("frame", result["reason"].lower())
            self.assert_restored(before)

    def test_simulation_invalid_finite_range_and_ownership(self):
        sim = self.simulation()
        for prop, value in (("aTimeStart", float("nan")), ("bTimeEnd", -1),
                            ("cTimeStepOutput", 0), ("cTimeStepOutput", float("inf"))):
            original = getattr(sim, prop)
            setattr(sim, prop, value)
            with self.assertRaises(ValueError):
                self.sweep_simulation(sim)
            setattr(sim, prop, original)
        self.assembly.Group = [o for o in self.assembly.Group if o.TypeId != "Assembly::SimulationGroup"]
        with self.assertRaises(ValueError):
            self.sweep_simulation(sim)

    def test_coverage_thin_obstacle_missed_by_coarse_samples(self):
        self.arm.Shape = Part.makeBox(1, 1, 0.01)
        self.obstacle.Shape = Part.makeBox(1, 1, 0.01, App.Vector(0, 0, 3.37))
        self.doc.recompute()
        before = self.state()
        result = self.sweep(interval=(0, 8), step=8, refinementPoseBudget=0, coverageBudget=64,
                            refinementTolerance=0.001)
        self.assertTrue(all(not s["contact"] for s in result["samples"]))
        event = result["firstContact"]
        self.assertIsNotNone(event, result["reason"])
        self.assertGreaterEqual(event["coordinate"], 3.36 - 1e-6)
        self.assertLessEqual(event["coordinate"], 3.38 + 1e-6)
        print("ANALYTIC coverage thin contact=%.9f expected entry=3.36 exit=3.38" %
              event["coordinate"])
        self.assertEqual(result["coverage"], "unresolved")
        self.assert_restored(before)

    def test_coverage_enter_exit_between_clear_endpoints(self):
        result = self.sweep(interval=(0, 10), step=10, refinementPoseBudget=0, coverageBudget=64)
        self.assertTrue(all(not s["contact"] for s in result["samples"]))
        self.assertIsNotNone(result["firstContact"])
        self.assertAlmostEqual(result["firstContact"]["coordinate"], 5)
        print("ANALYTIC coverage enter/exit contact=5 expected entry=5 exit=8")

    def test_coverage_tangency(self):
        self.arm.Shape = Part.makeSphere(1)
        self.obstacle.Shape = Part.makeSphere(1, App.Vector(2, 0, 4))
        self.doc.recompute()
        result = self.sweep(interval=(0, 8), step=8, refinementPoseBudget=0, coverageBudget=64)
        self.assertIsNotNone(result["firstContact"])
        self.assertAlmostEqual(result["firstContact"]["coordinate"], 4)
        print("ANALYTIC coverage tangency=4 expected=4")

    def test_coverage_revolute_periodic_wrap(self):
        self.revolute()
        self.arm.Shape = Part.makeSphere(0.1, App.Vector(5, 0, 0))
        self.obstacle.Shape = Part.makeSphere(0.1, App.Vector(5, 0, 0))
        self.doc.recompute()
        result = self.sweep(interval=(350, 370), step=20, refinementPoseBudget=0,
                            coverageBudget=64)
        self.assertTrue(all(not s["contact"] for s in result["samples"]))
        self.assertAlmostEqual(result["firstContact"]["coordinate"], 360, places=5)
        self.assertAlmostEqual(result["boundConstant"], math.hypot(5*math.cos(math.radians(10)) + 0.1,
                                               5*math.sin(math.radians(10)) + 0.1), places=5)
        self.assertEqual(result["boundUnit"], "mm/rad")
        print("ANALYTIC coverage wrap=%.9f expected=360 R=%.9f" %
              (result["firstContact"]["coordinate"], result["boundConstant"]))

    def test_coverage_budget_exhausted_and_off(self):
        self.arm.Shape = Part.makeSphere(1)
        self.obstacle.Shape = Part.makeSphere(1, App.Vector(2.01, 0, 4.3))
        self.doc.recompute()
        for budget in (0, 1):
            result = self.sweep(interval=(0, 8), step=8, refinementPoseBudget=0,
                                coverageBudget=budget)
            self.assertEqual(result["coverage"], "unresolved")
            self.assertTrue(result["unresolvedIntervals"])
            self.assertEqual(result["coveragePoses"], budget)
            for low, high in result["unresolvedIntervals"]:
                self.assertLess(low, high)
                self.assertGreaterEqual(low, 0)
                self.assertLessEqual(high, 8)
            self.assertIn("sampled + refined", result["message"])

    def test_coverage_proven_clear_analytic_lower_bound(self):
        result = self.sweep(interval=(0, 2), step=2, refinementPoseBudget=0, coverageBudget=4)
        self.assertEqual(result["coverage"], "proven", result["coverageReason"])
        self.assertEqual(result["unresolvedIntervals"], [])
        self.assertEqual(result["boundConstant"], 1)
        self.assertEqual(result["boundUnit"], "mm/mm")
        # d(0)=5, d(2)=3: (5+3-1*2)/2 = 3 mm.
        self.assertAlmostEqual(result["provenLowerBound"], 3, places=5)
        self.assertIn("no contact proven between Arm and Obstacle", result["message"])
        print("ANALYTIC coverage lower bound=%.9f expected=3 L=1" %
              result["provenLowerBound"])

    def test_coverage_simulation_not_available(self):
        result = self.sweep_simulation(self.simulation("2*time"), refinementPoseBudget=0)
        self.assertEqual(result["coverage"], "not available")
        self.assertIn("trajectory", result["coverageReason"])
        self.assertIsNone(result["boundConstant"])
        self.assertNotIn("no contact proven", result["message"])

    def test_coverage_same_side_not_available(self):
        result = Assembly.sweepJoint(self.assembly, self.joint, self.base, self.obstacle,
                                     interval=(0, 2), step=2, refinementPoseBudget=0)
        self.assertEqual(result["coverage"], "not available")
        self.assertIn("opposite", result["coverageReason"])

    def test_coverage_budget_validation(self):
        for value in (-1, 1.5, True):
            with self.subTest(value=value), self.assertRaises(ValueError):
                self.sweep(coverageBudget=value)

    def test_coverage_cylindrical_one_parameter(self):
        self.doc.removeObject(self.joint.Name)
        self.joint = self.make_joint("Cylindrical", self.base, self.arm)
        self.doc.recompute()
        result = self.sweep(interval=(0, 2), step=2, refinementPoseBudget=0)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertEqual(result["coverage"], "proven", result["coverageReason"])
        self.assertAlmostEqual(result["provenLowerBound"], 3, places=5)
        self.arm.Shape = Part.makeSphere(0.1, App.Vector(5, 0, 0))
        self.obstacle.Shape = Part.makeSphere(0.1, App.Vector(5, 0, 0))
        self.doc.recompute()
        before = self.state()
        result = self.sweep(interval=(350, 370), step=20, motionType="Angular",
                            refinementPoseBudget=0)
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertIsNotNone(result["firstContact"])
        self.assertAlmostEqual(result["firstContact"]["coordinate"], 360, places=5)
        self.assert_restored(before)

    def test_coverage_cancel_and_callback_error_restore(self):
        self.arm.Shape = Part.makeSphere(1)
        self.obstacle.Shape = Part.makeSphere(1, App.Vector(2.01, 0, 4.3))
        self.doc.recompute()
        for mode in ("cancel", "error"):
            token = Assembly.SweepCancellation()
            before = self.state()
            observed = []
            def progress(record):
                if record["phase"] == "refinement":
                    observed.append(record["coordinate"])
                    if mode == "cancel":
                        token.cancel()
                    else:
                        raise RuntimeError("Coverage callback failure")
            result = self.sweep(interval=(0, 8), step=8, refinementPoseBudget=0,
                                cancel=token, progress=progress)
            self.assertTrue(observed)
            self.assertEqual(result["coverage"], "unresolved")
            self.assertTrue(result["unresolvedIntervals"])
            self.assertEqual(result["completeness"],
                             "cancelled" if mode == "cancel" else "incomplete")
            self.assert_restored(before)

    def test_coverage_clearance_and_descending_bound(self):
        result = self.sweep(interval=(2, 0), step=2, clearance=3.1, refinementPoseBudget=0)
        self.assertIsNotNone(result["firstClearanceViolation"])
        self.assertEqual(result["coverage"], "unresolved")
        self.assertNotIn("no contact proven", result["message"])
        for interval in ((0, 2), (2, 0)):
            result = self.sweep(interval=interval, step=2, clearance=2.9, refinementPoseBudget=0)
            self.assertEqual(result["coverage"], "proven")
            self.assertAlmostEqual(result["provenLowerBound"], 3, places=5)

    def test_coverage_dependent_trajectory_not_available(self):
        sim, follower, dependent = self.gear_simulation()
        result = Assembly.sweepSimulation(self.assembly, sim, follower, self.obstacle,
                                          refinementPoseBudget=0)
        self.assertEqual(result["coverage"], "not available")
        self.assertIn("trajectory", result["coverageReason"])
