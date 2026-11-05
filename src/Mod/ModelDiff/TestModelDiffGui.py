# SPDX-License-Identifier: LGPL-2.1-or-later
"""GUI integration and real --diff startup coverage (registered in InitGui)."""

import json
import os
import subprocess
import textwrap
import time
import unittest
from pathlib import Path
from unittest.mock import patch

import FreeCAD as App
import FreeCADGui as Gui
import modeldiff as Diff
import TestModelDiff as Fixtures


class ModelDiffGuiTests(unittest.TestCase):
    setUp = Fixtures.ModelDiffTests.setUp
    tearDown = Fixtures.ModelDiffTests.tearDown
    child_environment = Fixtures.ModelDiffTests.child_environment
    box = Fixtures.ModelDiffTests.box
    save = Fixtures.ModelDiffTests.save
    changes = Fixtures.ModelDiffTests.changes
    _external_pair = Fixtures.ModelDiffTests._external_pair
    test_available_external_link = Fixtures.ModelDiffTests.test_available_external_link
    test_open_edited_external_file_untouched = (
        Fixtures.ModelDiffTests.test_open_edited_external_file_untouched
    )
    test_missing_external_link = Fixtures.ModelDiffTests.test_missing_external_link
    test_moved_mesh_link_keeps_instance_placement = (
        Fixtures.ModelDiffTests.test_moved_mesh_link_keeps_instance_placement
    )
    cli_binary = "FreeCAD"
    test_cli_invalid_inputs_name_path = (
        Fixtures.ModelDiffTests.test_cli_invalid_inputs_name_path
    )

    def compare(self):
        from Test.GuiRecompute import _pump_gui_until

        before = set(App.listDocuments())
        job = Diff.compare_files_async(self.local, self.remote)
        _pump_gui_until(lambda: job.done, 60)
        self.assertIsNone(job.error, str(job.error))
        self.assertEqual(set(App.listDocuments()), before)
        return job.result

    def test_diff_close_without_prompt_and_save_as(self):
        from PySide import QtCore, QtWidgets
        from Test.GuiRecompute import _pump_gui_until, save_document_as

        self.box()
        self.save()
        self.save(True)
        shown = Diff.show_result(self.compare())
        saved = str(Path(self.tmp.name) / "saved difference.FCStd")
        save_document_as(shown, saved)
        self.assertTrue(Path(saved).is_file())
        shown.Label = "Still a disposable comparison"
        prompts = []
        timer = QtCore.QTimer()

        def reject_prompts():
            for widget in QtWidgets.QApplication.topLevelWidgets():
                if isinstance(widget, QtWidgets.QMessageBox) and widget.isVisible():
                    prompts.append(widget.text())
                    widget.reject()

        timer.timeout.connect(reject_prompts)
        timer.start(10)
        name = shown.Name
        try:
            Gui.runCommand("Std_CloseActiveWindow", 0)
            _pump_gui_until(
                lambda: name not in App.listDocuments() or bool(prompts), 10
            )
            self.assertFalse(prompts, prompts)
            self.assertNotIn(name, App.listDocuments())
        finally:
            timer.stop()

    def test_async_comparison_keeps_gui_responsive_and_detached(self):
        from PySide import QtCore
        from Test.GuiRecompute import _pump_gui_until

        self.box()
        self.box("Unrelated")
        self.save()
        import shutil

        shutil.copyfile(self.local, self.remote)
        ticks = []
        timer = QtCore.QTimer()
        timer.timeout.connect(lambda: ticks.append(time.monotonic()))
        timer.start(10)
        try:
            # No GUI-process document restore, extraction or live input reads.
            with patch.object(
                Diff, "_restore", side_effect=AssertionError("GUI restore")
            ), patch.object(
                Diff, "_geometry", side_effect=AssertionError("GUI geometry")
            ), patch.object(
                App, "listDocuments", side_effect=AssertionError("live documents")
            ):
                start = time.monotonic()
                job = Diff.compare_files_async(self.local, self.remote)
                self.assertLess(time.monotonic() - start, 0.2)
                _pump_gui_until(lambda: job.done, 60)
                self.assertIsNone(job.error)
                self.assertTrue(job.result.identical)
                job = Diff.compare_objects_async(self.local, self.remote, "Box")
                _pump_gui_until(lambda: job.done, 60)
            QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
            self.assertTrue(job.done)
            self.assertIsNone(job.error)
            self.assertTrue(job.result.identical)
            self.assertEqual({visual.name for visual in job.result.visuals}, {"Box"})
            self.assertGreaterEqual(len(ticks), 2)
        finally:
            timer.stop()

    def test_async_job_is_destroyed_on_gui_thread(self):
        import threading

        from PySide import QtCore
        from Test.GuiRecompute import _pump_gui_until

        self.box()
        self.save()
        self.save(True)
        decoded, release = threading.Event(), threading.Event()
        threads, destroyed = [], []
        original_thread = threading.Thread
        gui_thread = threading.get_ident()

        def decoder_thread(*, target, daemon):
            def decode():
                try:
                    target()
                finally:
                    decoded.set()
                    # Retain the decoder's job reference after emitting its
                    # queued signal, until the GUI caller has dropped theirs.
                    release.wait(60)

            thread = original_thread(target=decode, daemon=daemon)
            threads.append(thread)
            return thread

        def job_destroyed():
            QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
            return bool(destroyed)

        try:
            with patch("threading.Thread", new=decoder_thread):
                job = Diff.compare_files_async(self.local, self.remote)
                job.destroyed.connect(
                    lambda: destroyed.append(threading.get_ident()),
                    QtCore.Qt.DirectConnection,
                )
                _pump_gui_until(lambda: job.done and decoded.is_set(), 60)
                self.assertIsNone(job.error, str(job.error))
                del job
                release.set()
                _pump_gui_until(job_destroyed, 10)
            self.assertEqual(destroyed, [gui_thread])
        finally:
            release.set()
            for thread in threads:
                thread.join(timeout=10)

    def test_colours_groups_and_faint_common(self):
        self.assertIn("TestModelDiffGui", App.__unit_test__)
        box = self.box()
        self.save()
        box.Placement.Base.x = 5
        self.save(True)
        shown = Diff.show_result(self.compare())
        Gui.updateGui()
        roles = {obj.DiffRole: obj for obj in shown.Changed.Group}
        self.assertEqual(set(roles), {"Added", "Removed", "Unchanged"})
        self.assertNotEqual(
            roles["Added"].ViewObject.ShapeColor, roles["Removed"].ViewObject.ShapeColor
        )
        self.assertEqual(roles["Unchanged"].ViewObject.Transparency, 85)
        self.assertEqual(shown.FileName, "")
        self.assertTrue(all(obj.ViewObject.Visibility for obj in roles.values()))

    def test_hidden_intermediates_not_rendered(self):
        box = self.box()
        hidden = self.box("Intermediate")
        hidden.ViewObject.Visibility = False
        self.save()
        hidden.Length = 30
        box.Length = 20
        self.save(True)
        result = self.compare()
        self.assertEqual({v.name for v in result.visuals}, {"Box"})
        self.assertIn("Intermediate", {obj.name for obj in result.objects})

    def test_hidden_part_hides_children(self):
        part = self.doc.addObject("App::Part", "Container")
        box = self.box()
        part.addObject(box)
        part.ViewObject.Visibility = False
        self.save()
        box.Length = 20
        self.save(True)
        self.assertEqual(self.compare().visuals, [])

    def test_hidden_link_does_not_hide_target(self):
        visible = self.doc.addObject("App::Part", "VisiblePart")
        box = self.box()
        visible.addObject(box)
        hidden = self.doc.addObject("App::Part", "HiddenPart")
        link = self.doc.addObject("App::Link", "Instance")
        link.setLink(visible)
        hidden.addObject(link)
        hidden.ViewObject.Visibility = False
        visible.ViewObject.Visibility = box.ViewObject.Visibility = True
        self.save()
        box.Length = 20
        self.save(True)
        self.assertEqual({v.name for v in self.compare().visuals}, {"Box"})

    def test_report_view_and_shared_entry(self):
        from PySide import QtWidgets

        box = self.box()
        self.save()
        box.Length = 20
        self.save(True)
        result = self.compare()
        with patch.object(App.Console, "PrintMessage") as message:
            from Test.GuiRecompute import _pump_gui_until

            job = Diff.run_cli(self.local, self.remote)
            _pump_gui_until(lambda: job.done, 60)
            self.assertIsNone(job.error, str(job.error))
            shown = job.document
        message.assert_called_once_with(result.report())
        self.assertEqual(shown.DiffReport, result.report())
        report = Gui.getMainWindow().findChild(QtWidgets.QDockWidget, "Report view")
        self.assertIsNotNone(report)
        self.assertFalse(report.isHidden())

    def test_mesh_overlay_colours(self):
        import Mesh

        mesh = self.doc.addObject("Mesh::Feature", "Mesh")
        mesh.Mesh = Mesh.Mesh(
            [(App.Vector(), App.Vector(10, 0, 0), App.Vector(0, 10, 0))]
        )
        self.save()
        mesh.Mesh = Mesh.Mesh(
            [(App.Vector(), App.Vector(20, 0, 0), App.Vector(0, 10, 0))]
        )
        self.save(True)
        shown = Diff.show_result(self.compare())
        self.assertEqual(len(shown.Changed.Group), 2)
        old, new = shown.Changed.Group
        self.assertNotEqual(old.ViewObject.ShapeColor, new.ViewObject.ShapeColor)
        self.assertEqual(old.TypeId, "Mesh::Feature")

    def test_visibility_change_is_reported(self):
        box = self.box()
        box.ViewObject.Visibility = False
        self.save()
        box.ViewObject.Visibility = True
        self.save(True)
        result = self.compare()
        self.assertEqual({v.role for v in result.visuals}, {"Added"})
        change = next(obj for obj in result.objects if obj.name == "Box")
        self.assertEqual(change.properties["View.Visibility"], ("false", "true"))

    def test_cli_gui_startup(self):
        box = self.box()
        self.save()
        box.Placement.Base.x = 5
        self.save(True)
        expected = self.compare().report()
        # Observe the real startup entry point from a child process. The shim
        # delegates to the installed implementation, records the returned GUI
        # document, then closes through the ordinary window close path. A timer
        # rejects and records any save prompt, so prompting is a test failure.
        shim_dir = Path(self.tmp.name) / "shim"
        shim_dir.mkdir()
        output = Path(self.tmp.name) / "gui-result.json"
        (shim_dir / "Init.py").write_text("# Test-only startup observer\n")
        (shim_dir / "InitGui.py").write_text("import diff_cli_observer\n")
        (shim_dir / "diff_cli_observer.py").write_text(textwrap.dedent(f"""\
            import json, os
            import FreeCAD as App
            import FreeCADGui as Gui
            import modeldiff
            _original_run_cli = modeldiff.run_cli
            def run_cli(local, remote):
                outcome = _original_run_cli(local, remote)
                from PySide import QtCore, QtWidgets
                def record(doc):
                    data = {{'report': doc.DiffReport, 'filename': doc.FileName,
                            'sources': [o.SourceName for o in doc.Changed.Group],
                            'roles': sorted(o.DiffRole for o in doc.Changed.Group),
                            'groups': sorted(o.Name for o in doc.Objects
                                             if o.TypeId == 'App::DocumentObjectGroup'),
                            'documents': len(App.listDocuments()), 'pid': os.getpid(), 'prompts': []}}
                    def write():
                        with open({str(output)!r}, 'w') as stream:
                            json.dump(data, stream)
                    def watch():
                        for widget in QtWidgets.QApplication.topLevelWidgets():
                            if isinstance(widget, QtWidgets.QMessageBox) and widget.isVisible():
                                data['prompts'].append(widget.text())
                                write()
                                widget.reject()
                    timer = QtCore.QTimer(Gui.getMainWindow())
                    timer.timeout.connect(watch)
                    timer.start(10)
                    def close():
                        if os.environ.get('MODELDIFF_TEST_CLOSE') == 'quit':
                            def at_quit():
                                data['documents_after_close'] = len(App.listDocuments())
                                write()
                            QtWidgets.QApplication.instance().aboutToQuit.connect(at_quit)
                            Gui.getMainWindow().close()
                        else:
                            Gui.runCommand('Std_CloseActiveWindow', 0)
                            QtCore.QTimer.singleShot(300, finish)
                    def finish():
                        data['documents_after_close'] = len(App.listDocuments())
                        write()
                        Gui.getMainWindow().close()
                    QtCore.QTimer.singleShot(200, close)
                if hasattr(outcome, 'DiffReport'):
                    record(outcome)
                else:
                    outcome.finished.connect(lambda: record(outcome.document))
                return outcome
            modeldiff.run_cli = run_cli
        """))
        binary = str(Path(App.ConfigGet("BinPath")) / "FreeCAD")
        # The running GUI test process is already a single-instance server.
        for options, mode in (
            ([], "document"),
            (["--single-instance"], "document"),
            (["--set-config", "SingleInstance=1"], "quit"),
        ):
            output.unlink(missing_ok=True)
            process = subprocess.run(
                check=False,
                args=[
                    binary,
                    *options,
                    "-M",
                    str(shim_dir),
                    "--diff",
                    self.local,
                    self.remote,
                ],
                text=True,
                capture_output=True,
                timeout=180,
                env=self.child_environment(
                    QTWEBENGINE_DISABLE_SANDBOX="1", MODELDIFF_TEST_CLOSE=mode
                ),
            )
            self.assertEqual(process.returncode, 0, process.stdout + process.stderr)
            self.assertTrue(output.exists(), process.stdout + process.stderr)
            observed = json.loads(output.read_text())
            self.assertEqual(observed["report"], expected)
            self.assertEqual(observed["filename"], "")
            self.assertEqual(observed["roles"], ["Added", "Removed", "Unchanged"])
            self.assertEqual(
                observed["groups"], ["Added", "Changed", "Removed", "Unchanged"]
            )
            self.assertEqual(observed["sources"], ["Box"] * 3)
            self.assertEqual(observed["documents"], 1)
            self.assertEqual(observed["prompts"], [])
            self.assertEqual(observed["documents_after_close"], 0)
            self.assertNotEqual(observed["pid"], os.getpid())
