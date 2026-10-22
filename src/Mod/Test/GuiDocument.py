# SPDX-License-Identifier: LGPL-2.1-or-later
"""**************************************************************************
*                                                                          *
*   Copyright (c) 2024 Ondsel <development@ondsel.com>                     *
*                                                                          *
*   This file is part of FreeCAD.                                          *
*                                                                          *
*   FreeCAD is free software: you can redistribute it and/or modify it     *
*   under the terms of the GNU Lesser General Public License as            *
*   published by the Free Software Foundation, either version 2.1 of the   *
*   License, or (at your option) any later version.                        *
*                                                                          *
*   FreeCAD is distributed in the hope that it will be useful, but         *
*   WITHOUT ANY WARRANTY; without even the implied warranty of             *
*   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
*   Lesser General Public License for more details.                        *
*                                                                          *
*   You should have received a copy of the GNU Lesser General Public       *
*   License along with FreeCAD. If not, see                                *
*   <https://www.gnu.org/licenses/>.                                       *
*                                                                          *
***************************************************************************/"""

import os
import tempfile
import threading
import time
import unittest
import warnings
import zipfile

import FreeCAD
import FreeCADGui
from PySide6 import QtCore, QtWidgets

from Test.GuiRecompute import (
    close_document,
    recompute_document,
    save_document_as,
    touch_on_owner_thread,
    undo_document,
    wait_for_mutation_ready,
    write_recovery_snapshot,
)

# ---------------------------------------------------------------------------
# define the functions to test the FreeCAD Gui Document code
# ---------------------------------------------------------------------------


class TestGuiDocument(unittest.TestCase):
    def setUp(self):
        # Create a new document
        self.doc = FreeCAD.newDocument("TestDoc")

    def tearDown(self):
        # Close the document
        if self.doc is not None and self.doc.Name in FreeCAD.listDocuments():
            close_document(self.doc)

    def _findAutoSaver(self):
        app = QtWidgets.QApplication.instance()
        self.assertIsNotNone(app)

        for child in app.children():
            meta_object = child.metaObject() if hasattr(child, "metaObject") else None
            if meta_object and meta_object.className() == "Gui::AutoSaver":
                return child

        raise self.failureException("Gui::AutoSaver was not found in the QApplication object tree")

    def _recoveryArchive(self):
        return os.path.join(self.doc.TransientDir, "fc_recovery_file.fcstd")

    def _removeRecoveryArchive(self):
        archive = self._recoveryArchive()
        if os.path.exists(archive):
            os.remove(archive)

    def _invokeAutoSaverFlush(self):
        invoked = QtCore.QMetaObject.invokeMethod(
            self._findAutoSaver(),
            "flushPendingSave",
            QtCore.Qt.ConnectionType.DirectConnection,
            QtCore.Q_ARG(str, self.doc.Name),
        )
        self.assertTrue(invoked)

    def _processEventsUntil(self, predicate, timeout=3.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            QtWidgets.QApplication.processEvents(QtCore.QEventLoop.ProcessEventsFlag.AllEvents, 50)
            if predicate():
                return True
            time.sleep(0.01)

        return predicate()

    def _assertRecoveryArchiveContains(self, expected_label=None):
        archive = self._recoveryArchive()
        self.assertTrue(os.path.isfile(archive))

        with zipfile.ZipFile(archive) as recovery:
            self.assertIn("Document.xml", recovery.namelist())
            self.assertIn("GuiDocument.xml", recovery.namelist())

            if expected_label is not None:
                document_xml = recovery.read("Document.xml").decode("utf-8", errors="replace")
                self.assertIn(expected_label, document_xml)

    def _writeArchiveWithoutGuiDocument(self, source_path, target_path):
        with zipfile.ZipFile(source_path, "r") as source:
            self.assertIn("Document.xml", source.namelist())
            self.assertIn("GuiDocument.xml", source.namelist())

            with zipfile.ZipFile(target_path, "w") as target:
                for item in source.infolist():
                    if item.filename == "GuiDocument.xml":
                        continue

                    target.writestr(item, source.read(item.filename))

        with zipfile.ZipFile(target_path, "r") as target:
            self.assertIn("Document.xml", target.namelist())
            self.assertNotIn("GuiDocument.xml", target.namelist())

    def testGetTreeRootObject(self):
        # Create objects at the root level
        group1 = self.doc.addObject("App::DocumentObjectGroup", "Group1")
        group2 = self.doc.addObject("App::DocumentObjectGroup", "Group2")
        obj1 = self.doc.addObject("App::FeaturePython", "RootObject1")
        part1 = self.doc.addObject("App::Part", "Part1")

        # Create App::Parts and groups with objects in them
        part1_obj = part1.newObject("App::FeaturePython", "Part1_Object")
        group3 = group2.newObject("App::DocumentObjectGroup", "Group1")
        group1_obj = group3.newObject("App::FeaturePython", "Group1_Object")

        # Fetch the root objects using getTreeRootObjects
        root_objects = FreeCADGui.getDocument("TestDoc").TreeRootObjects

        # Check if the new function returns the correct root objects
        expected_root_objects = [group1, group2, obj1, part1]
        self.assertEqual(set(root_objects), set(expected_root_objects))

    def testIssue30418(self):
        class ViewProvider:
            def __init__(self, vobj):
                vobj.Proxy = self

            def attach(self, vobj):
                self.ViewObject = vobj
                self.Object = vobj.Object

            def setEdit(self, vobj, mode):
                return True

            def unsetEdit(self, vobj, mode):
                obj = vobj.Object
                doc = obj.Document
                doc.removeObject(obj.Name)
                return True

        gui = FreeCADGui.getDocument(self.doc)

        self.doc.openTransaction("Add object")
        obj = self.doc.addObject("App::FeaturePython", "Object")
        ViewProvider(obj.ViewObject)
        self.doc.commitTransaction()
        gui.setEdit(obj, 0)
        undo_document(self.doc)

        self._processEventsUntil(lambda: self.doc.getObject("Object") is None, timeout=5.0)

        self.assertTrue(True)

    def testViewObjectRequiresMainThread(self):
        obj = self.doc.addObject("App::FeaturePython", "ThreadGuard")

        result = {}

        def access_view_object():
            try:
                _ = obj.ViewObject
            except Exception as exc:  # pragma: no cover - exercised through assertion below
                result["exception"] = exc

        worker = threading.Thread(target=access_view_object)
        worker.start()
        worker.join()

        self.assertIn("exception", result)
        self.assertIsInstance(result["exception"], RuntimeError)
        self.assertIn("main thread", str(result["exception"]).lower())

    def _queuedPresentationWhileBusy(self, *, failing=False, from_owner=False):
        from Test.GuiRecompute import _pump_gui_until
        from draftutils import gui_utils

        obj = self.doc.addObject("App::FeaturePython", "QueuedPresentation")
        entered, release = threading.Event(), threading.Event()
        errors, calls = [], []
        main_thread = threading.get_ident()

        def hold_lane():
            if from_owner:
                gui_utils.run_gui_setup(obj, lambda: record("A"))
                gui_utils.run_gui_setup(obj, lambda: record("B"))
            entered.set()
            if not release.wait(10):
                raise RuntimeError("presentation test did not release owner")

        def worker():
            try:
                self.doc.commitCompatibilityMutation(hold_lane, structural=False, recompute=False)
            except BaseException as exc:
                errors.append(exc)

        def record(value):
            calls.append((value, threading.get_ident()))
            if failing and value == "A":
                raise ValueError("queued presentation failure")

        thread = threading.Thread(target=worker)
        thread.start()
        try:
            _pump_gui_until(entered.is_set, 5)
            # The real public Draft entry point must queue even on the GUI thread.
            if not from_owner:
                gui_utils.run_gui_setup(obj, lambda: record("A"))
                gui_utils.run_gui_setup(obj, lambda: record("B"))
            self.assertEqual(calls, [])
        finally:
            release.set()
            _pump_gui_until(lambda: not thread.is_alive(), 10)
            thread.join()
        if errors:
            raise errors[0]
        # B is already queued when C arrives after release: C cannot overtake it.
        gui_utils.run_gui_setup(obj, lambda: record("C"))
        _pump_gui_until(lambda: not gui_utils._pending_gui_setups, 5)
        expected = [(v, main_thread) for v in "ABC"]
        self.assertEqual(calls, expected)
        for _ in range(5):
            QtCore.QCoreApplication.processEvents()
        self.assertEqual(calls, expected)  # no duplicate timer or wake delivery

    def testPresentationCallbackWaitsForDocumentLane(self):
        self._queuedPresentationWhileBusy()

    def testQueuedPresentationFromOwnerRunsInOrderOnce(self):
        self._queuedPresentationWhileBusy(from_owner=True)

    def testQueuedPresentationCannotOvertakeDeferredCallback(self):
        from Test.GuiRecompute import _pump_gui_until
        from draftutils import gui_utils
        from unittest.mock import patch

        obj = self.doc.addObject("App::FeaturePython", "OrderedPresentation")
        calls, rejected = [], []
        main_thread = threading.get_ident()
        def record(value):
            calls.append((value, threading.get_ident()))
        # Inject one admission refusal at the native boundary. Delivery and
        # retry use the real Qt queue; the test never retries a callback itself.
        with patch.object(FreeCADGui, "tryRunPresentationCallback",
                          side_effect=lambda *_: rejected.append(True) or False):
            thread = threading.Thread(target=lambda: gui_utils.run_gui_setup(
                obj, lambda: record("A")))
            thread.start()
            thread.join()
            _pump_gui_until(lambda: bool(rejected), 5)
        gui_utils.run_gui_setup(obj, lambda: record("B"))
        _pump_gui_until(lambda: not gui_utils._pending_gui_setups, 5)
        self.assertEqual(calls, [(value, main_thread) for value in "AB"])

    def testQueuedPresentationDropsClosedDocument(self):
        from Test.GuiRecompute import _pump_gui_until
        from draftutils import gui_utils

        obj = self.doc.addObject("App::FeaturePython", "ClosingPresentation")
        calls = []
        thread = threading.Thread(target=lambda: gui_utils.run_gui_setup(
            obj, lambda: calls.append(True)))
        thread.start()
        thread.join()
        # Admit Close before delivering the queued Qt wake. This is the real
        # document lifecycle, with no manual retry of the presentation callback.
        close_document(self.doc)
        self.doc = None
        _pump_gui_until(lambda: not gui_utils._pending_gui_setups, 5)
        self.assertEqual(calls, [])

    def testQueuedPresentationCallbackFailureRunsOnce(self):
        self._queuedPresentationWhileBusy(failing=True)

    def testQueuedPresentationDropsFailedOwner(self):
        from Test.GuiRecompute import _pump_gui_until
        from draftutils import gui_utils
        from types import SimpleNamespace
        from unittest.mock import patch

        calls = []
        # Inject the terminal readiness reported after an owner rollback fails.
        failed = SimpleNamespace(getMutationReadiness=lambda: {"poisoned": True})
        obj = SimpleNamespace(Document=failed, Name="FailedOwnerObject")
        thread = threading.Thread(target=lambda: gui_utils.run_gui_setup(
            obj, lambda: calls.append(threading.get_ident())))
        with patch.object(gui_utils, "_err"):
            thread.start()
            thread.join()
            _pump_gui_until(lambda: not gui_utils._pending_gui_setups, 5)
        self.assertEqual(calls, [])

    def testPresentationCallbackRejectsDeletedDocument(self):
        closed = FreeCAD.newDocument("ClosedPresentationTarget")
        close_document(closed)
        callbacks = []
        with self.assertRaises(ReferenceError):
            FreeCADGui.tryRunPresentationCallback(lambda: callbacks.append(True), closed)
        self.assertFalse(callbacks)

    def testRefreshRunsUnoptedFeaturePythonOnTheOwnerThread(self):
        """An un-opted scripted feature recomputes, on the owner thread.

        A FeaturePython proxy only opts into worker execution when it
        implements supportsAsyncRecompute(); its execute() otherwise lives in a
        proxy the archive cannot carry, so a detached process cannot reproduce
        it. Refusing the job instead would leave the feature permanently
        touched and invalid -- Draft, Arch, FEM, Assembly's joints and every
        user macro included -- so the coordinator runs execute() on the owner
        thread inside its commit boundary.

        What has to hold is that the refresh command does not *block* on it and
        that the proxy runs on the document owner thread, not that the feature
        is left unrecomputed.
        """

        class RefreshProxy:
            def __init__(self):
                self.executed_thread_id = None
                self.execute_count = 0

            def execute(self, obj):
                self.execute_count += 1
                self.executed_thread_id = threading.get_ident()
                time.sleep(0.05)
                obj.Result = 42

        proxy = RefreshProxy()
        obj = self.doc.addObject("App::FeaturePython", "PythonFeature")
        obj.addProperty("App::PropertyInteger", "Result")
        obj.Result = -1
        obj.Proxy = proxy
        touch_on_owner_thread(self.doc, obj)

        main_thread_id = threading.get_ident()
        start = time.monotonic()
        FreeCADGui.runCommand("Std_Refresh", 0)
        elapsed = time.monotonic() - start

        deadline = time.monotonic() + 5.0
        while "Up-to-date" not in obj.State and time.monotonic() < deadline:
            QtCore.QCoreApplication.processEvents(QtCore.QEventLoop.AllEvents, 50)
            FreeCADGui.updateGui()
            time.sleep(0.005)

        # Object state can settle before commit finalization and notification
        # replay finish. Wait for the document before reading live results.
        wait_for_mutation_ready(self.doc, timeout_seconds=5.0)

        # Dispatched, not run inline: the proxy sleeps 50ms and the command
        # still returns immediately.
        self.assertLess(elapsed, 0.033)
        # Executed exactly once on the document owner thread (never a worker).
        self.assertEqual(proxy.execute_count, 1)
        self.assertIsNotNone(proxy.executed_thread_id)
        self.assertNotEqual(proxy.executed_thread_id, main_thread_id)
        # The recompute actually landed rather than failing the node.
        self.assertEqual(obj.Result, 42)
        self.assertIn("Up-to-date", obj.State)
        self.assertNotIn("Invalid", obj.State)
        self.assertNotIn("Touched", obj.State)


    def testSaveCommandDoesNotUseDeprecatedAPI(self):
        with tempfile.TemporaryDirectory() as temp_dir:
            save_document_as(self.doc, os.path.join(temp_dir, "TestDoc.FCStd"))
            self.doc.addObject("App::FeaturePython", "ModifiedObject")

            with warnings.catch_warnings(record=True) as caught_warnings:
                warnings.simplefilter("always", DeprecationWarning)
                FreeCADGui.runCommand("Std_Save", 0)

            deprecations = [
                warning
                for warning in caught_warnings
                if issubclass(warning.category, DeprecationWarning)
            ]
            self.assertEqual(deprecations, [])

    def testRecoverySnapshotIncludesGuiDocument(self):
        self.doc.addObject("App::FeaturePython", "RecoveryGuiObject")

        self.assertTrue(
            write_recovery_snapshot(self.doc, save_thumbnail=False)
        )
        self._processEventsUntil(lambda: os.path.isfile(self._recoveryArchive()))

        self._assertRecoveryArchiveContains()

    def testRestoreWithoutGuiDocumentFitsViewAndRestoresVisibility(self):
        try:
            from pivy import coin  # noqa: F401
        except ImportError as exc:
            raise unittest.SkipTest(f"Coin bindings are unavailable: {exc}") from exc

        try:
            import Part  # noqa: F401
        except ImportError as exc:
            raise unittest.SkipTest(f"Part workbench objects are unavailable: {exc}") from exc

        visible_box = self.doc.addObject("Part::Box", "VisibleBox")
        hidden_cylinder = self.doc.addObject("Part::Cylinder", "HiddenCylinder")

        visible_box.Length = 25
        visible_box.Width = 20
        visible_box.Height = 15
        visible_box.Placement.Base = FreeCAD.Vector(80, 35, 45)
        visible_box.Visibility = True
        visible_box.ViewObject.Visibility = True

        hidden_cylinder.Radius = 5
        hidden_cylinder.Height = 20
        hidden_cylinder.Placement.Base = FreeCAD.Vector(120, 50, 55)
        hidden_cylinder.Visibility = False
        hidden_cylinder.ViewObject.Visibility = False

        recompute_document(self.doc)

        with tempfile.TemporaryDirectory() as temp_dir:
            source_path = os.path.join(temp_dir, "with_gui_document.FCStd")
            target_path = os.path.join(temp_dir, "without_gui_document.FCStd")

            save_document_as(self.doc, source_path)
            self._writeArchiveWithoutGuiDocument(source_path, target_path)

            close_document(self.doc)
            self.doc = None

            restored_doc = FreeCAD.openDocument(target_path)
            self.doc = restored_doc
            try:
                FreeCAD.setActiveDocument(restored_doc.Name)

                view = FreeCADGui.getDocument(restored_doc.Name).ActiveView
                self.assertIsNotNone(view)

                recompute_document(restored_doc)

                self.assertTrue(restored_doc.VisibleBox.ViewObject.Visibility)
                self.assertFalse(restored_doc.HiddenCylinder.Visibility)
                self.assertFalse(restored_doc.HiddenCylinder.ViewObject.Visibility)

                def cameraIsFitted():
                    view.fitAll()
                    FreeCADGui.updateGui()
                    position = view.getCameraNode().position.getValue().getValue()
                    return sum(abs(component) for component in position) > 10.0

                self.assertTrue(
                    self._processEventsUntil(cameraIsFitted, timeout=15.0),
                    view.getCamera(),
                )
            finally:
                if self.doc is not None and self.doc.Name in FreeCAD.listDocuments():
                    close_document(self.doc)
                self.doc = None

    def testAutoSaverFlushWritesRecoverySnapshot(self):
        obj = self.doc.addObject("App::FeaturePython", "AutoSaveGuiObject")
        obj.Label = "AutoSaveImmediate"
        self._removeRecoveryArchive()

        self._invokeAutoSaverFlush()

        self.assertTrue(
            self._processEventsUntil(lambda: os.path.isfile(self._recoveryArchive()), timeout=10.0)
        )
        self._assertRecoveryArchiveContains(expected_label="AutoSaveImmediate")

    def testAutoSaverRetriesWhenDocumentBecomesStable(self):
        obj = self.doc.addObject("App::FeaturePython", "AutoSaveBlockedObject")
        self._removeRecoveryArchive()

        self.doc.openTransaction("AutoSaveBlocked")
        obj.Label = "AutoSaveRetried"
        self._invokeAutoSaverFlush()

        self.assertFalse(os.path.exists(self._recoveryArchive()))

        self.doc.abortTransaction()

        self.assertTrue(self._processEventsUntil(lambda: os.path.exists(self._recoveryArchive())))
        self._assertRecoveryArchiveContains()

    def testAutoSaverStableSignalDoesNotBypassTimeout(self):
        obj = self.doc.addObject("App::FeaturePython", "AutoSaveStableObject")
        self._removeRecoveryArchive()

        self.doc.openTransaction("AutoSaveStable")
        obj.Label = "AutoSaveStillPending"
        self.doc.commitTransaction()

        QtWidgets.QApplication.processEvents(QtCore.QEventLoop.ProcessEventsFlag.AllEvents, 50)
        self.assertFalse(os.path.exists(self._recoveryArchive()))

    def testAutoSaverCoalescesBlockedFlushesToLatestCommittedState(self):
        obj = self.doc.addObject("App::FeaturePython", "AutoSaveChurnObject")
        self._removeRecoveryArchive()

        self.doc.openTransaction("AutoSaveChurn")
        for index in range(8):
            obj.Label = f"AutoSaveBurst{index}"
            self._invokeAutoSaverFlush()

        self.assertFalse(os.path.exists(self._recoveryArchive()))

        self.doc.commitTransaction()

        self.assertTrue(self._processEventsUntil(lambda: os.path.exists(self._recoveryArchive())))
        self._assertRecoveryArchiveContains(expected_label="AutoSaveBurst7")

    def testSaveDispatchesToFocusedMacroEditor(self):
        with tempfile.TemporaryDirectory() as temp_dir:

            save_document_as(self.doc, os.path.join(temp_dir, "TestDoc.FCStd"))
            doc_path = self.doc.FileName

            macro_path = os.path.join(temp_dir, "test_macro.FCMacro")
            with open(macro_path, "w", encoding="utf-8") as macro_file:
                macro_file.write("# original\n")

            FreeCADGui.open(macro_path)

            main_window = FreeCADGui.getMainWindow()

            def active_type_id():
                view = main_window.getActiveWindow()
                return view.getTypeId() if view else ""

            self.assertTrue(self._processEventsUntil(lambda: "EditorView" in active_type_id()))

            editor = main_window.getActiveWindow()

            self.assertIn("EditorView", editor.getTypeId())

            text_edits = [
                widget
                for widget in main_window.findChildren(QtWidgets.QPlainTextEdit)
                if widget.toPlainText().startswith("# original")
            ]

            self.assertEqual(len(text_edits), 1)

            text_edit = text_edits[0]

            editor_closed = []

            def closeEditor():
                if editor_closed:
                    return
                editor_closed.append(True)
                text_edit.document().setModified(False)
                main_window.removeWindow(editor)
                # removeWindow() only schedules the deletion. Delete the editor now, so
                # its file timestamp check cannot ask about the removed temporary file.
                QtWidgets.QApplication.sendPostedEvents(None, QtCore.QEvent.Type.DeferredDelete)

            self.addCleanup(closeEditor)

            text_edit.insertPlainText("# modified\n")

            QtWidgets.QApplication.processEvents(
                QtCore.QEventLoop.ProcessEventsFlag.AllEvents,
                50,
            )

            self.assertTrue(editor.supportMessage("Save"))

            gui_doc = FreeCADGui.getDocument(self.doc.Name)

            self.doc.addObject("App::FeaturePython", "ModifiedObject")

            self.assertTrue(gui_doc.Modified)

            doc_mtime_before = os.stat(doc_path).st_mtime_ns

            FreeCADGui.runCommand("Std_Save", 0)

            with open(macro_path, encoding="utf-8") as macro_file:
                macro_contents = macro_file.read()

            self.assertIn("# modified", macro_contents)
            self.assertTrue(gui_doc.Modified)

            doc_mtime_after = os.stat(doc_path).st_mtime_ns

            # Std_Save must reach the focused macro editor, not the active document,
            # the macro is written and the document is left untouched
            self.assertEqual(doc_mtime_after, doc_mtime_before)

            # close the editor while its file still exists
            closeEditor()


class TestImagePlaneLoading(unittest.TestCase):
    """Image::ImagePlane canvases loaded in the GUI (live MCP report, 2026-10-05).

    After image planes were created, get_mutation_readiness stayed at
    pending_recompute. ViewProviderImagePlane::setPlaneSize writes XSize/YSize
    (and the pixel densities) on every image load, touching the model even when
    the values are unchanged, and the live presentation catch-up after each
    recompute reloads the image.
    """

    def setUp(self):
        self.doc = FreeCAD.newDocument("ImagePlaneLoading")
        self.tempdir = tempfile.TemporaryDirectory()
        self.png = os.path.join(self.tempdir.name, "canvas.png")
        from PySide6 import QtGui

        image = QtGui.QImage(64, 32, QtGui.QImage.Format_RGB32)
        image.fill(0x336699)
        self.assertTrue(image.save(self.png))
        self.plane = self.doc.addObject("Image::ImagePlane", "Canvas")
        self.plane.ImageFile = self.png

    def tearDown(self):
        close_document(self.doc)
        self.tempdir.cleanup()

    def pump(self, seconds=1.0):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline:
            FreeCADGui.updateGui()
            time.sleep(0.02)

    def testPlaneTakesImageAspect(self):
        self.pump()
        self.assertGreater(self.plane.YSize.Value, 0)
        self.assertAlmostEqual(self.plane.XSize.Value / self.plane.YSize.Value, 2.0, places=6)

    def testRecomputeLeavesNothingPending(self):
        recompute_document(self.doc, timeout_seconds=20.0)
        self.pump()
        self.assertFalse(self.doc.mustExecute(), self.doc.getMutationReadiness())
