# SPDX-License-Identifier: LGPL-2.1-or-later

import os
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

import FreeCAD as App
import Part

if App.GuiUp:
    import FreeCADGui as Gui
    import AssemblyApp
    from PySide import QtCore, QtWidgets
    from CommandMotionInterference import CommandMotionInterference, MotionInterferenceTask

from AssemblyTests.TestMotionInterference import TestMotionSweep as _Fixture


@unittest.skipUnless(App.GuiUp, "GUI required")
class TestMotionInterferenceGui(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        Gui.showMainWindow()

    def setUp(self):
        # Exceptions caught by GUIApplication::notify otherwise leave the test
        # green even though an event handler unwound through Qt.
        self.report = Gui.getMainWindow().findChild(QtWidgets.QTextEdit, "Report view")
        self.assertIsNotNone(self.report, "Report output is required for exception checks")
        errors_enabled = App.Console.GetStatus("ReportOutput", "Err")
        App.Console.SetStatus("ReportOutput", "Err", True)
        self.addCleanup(App.Console.SetStatus, "ReportOutput", "Err", errors_enabled)
        QtCore.QCoreApplication.sendPostedEvents(self.report, QtCore.QEvent.User)
        self.report.clear()
        self.notify_errors = set()

        def capture():
            self.notify_errors.update(line for line in self.report.toPlainText().splitlines()
                                      if "GUIApplication::notify" in line)

        def check_report():
            QtCore.QCoreApplication.sendPostedEvents(self.report, QtCore.QEvent.User)
            self.report.textChanged.disconnect(capture)
            self.assertFalse(self.notify_errors, "\n".join(sorted(self.notify_errors)))

        self.report.textChanged.connect(capture)
        self.addCleanup(check_report)
        Gui.activateWorkbench("AssemblyWorkbench")
        self.fixture = _Fixture()
        self.fixture.setUp()
        self.fixture.revolute()
        self.doc = self.fixture.doc
        self.assembly = self.fixture.assembly
        self.arm = self.fixture.arm
        self.obstacle = self.fixture.obstacle
        self.joint = self.fixture.joint
        self.joint.EnableAngleMin = self.joint.EnableAngleMax = True
        self.joint.AngleMin, self.joint.AngleMax = 0, 90
        self.doc.recompute()
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.assembly, self.arm.Name + ".Face1")
        Gui.Selection.addSelection(self.assembly, self.obstacle.Name + ".Face1")
        self.panel = MotionInterferenceTask(self.assembly)
        Gui.Control.showDialog(self.panel)
        self.panel.stepField.setText("9")
        # Settle the tree's deferred selection expansion before state snapshots.
        timeout = App.ParamGet("User parameter:BaseApp/Preferences/TreeView").GetInt(
            "SelectionTimeout", 100
        )
        loop = QtCore.QEventLoop()
        QtCore.QTimer.singleShot(max(1, timeout) + 20, loop.quit)
        loop.exec_()

    def tearDown(self):
        self.panel.cancel()
        self.panel.restore()
        self.wait(lambda: not self.panel.running and not self.panel.replaying)
        self.panel.reject()
        Gui.Control.closeDialog()
        Gui.Selection.clearSelection()
        if Gui.activeDocument() is not None:
            Gui.activeDocument().resetEdit()
        self.fixture.tearDown()

    def wait(self, condition, timeout=30000):
        loop = QtCore.QEventLoop()
        poll = QtCore.QTimer()
        poll.setInterval(10)
        poll.timeout.connect(lambda: loop.quit() if condition() else None)
        deadline = QtCore.QTimer()
        deadline.setSingleShot(True)
        deadline.timeout.connect(loop.quit)
        poll.start()
        deadline.start(timeout)
        if not condition():
            loop.exec_()
        poll.stop()
        self.assertTrue(condition(), "Timed out waiting for GUI operation")

    def run_panel(self):
        self.panel.runButton.click()
        self.wait(lambda: not self.panel.running)
        message = "Panel closed" if self.panel._closed else self.panel.resultText.text()
        self.assertIsNotNone(self.panel.result, message)
        return self.panel.result

    def replay(self, check, close=False):
        errors = []
        timer = QtCore.QTimer()
        timer.setInterval(10)

        def inspect():
            if self.panel._replayLoop is None:
                return
            timer.stop()
            try:
                check()
            except BaseException as error:
                errors.append(error)
            finally:
                if close:
                    self.assertFalse(self.panel.reject())
                else:
                    self.panel.restoreButton.click()

        timer.timeout.connect(inspect)
        timer.start()
        self.panel.showButton.click()
        self.wait(lambda: not self.panel.replaying)
        timer.stop()
        if errors:
            raise errors[0]

    def test_open_selection_and_enabled_limits_prefill(self):
        self.assertTrue(Gui.Control.activeDialog())
        self.assertEqual(self.panel.components[self.panel.firstPicker.currentIndex()], self.arm)
        self.assertEqual(self.panel.components[self.panel.secondPicker.currentIndex()], self.obstacle)
        self.assertEqual(float(self.panel.startField.text()), 0)
        self.assertEqual(float(self.panel.endField.text()), 90)
        self.assertIn("deg", self.panel.startLabel.text())
        self.assertEqual(CommandMotionInterference().GetResources()["Pixmap"],
                         "Assembly_CheckInterference")

    def test_registered_command_opens_for_active_supported_joint(self):
        self.panel.reject()
        Gui.Control.closeDialog()
        Gui.activeDocument().setEdit(self.assembly.Name)
        command = CommandMotionInterference()
        self.assertTrue(command.IsActive())
        command.Activated()
        self.panel = command.panel
        self.assertTrue(Gui.Control.activeDialog())
        self.assertFalse(command.IsActive())

    def test_run_plain_text_and_restore(self):
        before = self.fixture.state()
        result = self.run_panel()
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertEqual(len(result["samples"]), 11)
        text = self.panel.resultText.text()
        for value in ("Arm", "Obstacle", "deg was clear", "resolution",
                      "sampled + refined", "Smallest clearance"):
            self.assertIn(value, text)
        self.assertTrue(self.panel.showButton.isEnabled())
        self.fixture.assert_restored(before)

    def test_show_contact_witness_and_close_restores_exact_state(self):
        self.run_panel()
        before = self.fixture.state()
        event = self.panel.result["firstContact"]

        def check():
            self.assertEqual(self.arm.Placement.toMatrix().A,
                             event["placements"][self.arm.Name].toMatrix().A)
            self.assertNotEqual(self.arm.Placement.toMatrix().A,
                                App.Placement().toMatrix().A)
            dock = Gui.getMainWindow().findChild(
                QtWidgets.QDockWidget, "Assembly Interference Check")
            self.assertIsNotNone(dock, "Existing checker presentation was not opened")
            table = dock.findChild(QtWidgets.QTableWidget)
            self.assertGreater(table.rowCount(), 0)
            self.assertIn(table.item(0, 0).text(), ("Contact", "Penetration"))
            from pivy import coin
            search = coin.SoSearchAction()
            search.setType(coin.SoCoordinate3.getClassTypeId())
            search.setInterest(coin.SoSearchAction.ALL)
            search.apply(Gui.activeDocument().activeView().getSceneGraph())
            detection = event["scan"]["pairs"][0]["solidDetection"]
            expected = (detection["pointOnFirst"], detection["pointOnSecond"])
            paths = search.getPaths()
            found = False
            for index in range(paths.getLength()):
                points = paths[index].getTail().point
                if points.getNum() != 2:
                    continue
                actual = [points[i].getValue() for i in range(2)]
                if all(abs(a - b) < 1e-5 for point, target in zip(actual, expected)
                       for a, b in zip(point, target)):
                    found = True
                    break
            self.assertTrue(found, "Stored world-space witness is missing from the view")

        self.replay(check, close=True)
        self.assertFalse(Gui.Control.activeDialog())
        self.fixture.assert_restored(before)

    def test_minimum_clearance_show_and_restore_button(self):
        self.joint.AngleMax = 18
        result = self.run_panel()
        self.assertEqual(result["coverage"], "proven")
        self.assertIn("no contact proven", self.panel.resultText.text())
        self.assertIsNone(result["firstContact"])
        self.assertIn("Smallest clearance", self.panel.resultText.text())
        self.panel.eventPicker.setCurrentIndex(len(self.panel.events) - 1)
        before = self.fixture.state()
        self.replay(lambda: self.assertEqual(
            self.arm.Placement.toMatrix().A,
            result["minimumClearance"]["placements"][self.arm.Name].toMatrix().A))
        self.fixture.assert_restored(before)
        self.assertTrue(Gui.Control.activeDialog())

    def test_cancel_mid_run_is_responsive_and_restores(self):
        before = self.fixture.state()
        original = self.panel._progress
        observed = []

        def progress(sample):
            original(sample)
            observed.append(sample["coordinate"])
            if len(observed) == 2:
                QtCore.QTimer.singleShot(0, self.panel.cancelButton.click)

        with patch.object(self.panel, "_progress", side_effect=progress):
            result = self.run_panel()
        self.assertGreaterEqual(len(observed), 2)
        self.assertLess(len(observed), 11)
        self.assertEqual(result["completeness"], "cancelled")
        self.assertIn("cancelled", self.panel.resultText.text())
        self.fixture.assert_restored(before)

    def test_document_thread_preparation_worker_scan_and_cancel(self):
        main = threading.get_ident()
        prepare = AssemblyApp._prepareSelectedPairScan
        run = AssemblyApp._runPreparedInterferenceScan
        prepared_on, ran_on, heartbeats = [], [], []
        release = threading.Event()

        def capture(*args, **kwargs):
            prepared_on.append(threading.get_ident())
            return prepare(*args, **kwargs)

        def worker(snapshot):
            ran_on.append(threading.get_ident())
            release.wait(5)
            return run(snapshot)

        def heartbeat():
            heartbeats.append(True)
            self.panel.cancelButton.click()
            release.set()

        scan = self.panel._scan

        def start_scan(*args, **kwargs):
            QtCore.QTimer.singleShot(100, heartbeat)
            return scan(*args, **kwargs)

        before = self.fixture.state()
        with patch.object(AssemblyApp, "_prepareSelectedPairScan", side_effect=capture), \
                patch.object(AssemblyApp, "_runPreparedInterferenceScan", side_effect=worker), \
                patch.object(self.panel, "_scan", side_effect=start_scan):
            result = self.run_panel()
        self.assertTrue(heartbeats)
        self.assertTrue(prepared_on)
        self.assertTrue(ran_on)
        self.assertEqual(set(prepared_on), {main})
        self.assertNotIn(main, ran_on)
        self.assertEqual(result["completeness"], "cancelled")
        self.fixture.assert_restored(before)

    def test_document_close_is_blocked_during_sweep(self):
        original = self.panel._progress
        attempted = []

        def progress(sample):
            original(sample)
            if not attempted:
                attempted.append(True)
                self.assertFalse(self.doc.isClosable())
                with self.assertRaisesRegex(RuntimeError, "not closable"):
                    App.closeDocument(self.doc.Name)
                self.assertIsNotNone(App.getDocument(self.doc.Name))
                self.panel.cancelButton.click()

        before = self.fixture.state()
        with patch.object(self.panel, "_progress", side_effect=progress):
            result = self.run_panel()
        self.assertTrue(attempted)
        self.assertEqual(result["completeness"], "cancelled")
        self.assertTrue(self.doc.isClosable())
        self.fixture.assert_restored(before)

    def test_worker_failure_restores_and_allows_retry(self):
        before = self.fixture.state()
        with patch.object(AssemblyApp, "_runPreparedInterferenceScan",
                          side_effect=RuntimeError("Injected geometry worker failure")):
            result = self.run_panel()
        self.assertEqual(result["completeness"], "incomplete")
        self.assertIn("Injected geometry worker failure", self.panel.resultText.text())
        self.assertTrue(self.panel.runButton.isEnabled())
        self.fixture.assert_restored(before)
        self.assertEqual(self.run_panel()["completeness"], "complete")

    def test_missing_bounds_have_actionable_error_and_can_rerun(self):
        self.panel.startField.clear()
        self.joint.EnableAngleMin = False
        self.panel.runButton.click()
        self.wait(lambda: not self.panel.running)
        self.assertIn("Missing bound", self.panel.resultText.text())
        self.assertTrue(self.panel.runButton.isEnabled())
        self.assertFalse(self.panel.showButton.isEnabled())
        self.panel.startField.setText("0 deg")
        result = self.run_panel()
        self.assertEqual(result["completeness"], "complete", result["reason"])

    def test_external_edit_invalidates_replay(self):
        self.run_panel()
        self.obstacle.Placement.Base.x += 1
        self.assertFalse(self.panel.showButton.isEnabled())
        self.assertIn("Assembly changed", self.panel.resultText.text())

    def test_close_mid_run_restores_before_panel_removal(self):
        before = self.fixture.state()
        original = self.panel._progress

        def progress(sample):
            original(sample)
            QtCore.QTimer.singleShot(0, self.panel.reject)

        with patch.object(self.panel, "_progress", side_effect=progress):
            result = self.run_panel()
        self.assertEqual(result["completeness"], "cancelled")
        self.assertFalse(Gui.Control.activeDialog())
        self.fixture.assert_restored(before)

    def test_slider_units_limits_and_clearance_event(self):
        self.panel.reject()
        Gui.Control.closeDialog()
        self.fixture.tearDown()
        self.fixture.setUp()
        self.doc = self.fixture.doc
        self.assembly = self.fixture.assembly
        self.arm = self.fixture.arm
        self.obstacle = self.fixture.obstacle
        self.joint = self.fixture.joint
        self.joint.EnableLengthMin = self.joint.EnableLengthMax = True
        self.joint.LengthMin, self.joint.LengthMax = 0, 10
        self.panel = MotionInterferenceTask(self.assembly)
        Gui.Control.showDialog(self.panel)
        self.panel.firstPicker.setCurrentIndex(self.panel.components.index(self.arm))
        self.panel.secondPicker.setCurrentIndex(self.panel.components.index(self.obstacle))
        self.assertIn("mm", self.panel.startLabel.text())
        self.assertEqual(float(self.panel.endField.text()), 10)
        self.panel.clearanceField.setText("1.5 mm")
        before = self.fixture.state()
        result = self.run_panel()
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertAlmostEqual(result["firstClearanceViolation"]["sampledCoordinate"], 4)
        self.assertLessEqual(abs(result["firstClearanceViolation"]["coordinate"] - 3.5), 0.01)
        self.assertAlmostEqual(result["firstContact"]["coordinate"], 5)
        self.assertIn("at 5.0 mm", self.panel.resultText.text())
        self.fixture.assert_restored(before)

    def prepare_history(self):
        self.doc.recompute()
        self.doc.UndoMode = 1
        self.doc.openTransaction("Placement before sweep")
        self.arm.Placement.Base.z = 2
        self.doc.commitTransaction()
        self.doc.openTransaction("Redo before sweep")
        self.arm.Label = "Edited arm"
        self.doc.commitTransaction()
        self.doc.undo()
        self.assertGreater(self.doc.UndoCount, 0)
        self.assertGreater(self.doc.RedoCount, 0)

    def assert_mutations_refused(self):
        self.assertGreater(self.doc.UndoCount, 0)
        self.assertGreater(self.doc.RedoCount, 0)
        operations = [self.doc.undo, self.doc.redo,
                      lambda: self.doc.openTransaction("Reentrant transaction"),
                      self.doc.commitTransaction, self.doc.abortTransaction,
                      self.doc.save, self.arm.touch,
                      lambda: setattr(self.arm, "Shape", Part.makeBox(2, 2, 2)),
                      lambda: self.doc.removeObject(self.arm.Name)]
        for operation in operations:
            with self.assertRaisesRegex(RuntimeError, "temporary motion", msg=str(operation)):
                operation()

    def assert_shape_refused(self):
        with self.assertRaisesRegex(RuntimeError, "temporary motion"):
            self.arm.Shape = Part.makeBox(2, 2, 2)

    def check_scan_wait(self, check, history=False):
        errors, attempted = [], []
        release = threading.Event()
        run = AssemblyApp._runPreparedInterferenceScan

        def worker(snapshot):
            release.wait(5)
            return run(snapshot)

        def inspect():
            attempted.append(True)
            try:
                self.assertFalse(self.panel._owned, "Mutation must arrive during the scan wait")
                check()
            except BaseException as error:
                errors.append(error)
            finally:
                self.panel.cancel()
                release.set()

        scan = self.panel._scan

        def start_scan(*args, **kwargs):
            QtCore.QTimer.singleShot(0, inspect)
            return scan(*args, **kwargs)

        with tempfile.TemporaryDirectory() as directory:
            self.doc.saveAs(os.path.join(directory, "Clean.FCStd"))
            if history:
                self.prepare_history()
            before = self.fixture.state()
            with patch.object(AssemblyApp, "_runPreparedInterferenceScan", side_effect=worker), \
                    patch.object(self.panel, "_scan", side_effect=start_scan):
                self.run_panel()
            self.assertTrue(attempted)
            if errors:
                raise errors[0]
            self.fixture.assert_restored(before)
            if not history:
                self.assertEqual(self.doc.getFileChangeState()["state"], "clean")

    def test_native_mutations_refused_during_scan_wait(self):
        self.check_scan_wait(self.assert_mutations_refused, history=True)

    def test_shape_edit_refused_during_scan_wait(self):
        self.check_scan_wait(self.assert_shape_refused)

    def test_shape_edit_refused_during_replay(self):
        self.prepare_history()
        self.run_panel()
        with tempfile.TemporaryDirectory() as directory:
            self.doc.saveAs(os.path.join(directory, "CleanReplay.FCStd"))
            before = self.fixture.state()
            self.replay(self.assert_shape_refused)
            self.fixture.assert_restored(before)
            self.assertEqual(self.doc.getFileChangeState()["state"], "clean")

    def test_native_mutations_refused_during_replay(self):
        self.prepare_history()
        self.run_panel()
        before = self.fixture.state()
        self.replay(self.assert_mutations_refused)
        self.fixture.assert_restored(before)

    def slider_panel(self, end):
        self.panel.reject()
        Gui.Control.closeDialog()
        self.fixture.tearDown()
        self.fixture.setUp()
        for name in ("doc", "assembly", "arm", "obstacle", "joint"):
            setattr(self, name, getattr(self.fixture, name))
        self.panel = MotionInterferenceTask(self.assembly)
        Gui.Control.showDialog(self.panel)
        self.panel.firstPicker.setCurrentIndex(self.panel.components.index(self.arm))
        self.panel.secondPicker.setCurrentIndex(self.panel.components.index(self.obstacle))
        self.panel.startField.setText("0")
        self.panel.endField.setText(str(end))
        self.panel.stepField.setText("1")

    def test_large_slider_cancel_latency_and_restoration(self):
        self.slider_panel(1000)
        before = self.fixture.state()
        delivered = []
        started = time.monotonic()

        def cancel():
            delivered.append(time.monotonic() - started)
            self.panel.cancel()

        QtCore.QTimer.singleShot(100, cancel)
        result = self.run_panel()
        elapsed = time.monotonic() - started
        self.assertEqual(result["completeness"], "cancelled", result["reason"])
        self.assertTrue(delivered)
        self.assertLess(delivered[0], 3, f"Cancel timer delivered at {delivered[0]:.3f}s")
        self.assertLess(elapsed, 3, f"Cancelled sweep returned at {elapsed:.3f}s")
        self.fixture.assert_restored(before)

    def marker_state(self):
        proxy = self.joint.ViewObject.Proxy
        return [(tuple(marker.transform.translation.getValue()),
                 tuple(marker.transform.rotation.getValue().getValue()))
                for marker in (proxy.switch_JCS1, proxy.switch_JCS2)]

    def test_markers_restored_after_success_cancel_error_and_replay(self):
        self.slider_panel(3)
        before = self.marker_state()
        result = self.run_panel()
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertEqual(self.marker_state(), before)
        # Replay a nonzero pose even when no sampled contact was found.
        event = self.panel.events[-1][0]
        self.panel.eventPicker.setCurrentIndex(len(self.panel.events) - 1)

        def inspect():
            self.assertAlmostEqual(self.marker_state()[1][0][2], event["coordinate"])
            self.assertNotEqual(self.marker_state(), before)

        self.replay(inspect)
        self.assertEqual(self.marker_state(), before)
        original = self.panel._progress

        def cancel(sample):
            original(sample)
            if sample["coordinate"] >= 1:
                self.panel.cancel()

        with patch.object(self.panel, "_progress", side_effect=cancel):
            self.assertEqual(self.run_panel()["completeness"], "cancelled")
        self.assertEqual(self.marker_state(), before)

        def fail(sample):
            if sample["coordinate"] >= 1:
                raise RuntimeError("Original marker failure")

        with patch.object(self.panel, "_progress", side_effect=fail):
            result = self.run_panel()
        self.assertIn("Original marker failure", result["reason"])
        self.assertEqual(self.marker_state(), before)

    def test_refined_minimum_text_show_and_restore(self):
        self.slider_panel(8)
        self.fixture.offset_spheres()
        self.panel.stepField.setText("2")
        before = self.fixture.state()
        result = self.run_panel()
        text = self.panel.resultText.text()
        self.assertIn("Smallest clearance", text)
        self.assertIn("refined to", text)
        self.assertIn("sampled + refined", text)
        minimum = result["minimumClearance"]
        self.assertLess(abs(minimum["coordinate"] - 4.3), 0.02)
        self.panel.eventPicker.setCurrentIndex(len(self.panel.events) - 1)
        self.replay(lambda: self.assertEqual(
            self.arm.Placement.toMatrix().A,
            minimum["placements"][self.arm.Name].toMatrix().A))
        self.fixture.assert_restored(before)

    def test_gap_field_event_show_and_restore(self):
        self.slider_panel(0)
        self.panel.startField.setText("4")
        self.panel.maximumGapField.setText("2 mm")
        before = self.fixture.state()
        result = self.run_panel()
        event = result["firstGapViolation"]
        self.assertLess(abs(event["coordinate"] - 3), 0.01)
        self.assertIn("maximum gap", self.panel.resultText.text())
        index = next(i for i, (record, _, _) in enumerate(self.panel.events) if record is event)
        self.panel.eventPicker.setCurrentIndex(index)
        self.replay(lambda: self.assertEqual(
            self.arm.Placement.toMatrix().A, event["placements"][self.arm.Name].toMatrix().A))
        self.fixture.assert_restored(before)

    def test_refinement_only_contact_text_show_and_restore(self):
        self.slider_panel(8)
        self.arm.Shape = Part.makeSphere(1)
        self.obstacle.Shape = Part.makeSphere(1, App.Vector(1.99, 0, 4.3))
        self.doc.recompute()
        self.panel.stepField.setText("2")
        before = self.fixture.state()
        result = self.run_panel()
        self.assertTrue(all(not sample["contact"] for sample in result["samples"]))
        event = result["firstContact"]
        self.assertIsNotNone(event)
        self.assertNotIn("No finding", self.panel.resultText.text())
        self.assertIn("sampled + refined", self.panel.resultText.text())
        index = next(i for i, (record, _, _) in enumerate(self.panel.events) if record is event)
        self.panel.eventPicker.setCurrentIndex(index)
        self.replay(lambda: self.assertEqual(
            self.arm.Placement.toMatrix().A, event["placements"][self.arm.Name].toMatrix().A))
        self.fixture.assert_restored(before)

    def test_event_probe_minimum_show_and_restore(self):
        self.slider_panel(0)
        self.panel.startField.setText("8")
        self.panel.stepField.setText("2")
        self.panel.clearanceField.setText("1.15")
        self.arm.Shape = Part.makeSphere(1)
        self.obstacle.Shape = Part.makeSphere(1, App.Vector(3, 0, 4.9))
        self.doc.recompute()
        before = self.fixture.state()
        from Assembly import sweepJoint

        def limited(*args, **kwargs):
            return sweepJoint(*args, **kwargs, refinementPoseBudget=1)

        with patch("Assembly.sweepJoint", side_effect=limited):
            result = self.run_panel()
        minimum = result["minimumClearance"]
        self.assertAlmostEqual(minimum["coordinate"], 5)
        self.assertIn("1.001666 mm at 5.0 mm", self.panel.resultText.text())
        self.panel.eventPicker.setCurrentIndex(len(self.panel.events) - 1)
        self.replay(lambda: self.assertAlmostEqual(self.arm.Placement.Base.z, 5))
        self.fixture.assert_restored(before)

    def test_nonconverged_minimum_text(self):
        self.slider_panel(8)
        self.fixture.offset_spheres()
        from Assembly import sweepJoint

        def limited(*args, **kwargs):
            return sweepJoint(*args, **kwargs, refinementPoseBudget=0)

        with patch("Assembly.sweepJoint", side_effect=limited):
            self.run_panel()
        self.assertIn("not converged", self.panel.resultText.text())

    def test_simulation_mode_run_show_restore(self):
        self.panel.reject()
        Gui.Control.closeDialog()
        import UtilsAssembly
        from CommandCreateSimulation import Simulation, Motion

        group = UtilsAssembly.getSimulationGroup(self.assembly)
        sim = group.newObject("App::FeaturePython", "Simulation")
        Simulation(sim)
        motion = self.doc.addObject("App::FeaturePython", "Motion")
        Motion(motion, "Angular", (self.joint, [""]), "pi/2*time")
        sim.Group = [motion]
        sim.cTimeStepOutput = 0.1
        self.doc.recompute()
        self.panel = MotionInterferenceTask(self.assembly)
        Gui.Control.showDialog(self.panel)
        self.panel.modePicker.setCurrentText("Simulation")
        self.assertFalse(self.panel.startField.isEnabled())
        self.assertEqual(self.panel.simulations[self.panel.simulationPicker.currentIndex()], sim)
        self.panel.firstPicker.setCurrentIndex(self.panel.components.index(self.arm))
        self.panel.secondPicker.setCurrentIndex(self.panel.components.index(self.obstacle))
        before = self.fixture.state()
        result = self.run_panel()
        self.assertEqual(result["completeness"], "complete", result["reason"])
        self.assertIn("Minimum sampled", self.panel.resultText.text())
        self.assertIn("frame", self.panel.resultText.text())
        self.assertIn("t =", self.panel.resultText.text())
        self.panel.eventPicker.setCurrentIndex(len(self.panel.events)-1)
        minimum = result["minimumClearance"]
        self.replay(lambda: self.assertEqual(self.arm.Placement.toMatrix().A,
                    minimum["placements"][self.arm.Name].toMatrix().A))
        self.fixture.assert_restored(before)

    def test_coverage_proven_and_unresolved_text(self):
        self.slider_panel(2)
        self.panel.stepField.setText("2")
        result = self.run_panel()
        self.assertEqual(result["coverage"], "proven")
        self.assertIn("no contact proven between Arm and Obstacle", self.panel.resultText.text())
        self.assertIn("lower bound", self.panel.resultText.text())
        from Assembly import sweepJoint
        def disabled(*args, **kwargs):
            return sweepJoint(*args, **kwargs, coverageBudget=0)
        with patch("Assembly.sweepJoint", side_effect=disabled):
            result = self.run_panel()
        self.assertEqual(result["coverage"], "unresolved")
        self.assertIn("unresolved", self.panel.resultText.text())
        self.assertIn("sampled + refined", self.panel.resultText.text())
        self.assertNotIn("no contact proven", self.panel.resultText.text())
