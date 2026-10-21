# SPDX-License-Identifier: LGPL-2.1-or-later

"""Selected-pair scan and bounded motion integration tests, collected by CI."""

import math
import unittest

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
