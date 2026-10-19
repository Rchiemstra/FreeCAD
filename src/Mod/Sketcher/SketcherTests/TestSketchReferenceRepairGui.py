# SPDX-License-Identifier: LGPL-2.1-or-later
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

    def make_pick_fixture(self, vertex=False):
        import Sketcher
        from SketcherTests.TestSketchReferenceRepair import signature

        Gui.activateWorkbench("SketcherWorkbench")
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
        source.Shape = Part.makeCompound([wanted.copy(noElementMap=True), other])
        self.recompute_gui_document()
        source.Visibility = True
        Gui.getMainWindow().show()
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
                        seen.append(action.text())
                        action.trigger()
                        menu.close()
                        return
                menu.close()

        QtCore.QTimer.singleShot(100, choose)
        self.right_click(viewport, point)
        self.assertTrue(seen, "Broken geometry context menu has no reassign action")
        session = pick_gui.command.session
        try:
            self.assertFalse(session.closed)
            self.assertAlmostEqual(
                session.material.transparency.getValue()[0], 0.65, places=5
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
