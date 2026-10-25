# SPDX-License-Identifier: LGPL-2.1-or-later
from contextlib import nullcontext
import threading
import time
from unittest.mock import patch

import FreeCAD as App
import FreeCADGui as Gui
import Part
from PySide import QtCore, QtGui

from SketcherTests.GuiTestCase import SketcherGuiTestCase


class TestSketchReferenceRepairGui(SketcherGuiTestCase):
    def test_command_preview_cancel(self):
        Gui.activateWorkbench("SketcherWorkbench")
        import SketchReferenceRepairGui as repair_gui

        self.flush_gui()
        self.assertTrue(
            any(
                action.objectName() == "Sketcher_RepairReferences"
                for menu in Gui.getMainWindow().menuBar().findChildren(QtGui.QMenu)
                for action in menu.actions()
            )
        )
        self.doc = App.newDocument("ReferenceRepairGui")
        self.doc.UndoMode = 1
        source = self.doc.addObject("PartDesign::Feature", "Source")
        sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        wanted = Part.makeLine(App.Vector(0, 0, 0), App.Vector(10, 0, 0))
        other = Part.makeLine(App.Vector(30, 0, 0), App.Vector(40, 0, 0))
        source.Shape = Part.makeCompound([other, other.copy(), wanted])
        self.recompute_gui_document()
        sketch.addExternal(source.Name, "Edge3")
        self.recompute_gui_document()
        source.Shape = Part.makeCompound([other])
        self.recompute_gui_document()
        source.Shape = Part.makeCompound([wanted.copy(noElementMap=True), other])
        self.recompute_gui_document()
        source.Visibility = False
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(sketch)
        self.flush_gui()
        undo_count = self.doc.UndoCount
        scene = Gui.activeDocument().activeView().getSceneGraph()
        children = scene.getNumChildren()
        self.assertTrue(repair_gui.command.IsActive())
        Gui.runCommand("Sketcher_RepairReferences")
        self.flush_gui()
        dialog = repair_gui.command.dialog
        try:
            self.assertTrue(dialog.isVisible())
            self.assertEqual(dialog.references.count(), 1)
            self.assertTrue(self.wait_until(lambda: dialog.options.count() > 0, 5000))
            dialog.options.setCurrentRow(0)
            self.assertIsNotNone(dialog.preview.node)
            self.assertEqual(scene.getNumChildren(), children + 1)
            self.assertFalse(source.Visibility)
        finally:
            dialog.close()
            self.flush_gui()
        self.assertEqual(scene.getNumChildren(), children)
        self.assertEqual(self.doc.UndoCount, undo_count)
        self.assertEqual(Gui.Selection.getSelection(), [sketch])

        # Reopen through the registered command and exercise its apply path.
        Gui.runCommand("Sketcher_RepairReferences")
        dialog = repair_gui.command.dialog
        try:
            self.assertTrue(self.wait_until(lambda: dialog.options.count() > 0, 5000))
            dialog.options.setCurrentRow(0)
            self.assertTrue(dialog.apply_button.isEnabled())
            dialog.apply()
            self.assertTrue(
                self.wait_until(lambda: dialog.references.count() == 0, 10000),
                dialog.message.text(),
            )
            self.assertIsNone(dialog.preview.node)
            self.assertEqual(self.doc.UndoCount, undo_count + 1)
        finally:
            dialog.close()
            self.flush_gui()
        self.undo_gui_document()
        self.recompute_gui_document()
        from SketchReferenceRepair import broken_references

        self.assertEqual(len(broken_references(sketch)), 1)

    def make_dialog(self):
        import SketchReferenceRepairGui as module

        self.doc = App.newDocument("RepairLifecycle")
        source = self.doc.addObject("PartDesign::Feature", "Source")
        sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        wanted = Part.makeLine(App.Vector(), App.Vector(10, 0, 0))
        other = Part.makeLine(App.Vector(30, 0, 0), App.Vector(40, 0, 0))
        source.Shape = Part.makeCompound([other, other.copy(), wanted])
        self.recompute_gui_document()
        sketch.addExternal(source.Name, "Edge3")
        self.recompute_gui_document()
        source.Shape = Part.makeCompound([other])
        self.recompute_gui_document()
        source.Shape = Part.makeCompound([wanted.copy(noElementMap=True), other])
        self.recompute_gui_document()
        dialog = module.RepairDialog(sketch)
        dialog.show()
        self.assertTrue(self.wait_until(lambda: dialog.options.count() > 0, 5000))
        dialog.options.setCurrentRow(0)
        return module, sketch, dialog

    def test_sketch_deletion_closes_preview(self):
        _module, sketch, dialog = self.make_dialog()
        self.assertIsNotNone(dialog.preview.node)
        try:
            self.doc.removeObject(sketch.Name)
            self.flush_gui()
            self.assertIsNone(dialog.preview.scene)
            self.assertIsNone(dialog.preview.node)
        finally:
            if dialog.preview.scene is not None:
                dialog.close()

    def test_document_close_closes_preview(self):
        _module, _sketch, dialog = self.make_dialog()
        from Test.GuiRecompute import close_document

        try:
            close_document(self.doc)
            self.doc = None
            self.flush_gui()
            self.assertIsNone(dialog.preview.scene)
            self.assertIsNone(dialog.preview.node)
        finally:
            if dialog.preview.scene is not None:
                dialog.close()

    def test_preview_uses_sketch_document_and_command_rejects_foreign_selection(self):
        module, sketch, dialog = self.make_dialog()
        dialog.close()
        self.flush_gui()
        scene = Gui.getDocument(self.doc.Name).activeView().getSceneGraph()
        other_doc = App.newDocument("OtherRepairDocument")
        try:
            from Test.GuiRecompute import refresh_document

            refresh_document(other_doc)
            foreign_scene = Gui.getDocument(other_doc.Name).activeView().getSceneGraph()
            original_count = foreign_scene.getNumChildren()
            dialog = module.RepairDialog(sketch)
            dialog.show()
            self.assertTrue(self.wait_until(lambda: dialog.options.count() > 0, 5000))
            dialog.options.setCurrentRow(0)
            self.assertEqual(dialog.preview.scene, scene)
            self.assertEqual(foreign_scene.getNumChildren(), original_count)
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(sketch)
            self.assertFalse(module.command.IsActive())
        finally:
            dialog.close()
            from Test.GuiRecompute import close_document

            close_document(other_doc)

    def test_apply_returns_while_owner_work_is_pending(self):
        _module, _sketch, dialog = self.make_dialog()
        import SketchReferenceRepair as repair_module

        entered, release = threading.Event(), threading.Event()
        original = repair_module.broken_references
        threads = []

        def stalled(target):
            if target.Document.HasPendingTransaction:
                threads.append(QtCore.QThread.currentThread())
                entered.set()
                if not release.wait(5):
                    raise RuntimeError("test owner work timed out")
            return original(target)

        try:
            with patch.object(repair_module, "broken_references", stalled):
                QtCore.QTimer.singleShot(100, release.set)
                started = time.monotonic()
                dialog.apply()
                self.assertLess(
                    time.monotonic() - started, 0.5, "apply blocked the GUI"
                )
                # A sync call will have waited for the timer/test timeout here.
                self.assertFalse(release.is_set(), "apply blocked the GUI")
                self.assertTrue(self.wait_until(entered.is_set, 5000))
                self.assertTrue(self.wait_until(lambda: not dialog.busy, 10000))
            self.assertTrue(threads)
            self.assertTrue(
                all(t != QtCore.QCoreApplication.instance().thread() for t in threads)
            )
            self.assertEqual(dialog.references.count(), 0, dialog.message.text())
        finally:
            release.set()
            dialog.close()

    def test_ranking_yields_to_gui_timer(self):
        _module, sketch, dialog = self.make_dialog()
        from SketchReferenceRepair import candidates

        ref = dialog.refs[0]
        expected = candidates(sketch, ref)
        dialog.ranked = []
        tick = []
        QtCore.QTimer.singleShot(0, lambda: tick.append(True))
        try:
            dialog.select_reference(0)
            self.assertEqual(
                dialog.ranked, [], "ranking completed on the GUI call stack"
            )
            self.assertTrue(
                self.wait_until(lambda: len(dialog.ranked) == len(expected), 5000)
            )
            self.assertTrue(tick)
        finally:
            dialog.close()

    def make_pick_fixture(self, vertex=False, switch_workbench=True, replacement=None):
        import Sketcher
        from SketcherTests.TestSketchReferenceRepair import signature

        Gui.activateWorkbench(
            "SketcherWorkbench" if switch_workbench else "PartWorkbench"
        )
        import SketcherGui  # noqa: F401 - native editor, without Python workbench initialization

        self.doc = App.newDocument("ReassignInView")
        self.doc.UndoMode = 1
        source = self.doc.addObject("PartDesign::Feature", "Source")
        sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        start, end = App.Vector(0, 10, 0), App.Vector(10, 10, 0)
        wanted = Part.Vertex(start) if vertex else Part.makeLine(start, end)
        other = Part.makeLine(App.Vector(30, 10, 0), App.Vector(40, 10, 0))
        source.Shape = Part.makeCompound([other, other.copy(), wanted])
        self.recompute_gui_document()
        sketch.addExternal(source.Name, "Vertex5" if vertex else "Edge3")
        sketch.addGeometry(Part.LineSegment(start, App.Vector(0, 15, 0)))
        sketch.addConstraint(Sketcher.Constraint("Coincident", 0, 1, -3, 1))
        dim = sketch.addConstraint(Sketcher.Constraint("Distance", 0, 5.0))
        sketch.renameConstraint(dim, "Height")
        sketch.setExpression("Constraints.Height", "2 + 3")
        refdim = (
            sketch.addConstraint(Sketcher.Constraint("DistanceX", -3, 1, -3, 2, 10.0))
            if not vertex
            else None
        )
        if refdim is not None:
            sketch.setDriving(refdim, False)
            sketch.renameConstraint(refdim, "ExternalWidth")
            sketch.setVirtualSpace(refdim, True)
            sketch.setActive(refdim, False)
        self.recompute_gui_document()
        source.Shape = Part.makeCompound([other])
        self.recompute_gui_document()
        source.Shape = Part.makeCompound([
            wanted.copy(noElementMap=True) if replacement is None else replacement, other
        ])
        self.recompute_gui_document()
        source.Visibility = True
        Gui.getMainWindow().show()
        if not switch_workbench:
            sketch.ViewObject.EditingWorkbench = ""
        Gui.activeDocument().setEdit(sketch.Name)
        view = Gui.activeDocument().activeView()
        view.viewTop()
        view.fitAll()
        self.pump(200)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(sketch, "Vertex3" if vertex else "ExternalEdge1")
        return sketch, source, view, signature(sketch)

    def test_context_entry_pick_constraints_and_single_undo(self):
        import SketchReferenceReassignGui as pick_gui
        from SketchReferenceRepair import broken_references
        from SketcherTests.TestSketchReferenceRepair import signature

        sketch, source, view, before = self.make_pick_fixture()
        undo_count = self.doc.UndoCount
        provider_root = sketch.ViewObject.RootNode
        provider_children = provider_root.getNumChildren()
        self.assertTrue(pick_gui.command.IsActive())
        viewport = view.graphicsView().viewport()
        point = self.viewport_to_qpoint(
            view, viewport, view.getPointOnScreen(App.Vector(5, 10, 0))
        )
        seen = []

        def choose():
            menu = QtGui.QApplication.activePopupWidget()
            if isinstance(menu, QtGui.QMenu):
                for action in menu.actions():
                    if action.objectName() == "Sketcher_ReassignBrokenReference":
                        seen.append((action.text(), action.isEnabled()))
                        action.trigger()
                        menu.close()
                        return
                menu.close()

        QtCore.QTimer.singleShot(100, choose)
        self.right_click(viewport, point)
        self.assertTrue(seen, "Broken geometry context menu has no reassign action")
        self.assertTrue(seen[0][1], "Reassign action is disabled during sketch editing")
        session = pick_gui.command.session
        self.assertIsNotNone(session)
        try:
            self.assertFalse(session.closed)
            self.assertEqual(str(session.root.getName()), "Sketch_EditRoot")
            self.assertEqual(provider_root.getNumChildren(), provider_children)
            self.assertAlmostEqual(
                session.material.transparency.getValues()[0], 0.65, places=5
            )
            self.assertEqual(self.doc.UndoCount, undo_count)
            self.assertIs(Gui.activeDocument().getInEdit().Object, sketch)
            # Use the actual viewport event and ray pick, including overlapping cached geometry.
            self.click(viewport, point)
            self.assertTrue(
                self.wait_until(lambda: session.closed, 10000),
                "Viewport repair did not complete",
            )
            self.assertFalse(broken_references(sketch))
            self.assertEqual(signature(sketch), before)
            self.assertEqual(self.doc.UndoCount, undo_count + 1)
            self.assertIsNone(session.material)
            self.assertTrue(source.Visibility)
            self.undo_gui_document()
            self.recompute_gui_document()
            self.assertEqual(len(broken_references(sketch)), 1)
            self.assertEqual(signature(sketch), before)
        finally:
            if not session.closed:
                session.close()

    def test_pick_escape_restores_scene_and_selection_without_undo(self):
        import SketchReferenceReassignGui as pick_gui
        from SketchReferenceRepair import broken_references

        sketch, source, view, _before = self.make_pick_fixture(vertex=True)
        self.assertTrue(pick_gui.command.IsActive())
        undo_count = self.doc.UndoCount
        original = list(Gui.Selection.getSelectionEx()[0].SubElementNames)
        Gui.runCommand("Sketcher_ReassignBrokenReference")
        session = pick_gui.command.session
        root = session.root
        try:
            self.assertTrue(session.isVisible())
            self.key_click(view.graphicsView().viewport(), QtCore.Qt.Key_Escape)
            self.assertTrue(session.closed)
            self.assertIsNone(session.material)
            self.assertFalse(
                any(
                    str(root.getChild(i).getName()) == "ReferenceRepairTransparency"
                    for i in range(root.getNumChildren())
                )
            )
            self.assertEqual(self.doc.UndoCount, undo_count)
            self.assertEqual(len(broken_references(sketch)), 1)
            self.assertEqual(
                list(Gui.Selection.getSelectionEx()[0].SubElementNames), original
            )
            self.assertTrue(source.Visibility)
            self.assertIs(Gui.activeDocument().getInEdit().Object, sketch)
        finally:
            if not session.closed:
                session.close()

    def test_ranked_fallback_targets_clicked_reference(self):
        import SketchReferenceReassignGui as pick_gui

        sketch, _source, _view, _before = self.make_pick_fixture()
        Gui.runCommand("Sketcher_ReassignBrokenReference")
        session = pick_gui.command.session
        session.fallback()
        dialog = pick_gui.command.dialog
        try:
            self.assertTrue(session.closed)
            self.assertTrue(dialog.isVisible())
            self.assertEqual(dialog.refs[dialog.references.currentRow()].geo_id, -3)
            self.assertTrue(self.wait_until(lambda: dialog.options.count() > 0, 5000))
        finally:
            dialog.close()

    def test_pick_closes_when_edit_ends(self):
        import SketchReferenceReassignGui as pick_gui

        self.make_pick_fixture()
        Gui.runCommand("Sketcher_ReassignBrokenReference")
        session = pick_gui.command.session
        Gui.activeDocument().resetEdit()
        self.assertTrue(self.wait_until(lambda: session.closed, 2000))
        self.assertIsNone(session.material)

    def test_vertex_pick_and_incompatible_pick_is_read_only(self):
        import SketchReferenceReassignGui as pick_gui
        from SketchReferenceRepair import broken_references
        from SketcherTests.TestSketchReferenceRepair import signature

        sketch, _source, view, before = self.make_pick_fixture(vertex=True)
        undo_count = self.doc.UndoCount
        Gui.runCommand("Sketcher_ReassignBrokenReference")
        session = pick_gui.command.session
        viewport = view.graphicsView().viewport()
        try:
            invalid = self.viewport_to_qpoint(
                view, viewport, view.getPointOnScreen(App.Vector(35, 10, 0))
            )
            self.click(viewport, invalid)
            self.assertFalse(session.closed)
            self.assertFalse(session.busy)
            self.assertEqual(self.doc.UndoCount, undo_count)
            self.assertEqual(signature(sketch), before)
            point = self.viewport_to_qpoint(
                view, viewport, view.getPointOnScreen(App.Vector(0, 10, 0))
            )
            self.click(viewport, point)
            self.assertTrue(
                self.wait_until(lambda: session.closed, 10000),
                "Viewport repair did not complete",
            )
            self.assertFalse(broken_references(sketch))
            self.assertEqual(signature(sketch), before)
            self.assertEqual(self.doc.UndoCount, undo_count + 1)
        finally:
            if not session.closed:
                session.close()

    def test_context_entry_only_for_a_single_broken_external(self):
        import SketchReferenceReassignGui as pick_gui
        import SketchReferenceRepairGui as repair_gui

        sketch, _source, _view, _before = self.make_pick_fixture()
        self.assertTrue(repair_gui.menu_manipulator.modifyContextMenu("Sketch"))
        self.assertEqual(repair_gui.menu_manipulator.modifyContextMenu("View"), [])
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(sketch, "Edge1")
        self.assertFalse(pick_gui.command.IsActive())
        self.assertEqual(repair_gui.menu_manipulator.modifyContextMenu("Sketch"), [])
        Gui.Selection.addSelection(sketch, "ExternalEdge1")
        self.assertFalse(pick_gui.command.IsActive())
        Gui.activeDocument().resetEdit()
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(sketch, "ExternalEdge1")
        self.assertFalse(pick_gui.command.IsActive())

    def test_viewport_validation_yields_to_gui(self):
        import SketchReferenceReassignGui as pick_gui

        self.make_pick_fixture()
        Gui.runCommand("Sketcher_ReassignBrokenReference")
        session = pick_gui.command.session
        entered, release = threading.Event(), threading.Event()
        original = pick_gui.picked_candidate
        threads = []

        def stalled(*args):
            threads.append(QtCore.QThread.currentThread())
            entered.set()
            if not release.wait(5):
                raise RuntimeError("test validation timed out")
            return original(*args)

        try:
            with patch.object(pick_gui, "picked_candidate", stalled):
                QtCore.QTimer.singleShot(100, release.set)
                point = session.view.getPointOnScreen(App.Vector(5, 10, 0))
                started = time.monotonic()
                session.pick(tuple(point))
                self.assertLess(time.monotonic() - started, 0.5)
                self.assertFalse(release.is_set(), "validation blocked the GUI")
                self.assertTrue(self.wait_until(entered.is_set, 5000))
                self.assertTrue(self.wait_until(lambda: session.closed, 10000))
            self.assertTrue(threads)
            self.assertTrue(
                all(t != QtCore.QCoreApplication.instance().thread() for t in threads)
            )
        finally:
            release.set()
            if not session.closed:
                session.close()

    def test_context_entry_with_workbench_switch_disabled(self):
        # Run this method by itself in a fresh FreeCAD process too: importing
        # the Python repair command before opening the menu would hide a native
        # initialization regression.
        _sketch, _source, view, _before = self.make_pick_fixture(switch_workbench=False)
        self.assertEqual(Gui.activeWorkbench().name(), "PartWorkbench")
        import os
        import sys

        if os.environ.get("FREECAD_REPAIR_FRESH_CONTEXT") == "1":
            self.assertNotIn("SketchReferenceRepairGui", sys.modules)
        viewport = view.graphicsView().viewport()
        point = self.viewport_to_qpoint(
            view, viewport, view.getPointOnScreen(App.Vector(5, 10, 0))
        )
        seen = []

        def inspect():
            menu = QtGui.QApplication.activePopupWidget()
            if isinstance(menu, QtGui.QMenu):
                seen.extend(action.objectName() for action in menu.actions())
                menu.close()

        QtCore.QTimer.singleShot(100, inspect)
        self.right_click(viewport, point)
        self.assertIn("Sketcher_ReassignBrokenReference", seen)


    def assert_pending_pick_cancelled(self, action, phase="validation"):
        import SketchReferenceReassignGui as pick_gui
        import SketchReferenceRepairGui as repair_gui
        from SketchReferenceRepair import broken_references
        from SketcherTests.TestSketchReferenceRepair import signature
        from Test.GuiRecompute import close_document

        sketch, source, view, before = self.make_pick_fixture()
        undo_count, redo_count = self.doc.UndoCount, self.doc.RedoCount
        deleted_history = []

        class HistoryObserver:
            def slotDeletedDocument(observer, document):
                if document is target_document:
                    deleted_history.append((document.UndoCount, document.RedoCount))

        target_document = self.doc
        history_observer = HistoryObserver()
        App.addDocumentObserver(history_observer)
        original_selection = [
            (item.ObjectName, tuple(item.SubElementNames))
            for item in Gui.Selection.getSelectionEx()
        ]
        Gui.runCommand("Sketcher_ReassignBrokenReference")
        session = pick_gui.command.session
        root = session.root
        root.ref()  # Inspect cleanup even after resetEdit/document destruction.
        entered, release = threading.Event(), threading.Event()
        module, name = {
            "validation": (pick_gui, "picked_candidate"),
            "submission": (repair_gui, "prepare_repair"),
            "mutation": (repair_gui, "apply_prepared"),
            "commit": (repair_gui, "check_repair"),
        }[phase]
        original = getattr(module, name)

        def stalled(*args):
            # For commit, pause after the model/postcondition work, immediately
            # before returning to the native transaction commit.
            result = original(*args) if phase != "mutation" else None
            entered.set()
            if not release.wait(15):
                raise RuntimeError("test pending repair timed out")
            return original(*args) if phase == "mutation" else result

        prepares = []
        original_prepare = repair_gui.prepare_repair

        def record_prepare(*args):
            prepares.append(True)
            return original_prepare(*args)

        try:
            with patch.object(module, name, stalled):
                with (
                    patch.object(repair_gui, "prepare_repair", record_prepare)
                    if phase == "validation" else nullcontext()
                ):
                    point = view.getPointOnScreen(App.Vector(5, 10, 0))
                    session.pick(tuple(point))
                    self.assertTrue(self.wait_until(entered.is_set, 5000))
                    if action == "escape":
                        self.key_click(view.graphicsView().viewport(), QtCore.Qt.Key_Escape)
                    elif action == "button":
                        session.cancel_button.click()
                    elif action == "edit":
                        Gui.activeDocument().resetEdit()
                    elif action == "document":
                        close_document(self.doc)
                        self.doc = None
                    else:
                        session.close()
                    lifecycle_selection = [
                        (item.ObjectName, tuple(item.SubElementNames))
                        for item in Gui.Selection.getSelectionEx()
                    ]
                    closed = self.wait_until(lambda: session.closed, 2000)
                    release.set()
                    self.assertTrue(self.wait_until(lambda: not repair_gui._operations, 10000))
            self.assertTrue(closed, "pending picker could not be cancelled")
            self.assertIsNone(session.material)
            self.assertFalse(any(
                str(root.getChild(i).getName()) == "ReferenceRepairTransparency"
                for i in range(root.getNumChildren())
            ))
            if phase == "validation":
                self.assertFalse(prepares, "cancelled validation submitted a repair")
            self.assertEqual([
                (item.ObjectName, tuple(item.SubElementNames))
                for item in Gui.Selection.getSelectionEx()
            ], lifecycle_selection)
            if action == "document":
                self.assertEqual(deleted_history, [(undo_count, redo_count)])
                self.assertEqual(Gui.Selection.getSelection(), [])
            else:
                self.assertEqual((self.doc.UndoCount, self.doc.RedoCount), (undo_count, redo_count))
                self.assertFalse(self.doc.HasPendingTransaction)
                self.assertEqual(signature(sketch), before)
                self.assertEqual(len(broken_references(sketch)), 1)
                self.assertTrue(source.Visibility)
                if action != "edit":
                    self.assertEqual([
                        (item.ObjectName, tuple(item.SubElementNames))
                        for item in Gui.Selection.getSelectionEx()
                    ], original_selection)
        finally:
            App.removeDocumentObserver(history_observer)
            release.set()
            if not session.closed:
                session.close()
            root.unref()

    def test_pending_pick_cancel_button_restores_scene_selection_and_undo(self):
        self.assert_pending_pick_cancelled("button")

    def test_pending_pick_escape_restores_scene_selection_and_undo(self):
        self.assert_pending_pick_cancelled("escape")

    def test_pending_pick_close_fences_submission(self):
        self.assert_pending_pick_cancelled("close", "submission")

    def test_pending_pick_close_fences_mutation(self):
        self.assert_pending_pick_cancelled("close", "mutation")

    def test_pending_pick_close_fences_commit(self):
        self.assert_pending_pick_cancelled("close", "commit")

    def make_solver_update_fixture(self):
        Gui.activateWorkbench("SketcherWorkbench")
        self.doc = App.newDocument("QueuedSolverUpdate")
        sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        sketch.addGeometry(Part.LineSegment(App.Vector(), App.Vector(10, 0, 0)), False)
        self.recompute_gui_document()
        self.assertTrue(Gui.activeDocument().setEdit(sketch.Name))
        self.flush_gui(100)
        return sketch

    def watch_solver_redraws(self):
        from pivy import coin

        search = coin.SoSearchAction()
        search.setName("EditingRoot")
        search.setSearchingAll(True)
        search.apply(Gui.activeDocument().activeView().getSceneGraph())
        self.assertIsNotNone(search.getPath())
        edit_root = search.getPath().getTail()
        # An admitted initial draw may still be deferred. Traverse children
        # directly because an empty switchboard hides its disabled layers even
        # from SoSearchAction, while the coordinate nodes already exist.
        def find_coordinates(node):
            if str(node.getName()) == "PointsCoordinate0":
                return node
            if hasattr(node, "getNumChildren"):
                for index in range(node.getNumChildren()):
                    found = find_coordinates(node.getChild(index))
                    if found is not None:
                        return found
            return None

        coordinates = find_coordinates(edit_root)
        self.assertIsNotNone(coordinates)
        redraws = []
        sensor = coin.SoFieldSensor(lambda *_: redraws.append(True), None)
        sensor.setPriority(0)
        sensor.attach(coordinates.point)
        self.addCleanup(sensor.detach)
        return redraws, sensor

    def test_field_sensor_observes_solver_redraw_delivery(self):
        import Sketcher
        from pivy import coin

        sketch = self.make_solver_update_fixture()
        sketch.addConstraint(Sketcher.Constraint("Distance", 0, 10.0))
        self.recompute_gui_document()
        # Settle fixture work before attaching the diagnostic. Draw admission
        # can still defer a solve notification, so test notification timing
        # separately from the eventual solver redraw.
        from Test.GuiRecompute import wait_for_mutation_ready

        wait_for_mutation_ready(self.doc)
        _redraws, watcher = self.watch_solver_redraws()
        field = watcher.getAttachedField()
        watcher.detach()
        inside, calls = [False], []

        def on_draw(*_args):
            sensor.detach()  # One shot, before any callback work.
            calls.append(inside[0])

        sensor = coin.SoFieldSensor(on_draw, None)
        sensor.setPriority(0)
        sensor.attach(field)
        self.addCleanup(sensor.detach)
        inside[0] = True
        try:
            field.touch()
        finally:
            inside[0] = False
        if not calls:
            self.skipTest("inline field notification unavailable for this live coordinate field")
        self.assertEqual(calls, [True], "priority-zero field notification was not inline")

        calls.clear()
        sensor.attach(field)
        inside[0] = True
        try:
            sketch.solve()
        finally:
            inside[0] = False
        self.assertTrue(self.wait_until(lambda: bool(calls), 2000),
                        "solver notification lost its final redraw")
        self.assertEqual(len(calls), 1, "one-shot sensor ran more than once")
        App.Console.PrintMessage(
            "Solver redraw diagnostic: inline=%d deferred=%d\n" % (calls[0], not calls[0])
        )

    def test_element_list_coalesces_nested_geometry_change(self):
        sketch = self.make_solver_update_fixture()
        lists = Gui.getMainWindow().findChildren(QtGui.QListWidget, "listWidgetElements")
        self.assertEqual(len(lists), 1)
        elements = lists[0]
        self.assertEqual(elements.count(), sketch.GeometryCount)
        self.assertFalse(sketch.getConstruction(0))
        events, calls, nested, errors = [], [], [], []
        inside = [False]

        def on_rows(*_args):
            events.append(inside[0])
            if inside[0]:
                nested.append(True)
                return
            if calls:
                return
            calls.append(True)
            inside[0] = True
            try:
                sketch.toggleConstruction(0)
            except BaseException as error:
                errors.append(error)
            finally:
                inside[0] = False

        model = elements.model()
        model.rowsInserted.connect(on_rows)
        try:
            sketch.toggleConstruction(0)
            self.assertTrue(self.wait_until(lambda: len(events) >= 2, 2000),
                            "deferred element-list update was lost")
            self.flush_gui(100)
        finally:
            model.rowsInserted.disconnect(on_rows)
        self.assertFalse(errors, errors)
        self.assertEqual(calls, [True])
        self.assertFalse(nested, "element-list update reentered its row callback")
        self.assertEqual(elements.count(), sketch.GeometryCount)
        self.assertFalse(sketch.getConstruction(0))

    def test_queued_redraw_coalesces_inline_solver_notification(self):
        from pivy import coin

        import Sketcher

        sketch = self.make_solver_update_fixture()
        sketch.addConstraint(Sketcher.Constraint("Distance", 0, 10.0))
        self.flush_gui(100)
        redraws, watcher = self.watch_solver_redraws()
        field = watcher.getAttachedField()
        calls, nested = [], []

        def on_draw(*_args):
            sensor.detach()
            calls.append(True)
            before = len(redraws)
            # Request another solve before explicitly replacing model inputs.
            # Only exercise replacement once the nonrecursive control holds.
            sketch.solve()
            nested.append(len(redraws) - before)
            if nested[-1] == 0:
                # Replace constraints without changing geometry counts, then
                # replace geometry. The outer pass must keep its captured pair.
                sketch.renameConstraint(0, "OwnedRedraw")
                sketch.toggleConstruction(0)
                sketch.solve()

        sensor = coin.SoFieldSensor(on_draw, None)
        sensor.setPriority(0)
        sensor.attach(field)
        self.addCleanup(sensor.detach)

        def solve():
            sketch.solve()

        result = self.doc.commitCompatibilityMutationAsync(
            solve, object_name=sketch.Name, recompute=False,
        )
        results, errors = [], []

        def wait():
            try:
                results.append(result["wait"](10))
            except BaseException as error:
                errors.append(error)

        waiter = threading.Thread(target=wait, daemon=True)
        waiter.start()
        try:
            self.assertTrue(self.wait_until(lambda: not waiter.is_alive(), 10000))
            self.assertTrue(self.wait_until(lambda: bool(calls), 2000))
            self.flush_gui(100)
        finally:
            waiter.join(1)
        self.assertFalse(errors, errors)
        self.assertEqual([item["status"] for item in results], ["Committed"])
        self.assertEqual(calls, [True])
        self.assertEqual(nested, [0], "inline solver notification recursively redrew")
        self.assertGreaterEqual(len(redraws), 2, "deferred final redraw was lost")
        self.assertTrue(sketch.getConstruction(0))
        self.assertEqual(sketch.Constraints[0].Name, "OwnedRedraw")
        # Constraint-only replacement must also refresh without changing counts.
        redraws.clear()
        sketch.renameConstraint(0, "ConstraintOnly")
        sketch.solve()
        self.flush_gui(100)
        self.assertTrue(redraws)
        self.assertEqual(sketch.Constraints[0].Name, "ConstraintOnly")

    def test_queued_solver_update_does_not_redraw_restarted_edit(self):
        sketch = self.make_solver_update_fixture()
        # Bracket just the owner-lane solve with posted markers. Only its solver
        # MetaCall may restart the edit; unrelated earlier events cannot.
        application = QtCore.QCoreApplication.instance()
        begin = QtCore.QEvent.Type(QtCore.QEvent.registerEventType())
        end = QtCore.QEvent.Type(QtCore.QEvent.registerEventType())
        test = self
        watches, errors = [], []

        class RestartEdit(QtCore.QObject):
            armed = False
            calls = 0

            def eventFilter(self, receiver, event):
                if receiver != application:
                    return False
                if event.type() == begin:
                    self.armed = True
                elif event.type() == end:
                    self.armed = False
                elif self.armed and event.type() == QtCore.QEvent.MetaCall:
                    self.calls += 1
                    if self.calls == 1:
                        # Suspend this specific solver callback before it arms
                        # its timer: resetEdit can itself pump queued events.
                        try:
                            Gui.activeDocument().resetEdit()
                            if not Gui.activeDocument().setEdit(sketch.Name):
                                raise RuntimeError("could not restart sketch edit")
                            redraws, sensor = test.watch_solver_redraws()
                            watches.append((redraws, sensor,
                                            sensor.getAttachedField().getNum() == 0))
                        except BaseException as error:
                            errors.append(error)
                return False

        restart = RestartEdit()

        def solve():
            QtCore.QCoreApplication.postEvent(application, QtCore.QEvent(begin))
            sketch.solve()
            QtCore.QCoreApplication.postEvent(application, QtCore.QEvent(end))

        result = self.doc.commitCompatibilityMutationAsync(
            solve, object_name=sketch.Name, recompute=False,
        )
        results = []

        def wait():
            try:
                results.append(result["wait"](10))
            except BaseException as error:
                errors.append(error)

        waiter = threading.Thread(target=wait, daemon=True)
        application.installEventFilter(restart)
        waiter.start()
        try:
            self.wait_until(lambda: not waiter.is_alive(), 10000)
            self.flush_gui(100)
        finally:
            try:
                self.wait_until(lambda: not waiter.is_alive(), 10000)
            finally:
                waiter.join(1)
                application.removeEventFilter(restart)
        self.assertFalse(waiter.is_alive(), "solver commit worker did not finish")
        self.assertFalse(errors, errors)
        self.assertEqual([item["status"] for item in results], ["Committed"])
        self.assertEqual(restart.calls, 1, "expected exactly the solver's queued callback")
        self.assertEqual(len(watches), 1)
        initial_redraws, sensor, initial_deferred = watches[0]
        if initial_deferred:
            self.assertTrue(initial_redraws, "new edit lost its deferred initial redraw")
            self.assertGreater(sensor.getAttachedField().getNum(), 0)
        else:
            self.assertFalse(initial_redraws, "old solver update redrew the new edit")
        # Idle presentation catch-up can replace the coordinate nodes. Watch
        # the current nodes for the positive controls after draining it.
        sensor.detach()
        redraws, sensor = self.watch_solver_redraws()
        # Fresh GUI-thread and queued signals must still update this edit.
        sketch.solve()
        self.flush_gui(100)
        self.assertTrue(redraws, "current edit stopped receiving solver updates")
        if initial_deferred:
            self.assertEqual(len(initial_redraws), len(redraws),
                             "old solver update added a redraw beyond initial setup")
        redraws.clear()
        sketch.touch()
        self.doc.recompute()
        self.flush_gui(100)
        self.assertTrue(redraws, "current edit stopped receiving queued solver updates")
        sensor.detach()

    def test_queued_solver_update_does_not_draw_during_commit(self):
        sketch = self.make_solver_update_fixture()
        redraws, sensor = self.watch_solver_redraws()
        entered, release = threading.Event(), threading.Event()

        def mutate():
            sketch.toggleConstruction(0)
            entered.set()
            if not release.wait(10):
                raise RuntimeError("test solver update commit timed out")

        result = self.doc.commitCompatibilityMutationAsync(
            mutate, object_name=sketch.Name, recompute=False,
        )
        results, errors = [], []

        def wait():
            try:
                results.append(result["wait"](10))
            except BaseException as error:
                errors.append(error)

        waiter = threading.Thread(target=wait, daemon=True)
        waiter.start()
        try:
            self.assertTrue(entered.wait(5))
            self.flush_gui(100)
            self.assertFalse(redraws, "queued solver update drew during a model commit")
        finally:
            release.set()
            # Do not leave owner work alive if a redraw assertion fails.
            try:
                self.wait_until(lambda: not waiter.is_alive(), 10000)
            finally:
                waiter.join(1)
        self.assertFalse(waiter.is_alive(), "commit worker did not finish")
        self.assertFalse(errors, errors)
        self.assertEqual([item["status"] for item in results], ["Committed"])
        self.assertTrue(self.wait_until(lambda: bool(redraws), 2000),
                        "solver redraw was lost while the document was busy")
        sensor.detach()
        self.recompute_gui_document()

    def test_pending_pick_edit_end_restores_scene_without_undo(self):
        self.assert_pending_pick_cancelled("edit")

    def test_pending_pick_document_close_restores_scene_and_selection(self):
        self.assert_pending_pick_cancelled("document")


    def test_viewport_pick_moved_resized_edge_preserves_settings_and_undo(self):
        import SketchReferenceReassignGui as pick_gui
        from SketchReferenceRepair import broken_references
        from SketcherTests.TestSketchReferenceRepair import signature

        replacement = Part.makeLine(App.Vector(12, 20, 0), App.Vector(35, 20, 0))
        sketch, _source, view, before = self.make_pick_fixture(replacement=replacement)
        undo_count = self.doc.UndoCount
        Gui.runCommand("Sketcher_ReassignBrokenReference")
        session = pick_gui.command.session
        try:
            viewport = view.graphicsView().viewport()
            point = self.viewport_to_qpoint(
                view, viewport, view.getPointOnScreen(App.Vector(20, 20, 0))
            )
            self.click(viewport, point)
            self.assertTrue(self.wait_until(lambda: not session.busy, 10000))
            self.assertTrue(session.closed, session.message.text() if not session.closed else "")
            self.assertFalse(broken_references(sketch))
            self.assertEqual(signature(sketch)[:3], before[:3])
            self.assertLess((sketch.getPoint(0, 1) - App.Vector(12, 20, 0)).Length, 1e-7)
            self.assertEqual(self.doc.UndoCount, undo_count + 1)
            self.undo_gui_document()
            self.recompute_gui_document()
            self.assertEqual(signature(sketch), before)
            self.assertEqual(len(broken_references(sketch)), 1)
        finally:
            if not session.closed:
                session.close()
