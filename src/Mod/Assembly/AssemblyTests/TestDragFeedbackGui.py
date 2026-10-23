# SPDX-License-Identifier: LGPL-2.1-or-later
"""Displayed native mouse/solver steps, with transient feedback enabled/disabled."""
import os
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import FreeCAD as App
from AssemblyTests import TestMotionInterference as fixtures

if App.GuiUp:
    import FreeCADGui as Gui
    import AssemblyGui
    import AssemblyApp
    from PySide import QtWidgets
    from CommandMotionInterference import MotionInterferenceTask
    from Assembly import MotionInterference


@unittest.skipUnless(App.GuiUp, "GUI required")
class TestDragFeedbackGui(unittest.TestCase):
    def setUp(self):
        Gui.showMainWindow()
        Gui.activateWorkbench("AssemblyWorkbench")
        self.fixture = fixtures.TestMotionSweep()
        self.fixture.setUp()
        self.doc, self.assembly = self.fixture.doc, self.fixture.assembly
        self.docName = self.doc.Name
        self.doc.UndoMode = 1
        self.arm, self.obstacle = self.fixture.arm, self.fixture.obstacle
        Gui.activeDocument().setEdit(self.assembly.Name)
        Gui.Control.closeDialog()
        self.view = Gui.activeDocument().activeView()
        self.view.setCameraType("Orthographic")
        self.view.viewFront()
        self.panel = MotionInterferenceTask(self.assembly)
        self.panel.firstPicker.setCurrentIndex(self.panel.components.index(self.arm))
        self.panel.secondPicker.setCurrentIndex(self.panel.components.index(self.obstacle))
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.assembly, self.arm.Name + ".")
        Gui.updateGui()
        self.view.fitAll()
        self.feedback = None

    def tearDown(self):
        if self.feedback:
            self.feedback.close()
        self.panel.reject()
        Gui.Control.closeDialog()
        Gui.Selection.clearSelection()
        if Gui.activeDocument():
            Gui.activeDocument().resetEdit()
        if self.docName in App.listDocuments():
            self.fixture.tearDown()

    def wait(self, predicate, timeout=5):
        deadline = time.monotonic() + timeout
        while not predicate() and time.monotonic() < deadline:
            QtWidgets.QApplication.processEvents()
            time.sleep(0.002)
        self.assertTrue(predicate(), "Timed out waiting for feedback")

    def enable(self, clearance=None):
        self.feedback = MotionInterference.setDragFeedback(
            self.assembly, [(self.arm, self.obstacle)], clearance=clearance)
        return self.feedback

    def event(self, action, z=0):
        point = self.view.getPointOnScreen(App.Vector(0, 0, z))
        return AssemblyGui._motionDragEvent(self.assembly, action, *point)

    def begin(self):
        self.event("press")
        self.assertTrue(self.event("move"), "Native drag must start")

    def move(self, z):
        # Avoid wall-clock sleeps: advance only the controller's clock in tests.
        with patch("Assembly.DragFeedback.time.monotonic", return_value=max(time.monotonic(), self.feedback._lastStart + .101)):
            self.event("move", z)
        self.assertAlmostEqual(self.arm.Placement.Base.z, z, delta=0.08)
        if self.feedback:
            self.wait(lambda: self.feedback._future is None)

    def test_default_panel_switch_settings_and_close(self):
        Gui.Control.showDialog(self.panel)
        Gui.updateGui()
        self.view.fitAll()
        self.assertFalse(self.panel.dragFeedbackCheck.isChecked())
        from Assembly.DragFeedback import _sessions
        self.assertNotIn((self.doc.Name, self.assembly.Name), _sessions)
        self.panel.clearanceField.setText("1.5 mm")
        self.panel.dragFeedbackCheck.setChecked(True)
        self.feedback = self.panel._dragFeedback
        self.assertTrue(self.feedback.enabled)
        self.assertTrue(self.panel.isAllowedAlterDocument())
        self.begin()
        self.wait(lambda: self.feedback._future is None)
        self.move(5.4)
        self.assertTrue(self.feedback.findings)
        self.panel.reject()
        self.assertFalse(self.feedback.enabled)
        self.assertFalse(self.feedback.findings)
        self.assertIsNone(self.feedback._root)
        self.event("release")

    def test_contact_clearance_separation_release_and_document_invariance(self):
        self.enable(clearance="1.5 mm")
        self.begin()
        self.wait(lambda: self.feedback._future is None)
        self.move(5.4)
        self.assertTrue(self.feedback.findings[0][2])
        self.assertIn("Contact at this pose", self.feedback.message)
        self.assertEqual(Gui.getMainWindow().statusBar().currentMessage(), self.feedback.message)
        self.assertIn("Arm", self.feedback.message)
        self.assertIn("Obstacle", self.feedback.message)
        self.assertIsNotNone(self.feedback._root)
        self.assertTrue(self.feedback._scene.findChild(self.feedback._root) >= 0)
        self.move(4.1)
        self.assertTrue(self.feedback.findings)
        self.assertFalse(self.feedback.findings[0][2])
        self.assertIn("Clearance violation at this pose", self.feedback.message)
        self.assertAlmostEqual(self.feedback.findings[0][1], 0.9, delta=0.08)
        self.move(0)
        self.assertFalse(self.feedback.findings)
        self.assertIsNone(self.feedback._root)
        self.assertEqual(self.feedback.message, "")
        self.move(5.4)
        self.event("release")
        self.assertFalse(self.feedback.findings)
        self.assertIsNone(self.feedback._root)
        # Check a completed scan at an unchanged, clean saved pose, including
        # serialized properties, placements, undo/redo and file modified state.
        with tempfile.TemporaryDirectory() as directory:
            self.doc.saveAs(os.path.join(directory, "Clean.FCStd"))
            before = self.fixture.state()
            self.feedback.step()
            self.wait(lambda: self.feedback._future is None)
            self.assertTrue(self.feedback.findings)
            self.fixture.assert_restored(before)
            self.assertEqual(self.doc.getFileChangeState()["state"], "clean")
            self.feedback.step(True)
            self.fixture.assert_restored(before)

    def test_drag_result_and_history_equal_without_feedback_and_timing(self):
        self.enable()
        self.feedback.close()
        initial = App.Placement(self.arm.Placement)
        results, timings = [], []
        camera = self.view.getCamera()
        for enabled in (False, True):
            self.arm.Placement = initial
            self.view.setCamera(camera)
            Gui.updateGui()
            if enabled:
                self.enable()
            undo, redo = self.doc.UndoCount, self.doc.RedoCount
            self.begin()
            steps = 40
            started = time.perf_counter()
            for index in range(steps):
                self.event("move", (index + 1) * 0.12)
                QtWidgets.QApplication.processEvents()
            timings.append(1000 * (time.perf_counter() - started) / steps)
            self.event("release")
            results.append(([obj.Placement.toMatrix().A for obj in (self.arm, self.obstacle)],
                            self.doc.UndoCount - undo, self.doc.RedoCount - redo,
                            self.doc.getFileChangeState()["state"]))
        self.assertEqual(results[0][1:], results[1][1:])
        self.assertEqual(results[0][1], 1, "Each native drag must create one undo entry")
        difference = max(abs(a - b) for off, on in zip(results[0][0], results[1][0])
                         for a, b in zip(off, on))
        print(f"DRAG PLACEMENT max on/off matrix difference: {difference:.17g}")
        self.assertEqual(results[0][0], results[1][0])
        print(f"DRAG TIMING slider assembly (40 native solver steps): "
              f"off={timings[0]:.3f} ms/step on={timings[1]:.3f} ms/step")

    def test_resting_contact_after_throttle(self):
        self.enable()
        self.begin()
        self.wait(lambda: self.feedback._future is None)
        self.feedback._lastStart = time.monotonic()
        self.event("move", 5.4)
        self.assertIsNone(self.feedback._future, "Throttled step must skip preparation")
        self.wait(lambda: bool(self.feedback.findings))
        self.assertTrue(self.feedback.findings[0][2])
        self.event("release")

    def test_resting_contact_after_busy_and_latest_step_wins(self):
        self.enable()
        self.begin()
        self.wait(lambda: self.feedback._future is None)
        gate, entered = threading.Event(), threading.Event()
        run = AssemblyApp._runPreparedInterferenceScan

        def slow(prepared):
            entered.set()
            gate.wait(5)
            return run(prepared)

        self.addCleanup(gate.set)
        with patch.object(AssemblyApp, "_runPreparedInterferenceScan", side_effect=slow), \
                patch.object(AssemblyApp, "_prepareSelectedPairScan",
                             wraps=AssemblyApp._prepareSelectedPairScan) as prepare:
            self.feedback._lastStart = -float("inf")
            self.event("move", 0)
            self.wait(entered.is_set)
            for z in (4.1, 5.1, 5.4):
                self.event("move", z)
            self.assertEqual(prepare.call_count, 1, "Busy steps skip preparation")
            gate.set()
            self.wait(lambda: bool(self.feedback.findings))
            self.assertEqual(prepare.call_count, 2, "Only latest resting pose is captured")
            self.assertTrue(self.feedback.findings[0][2])
        self.event("release")

    def test_checked_geometry_matches_displayed_placements_including_external_link(self):
        import Part
        source = App.newDocument("DragSource")
        self.addCleanup(lambda: App.closeDocument(source.Name))
        definition = source.addObject("Part::Feature", "Definition")
        definition.Shape = Part.makeBox(1, 1, 1)
        source.recompute()
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        source.saveAs(os.path.join(directory.name, "Source.FCStd"))
        self.doc.saveAs(os.path.join(directory.name, "Owner.FCStd"))
        App.setActiveDocument(self.doc.Name)
        link = self.assembly.newObject("App::Link", "External")
        link.setLink(definition)
        self.doc.recompute()
        properties = {o.FullName: o.PropertiesList for d in (source, self.doc) for o in d.Objects}
        self.feedback = MotionInterference.setDragFeedback(
            self.assembly, [(self.arm, self.obstacle), (link, self.obstacle)])
        self.assertEqual(properties, {o.FullName: o.PropertiesList
                                      for d in (source, self.doc) for o in d.Objects})
        for z in (0, 5.4, 4.1):
            self.arm.Placement.Base.z = z
            link.Placement.Base.z = z + .123
            self.assertIsNotNone(self.feedback._geometry,
                                 "Placement-only steps must reuse owned geometry")
            self.feedback._lastStart = -float("inf")
            run = AssemblyApp._runPreparedInterferenceScan
            checked = []

            def capture(prepared):
                result = run(prepared)
                checked.extend(result["leaves"])
                return result

            with patch.object(AssemblyApp, "_runPreparedInterferenceScan", side_effect=capture):
                self.feedback.step()
                self.wait(lambda: self.feedback._future is None)
            for obj in (self.arm, link):
                leaf = next(l for l in checked if l["occurrenceSubName"] == obj.Name + ".")
                self.assertEqual(leaf["worldShape"].Placement.toMatrix().A,
                                 obj.Placement.toMatrix().A)
                if obj == self.arm:
                    self.assertFalse(leaf["worldShape"].isPartner(obj.Shape),
                                     "Worker topology must be independent of the document")
            self.assertEqual(properties, {o.FullName: o.PropertiesList
                                          for d in (source, self.doc) for o in d.Objects})
        # A terminal link can also display its definition's placement.
        link.LinkTransform = True
        for z in (2, 3):
            definition.Placement.Base.z = z
            self.feedback._lastStart = -float("inf")
            checked.clear()
            with patch.object(AssemblyApp, "_runPreparedInterferenceScan", side_effect=capture):
                self.feedback.step()
                self.wait(lambda: self.feedback._future is None)
            leaf = next(l for l in checked if l["occurrenceSubName"] == link.Name + ".")
            expected = link.Placement * definition.Placement
            self.assertEqual(leaf["worldShape"].Placement.toMatrix().A, expected.toMatrix().A)
        # Non-placement edits must invalidate the owned geometry, even externally.
        definition.Shape = Part.makeBox(3, 1, 1)
        self.assertIsNone(self.feedback._geometry)
        self.feedback._lastStart = -float("inf")
        checked.clear()
        with patch.object(AssemblyApp, "_runPreparedInterferenceScan", side_effect=capture):
            self.feedback.step()
            self.wait(lambda: self.feedback._future is None)
        leaf = next(l for l in checked if l["occurrenceSubName"] == link.Name + ".")
        self.assertEqual(leaf["worldShape"].BoundBox.XLength, 3)
        self.feedback.step(True)

    def test_highlight_excludes_hidden_distant_geometry_from_bounds(self):
        from pivy import coin
        self.obstacle.Placement.Base.x = 1000
        self.obstacle.Visibility = False
        Gui.updateGui()
        self.enable(clearance=2000)

        def bounds():
            action = coin.SoGetBoundingBoxAction(coin.SbViewportRegion(1024, 768))
            action.apply(self.view.getSceneGraph())
            box = action.getBoundingBox()
            return tuple(box.getMin()), tuple(box.getMax())

        self.feedback.step()
        self.wait(lambda: bool(self.feedback.findings))
        # Compare in the same GUI frame: joint labels also resize on redraw.
        highlighted = bounds()
        action = coin.SoGetBoundingBoxAction(coin.SbViewportRegion(1024, 768))
        action.apply(self.feedback._root)
        self.assertTrue(action.getBoundingBox().isEmpty())
        self.feedback.step(True)
        self.assertEqual(highlighted, bounds())

    def test_release_cancels_trailing_request(self):
        self.enable()
        self.feedback._lastStart = time.monotonic()
        self.feedback.step()
        self.feedback.step(True)
        self.assertIsNone(self.feedback._pending)
        self.wait(lambda: not self.feedback._timer.isActive())
        self.assertIsNone(self.feedback._future)

    def test_failure_disables_once_without_interrupting_native_drag(self):
        self.enable()
        with patch.object(AssemblyApp, "_prepareSelectedPairScan", side_effect=RuntimeError("injected")), \
                patch.object(App.Console, "PrintWarning") as warning:
            self.begin()
            self.assertFalse(self.feedback.enabled)
            for z in (2, 3, 4):
                self.event("move", z)
            self.event("release")
            self.assertEqual(warning.call_count, 1)
            self.assertIn("injected", warning.call_args[0][0])
            self.assertAlmostEqual(self.arm.Placement.Base.z, 4, delta=.08)

    def test_release_discards_result_still_in_flight(self):
        self.enable()
        self.begin()
        self.wait(lambda: self.feedback._future is None)
        gate, entered = threading.Event(), threading.Event()
        run = AssemblyApp._runPreparedInterferenceScan

        def slow(prepared):
            result = run(prepared)
            entered.set()
            gate.wait(5)
            return result

        self.addCleanup(gate.set)
        with patch.object(AssemblyApp, "_runPreparedInterferenceScan", side_effect=slow):
            self.feedback._lastStart = -float("inf")
            self.event("move", 5.4)
            self.wait(entered.is_set)
            self.event("release")
            gate.set()
            self.wait(lambda: self.feedback._future is None)
        self.assertFalse(self.feedback.findings)
        self.assertIsNone(self.feedback._root)
        self.assertEqual(self.feedback.message, "")

    def test_worker_failure_unchecks_panel_and_warns_once(self):
        Gui.Control.showDialog(self.panel)
        self.panel.dragFeedbackCheck.setChecked(True)
        self.feedback = self.panel._dragFeedback
        Gui.updateGui()
        self.view.fitAll()
        with patch.object(AssemblyApp, "_runPreparedInterferenceScan",
                          side_effect=RuntimeError("worker failed")), \
                patch.object(App.Console, "PrintWarning") as warning:
            self.begin()
            self.wait(lambda: not self.feedback.enabled)
            self.assertFalse(self.panel.dragFeedbackCheck.isChecked())
            self.event("move", 4)
            self.event("release")
            self.assertEqual(warning.call_count, 1)
            self.assertIn("worker failed", warning.call_args[0][0])
            self.assertAlmostEqual(self.arm.Placement.Base.z, 4, delta=.08)

    def test_sheet_rules_exclusions_and_python_disable(self):
        sheet = self.doc.addObject("Spreadsheet::Sheet", "DragClearance")
        for cell, value in {"A1": "Face", "B1": "Tolerance", "C1": "Comment",
                            "A2": "*", "B2": "1.5 mm", "C2": "Drag rule"}.items():
            sheet.set(cell, value)
        self.doc.recompute()
        self.assembly.InterferenceClearanceSheet = sheet
        self.enable(clearance=0)
        self.begin()
        self.wait(lambda: self.feedback._future is None)
        self.move(4.1)
        self.assertTrue(self.feedback.findings, "Spreadsheet clearance must override zero")
        self.assertFalse(self.feedback.findings[0][2])
        self.assembly.InterferenceExcludedSources = [(self.arm, [""]), (self.obstacle, [""])]
        self.move(5.4)
        self.assertFalse(self.feedback.findings, "Excluded contact must not be highlighted")
        self.assertIsNone(MotionInterference.setDragFeedback(self.assembly, enabled=False))
        self.assertFalse(self.feedback.enabled)
        self.event("release")

    def test_escape_and_document_close_remove_feedback(self):
        self.enable()
        self.begin()
        self.wait(lambda: self.feedback._future is None)
        self.move(5.4)
        self.assertTrue(self.feedback.findings)
        self.event("escape")
        self.assertFalse(self.feedback.findings)
        self.assertIsNone(self.feedback._root)
        self.event("release")
        self.panel.reject()
        Gui.Control.closeDialog()
        Gui.activeDocument().resetEdit()
        self.enable()
        self.feedback.step()
        self.wait(lambda: self.feedback._future is None)
        self.assertTrue(self.feedback.findings)
        App.closeDocument(self.doc.Name)
        self.assertFalse(self.feedback.enabled)
        self.assertIsNone(self.feedback._root)
