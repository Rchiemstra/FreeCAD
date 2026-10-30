# SPDX-License-Identifier: LGPL-2.1-or-later
"""Opening a saved sketch must not create document changes (issue #6290)."""

import os
import re
import tempfile
from unittest.mock import patch

import FreeCAD as App
import FreeCADGui as Gui
import Part
import Sketcher
from PySide import QtGui

from SketcherTests.GuiTestCase import SketcherGuiTestCase
from Test.GuiRecompute import (
    save_document_as, wait_for_mutation_ready, undo_document, redo_document,
)


class _RecomputeObserver:
    def __init__(self, document):
        self.document = document
        self.events = []

    def slotBeforeRecomputeDocument(self, document):
        if document == self.document:
            self.events.append("before")

    def slotRecomputedDocument(self, document):
        if document == self.document:
            self.events.append("after")

    def slotRecomputedObject(self, obj):
        if obj.Document == self.document:
            self.events.append(obj.Name)


class TestSketchEditCleanGui(SketcherGuiTestCase):
    def setUp(self):
        super().setUp()
        Gui.activateWorkbench("SketcherWorkbench")
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.doc = App.newDocument("SketchEditClean")
        self.doc.UndoMode = 1
        body = self.doc.addObject("PartDesign::Body", "Body")
        self.sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        body.addObject(self.sketch)
        self.sketch.addGeometry(Part.LineSegment(App.Vector(), App.Vector(10, 0, 0)), False)
        self.recompute_gui_document()
        self.doc.clearUndos()
        self.path = os.path.join(directory.name, "SketchEditClean.FCStd")
        save_document_as(self.doc, self.path)
        self.flush_gui(100)
        wait_for_mutation_ready(self.doc)
        self.assertFalse(Gui.activeDocument().Modified)
        self.assertFalse("Touched" in self.sketch.State)
        self.undo_count = self.doc.UndoCount
        self.observer = _RecomputeObserver(self.doc)
        App.addDocumentObserver(self.observer)
        self.addCleanup(App.removeDocumentObserver, self.observer)

    def edit_and_close(self, command, change=False, pending_recompute=False):
        self.assertTrue(Gui.activeDocument().setEdit(self.sketch.Name))
        # ZIP timestamps have two-second resolution; exercise different timestamps.
        self.flush_gui(2200)
        self.assertIsNotNone(Gui.activeDocument().getInEdit())
        if change:
            # Let the real Sketcher command own its transaction.
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(self.sketch, "Edge1")
            Gui.runCommand("Sketcher_ToggleConstruction")
            Gui.Selection.clearSelection()
            self.assertTrue(self.sketch.getConstruction(0))
        if command == "accept":
            boxes = [
                box for box in Gui.getMainWindow().findChildren(QtGui.QDialogButtonBox)
                if box.isVisible() and box.button(QtGui.QDialogButtonBox.Ok) is not None
            ]
            self.assertEqual(len(boxes), 1)
            boxes[0].button(QtGui.QDialogButtonBox.Ok).click()
        elif command == "cancel":
            Gui.runCommand("Sketcher_CancelSketch")
        elif command:
            Gui.runCommand("Sketcher_LeaveSketch")
        else:
            Gui.activeDocument().resetEdit()
        # Drain queued owner recomputes and their GUI notifications before asserting.
        self.flush_gui(200)
        if pending_recompute:
            # A preserved request intentionally makes readiness["ready"] false.
            # Wait for operations to settle without asking that work be erased.
            def settled():
                state = self.doc.getMutationReadiness()
                return not any(state[key] for key in (
                    "pending_transaction", "booked_transaction", "transaction_locked",
                    "recomputing", "pending_removal", "commit_barrier",
                    "notification_replay", "poisoned",
                ))
            self.assertTrue(self.wait_until(settled, timeout_ms=30000))
        else:
            wait_for_mutation_ready(self.doc)
        self.flush_gui(100)
        self.assertIsNone(Gui.activeDocument().getInEdit())
        self.assertFalse(Gui.Control.activeDialog())

    def assert_clean(self):
        checks = {
            "document modified": (Gui.activeDocument().Modified, False),
            "sketch touched": ("Touched" in self.sketch.State, False),
            "undo count": (self.doc.UndoCount, self.undo_count),
            "recompute events": (self.observer.events, []),
        }
        for invariant, (actual, expected) in checks.items():
            with self.subTest(invariant=invariant):
                self.assertEqual(actual, expected)

    def test_untouched_reset_edit(self):
        self.edit_and_close(command=False)
        self.assert_clean()

    def test_untouched_leave_command(self):
        self.edit_and_close(command=True)
        self.assert_clean()

    def test_changed_reset_edit(self):
        self.edit_and_close(command=False, change=True)
        self.assertTrue(Gui.activeDocument().Modified)
        self.assertEqual(self.doc.UndoCount, self.undo_count + 1)
        self.assert_edit_undo_redo()

    def test_changed_leave_command(self):
        self.edit_and_close(command=True, change=True)
        self.assertTrue(Gui.activeDocument().Modified)
        self.assertEqual(self.doc.UndoCount, self.undo_count + 1)
        self.assert_edit_undo_redo()

    def test_untouched_close_button(self):
        self.edit_and_close(command="accept")
        self.assert_clean()

    def test_changed_close_button(self):
        self.edit_and_close(command="accept", change=True)
        self.assertTrue(Gui.activeDocument().Modified)
        self.assertEqual(self.doc.UndoCount, self.undo_count + 1)
        self.assert_edit_undo_redo()

    def persistent_state(self):
        # Persistence includes all coordinates, construction flags, constraint
        # parameters and placement (rather than just list lengths).
        return (tuple(g.dumpContent() for g in self.sketch.Geometry),
                tuple(c.Content for c in self.sketch.Constraints),
                self.sketch.Placement.toMatrix().A)

    def assert_edit_undo_redo(self):
        self.assertTrue(self.sketch.getConstruction(0))
        edited = self.persistent_state()
        undo_document(self.doc)
        self.assertFalse(self.sketch.getConstruction(0))
        redo_document(self.doc)
        self.assertTrue(self.sketch.getConstruction(0))
        self.assertEqual(self.persistent_state(), edited)

    def test_cancel_restores_backup(self):
        self.sketch.addConstraint(Sketcher.Constraint("Distance", 0, 10.0))
        self.sketch.Placement = App.Placement(App.Vector(3, 4, 5), App.Rotation(0, 0, 20))
        self.recompute_gui_document()
        before = self.persistent_state()
        self.observer.events.clear()
        self.assertTrue(Gui.activeDocument().setEdit(self.sketch.Name))
        self.sketch.moveGeometry(0, 1, App.Vector(2, 3, 0), False)
        self.sketch.setDatum(0, 15.0)
        self.sketch.Placement = App.Placement(App.Vector(9, 8, 7), App.Rotation())
        self.sketch.addGeometry(Part.LineSegment(App.Vector(), App.Vector(2, 2, 0)))
        self.assertNotEqual(self.persistent_state(), before)
        Gui.runCommand("Sketcher_CancelSketch")
        self.flush_gui(200)
        wait_for_mutation_ready(self.doc)
        self.assertEqual(self.persistent_state(), before)
        self.assertEqual(self.observer.events, [])

    def test_reopened_untouched(self):
        name = self.doc.Name
        App.closeDocument(name)
        self.doc = App.openDocument(self.path)
        self.sketch = self.doc.getObject("Sketch")
        self.observer.document = self.doc
        self.observer.events.clear()
        self.undo_count = self.doc.UndoCount
        wait_for_mutation_ready(self.doc)
        self.assertFalse(Gui.activeDocument().Modified)
        self.edit_and_close(command=False)
        self.assert_clean()

    def test_preexisting_touch_survives(self):
        self.sketch.touch()
        self.assertIn("Touched", self.sketch.State)
        self.edit_and_close(command=False, pending_recompute=True)
        self.assertIn("Touched", self.sketch.State)
        self.assertTrue(self.doc.mustExecute())
        self.assertEqual(self.observer.events, [])

    def assert_geometry_touched(self):
        # Touched is status bit zero, which getPropertyStatus intentionally omits.
        match = re.search(r'<Property name="Geometry"[^>]* status="(\d+)"',
                          self.sketch.Content)
        self.assertIsNotNone(match)
        self.assertTrue(int(match.group(1)) & 1)

    def test_preexisting_geometry_touch_survives(self):
        self.sketch.Geometry = self.sketch.Geometry
        self.assert_geometry_touched()
        self.edit_and_close(command=False, pending_recompute=True)
        self.assert_geometry_touched()
        self.assertTrue(self.doc.mustExecute())
        self.assertEqual(self.observer.events, [])

    def test_touch_during_edit_survives(self):
        self.assertTrue(Gui.activeDocument().setEdit(self.sketch.Name))
        self.sketch.touch()
        Gui.activeDocument().resetEdit()
        self.flush_gui(100)
        self.assertIn("Touched", self.sketch.State)
        self.assertTrue(self.doc.mustExecute())

    def test_geometry_recompute_request_during_edit_survives(self):
        self.assertTrue(Gui.activeDocument().setEdit(self.sketch.Name))
        self.sketch.Geometry = self.sketch.Geometry
        Gui.activeDocument().resetEdit()
        self.flush_gui(100)
        self.assert_geometry_touched()
        self.assertTrue(self.doc.mustExecute())
        self.assertTrue(Gui.activeDocument().Modified)

    def test_visibility_callback_persistent_edit(self):
        sketch = self.sketch
        body = self.doc.getObject("Body")

        class Observer:
            called = False

            def slotChangedObject(observer, obj, prop):
                if obj == sketch and prop == "Visibility" and not observer.called:
                    observer.called = True
                    body.Label = "Persistent visibility callback edit"

        observer = Observer()
        App.addDocumentObserver(observer)
        try:
            self.edit_and_close(command=False)
        finally:
            App.removeDocumentObserver(observer)
        self.assertTrue(observer.called)
        self.assertEqual(body.Label, "Persistent visibility callback edit")
        self.assertTrue(Gui.activeDocument().Modified)

    def test_workbench_callback_persistent_edit(self):
        import Show
        original = Show.TempoVis.activateWorkbench
        body = self.doc.getObject("Body")

        def activate(tv, name):
            original(tv, name)
            body.Label = "Persistent workbench callback edit"

        with patch.object(Show.TempoVis, "activateWorkbench", activate):
            self.edit_and_close(command=False)
        self.assertEqual(body.Label, "Persistent workbench callback edit")
        self.assertTrue(Gui.activeDocument().Modified)

    def test_solver_callback_persistent_edit(self):
        sketch = self.sketch
        body = self.doc.getObject("Body")

        class Observer:
            called = False

            def slotChangedObject(observer, obj, prop):
                if obj == sketch and prop == "Geometry" and not observer.called:
                    observer.called = True
                    body.Label = "Persistent solver callback edit"

        observer = Observer()
        App.addDocumentObserver(observer)
        try:
            self.assertTrue(Gui.activeDocument().setEdit(self.sketch.Name))
        finally:
            App.removeDocumentObserver(observer)
        self.assertTrue(observer.called)
        self.assertTrue(Gui.activeDocument().Modified)
        Gui.activeDocument().resetEdit()

    def test_initial_solve_changes_geometry(self):
        self.sketch.addConstraint(Sketcher.Constraint("Horizontal", 0))
        self.recompute_gui_document()
        # Store deliberately unsolved geometry, as can occur in imported files.
        self.sketch.Geometry = [Part.LineSegment(App.Vector(), App.Vector(10, 2, 0))]
        save_document_as(self.doc, self.path)
        self.assertFalse(Gui.activeDocument().Modified)
        before = self.persistent_state()
        self.assertTrue(Gui.activeDocument().setEdit(self.sketch.Name))
        self.assertNotEqual(self.persistent_state(), before)
        self.assertTrue(Gui.activeDocument().Modified)
        Gui.activeDocument().resetEdit()

    def test_cross_document_tempovis(self):
        import Show
        other = App.newDocument("SketchEditOther")
        try:
            obj = other.addObject("PartDesign::Feature", "Support")
            obj.ViewObject.Visibility = False
            self.recompute_gui_document(other)
            save_document_as(other, os.path.join(os.path.dirname(self.path), "Other.FCStd"))
            App.setActiveDocument(self.doc.Name)
            original = Show.TempoVis.activateWorkbench

            def activate(tv, name):
                original(tv, name)
                tv.show(obj)

            with patch.object(Show.TempoVis, "activateWorkbench", activate):
                self.assertTrue(Gui.activeDocument().setEdit(self.sketch.Name))
                self.assertTrue(obj.ViewObject.Visibility)
                self.assertFalse(Gui.getDocument(other.Name).Modified)
                Gui.activeDocument().resetEdit()
            self.assertFalse(obj.ViewObject.Visibility)
            self.assertFalse(Gui.getDocument(other.Name).Modified)
        finally:
            Gui.getDocument(self.doc.Name).resetEdit()
            App.closeDocument(other.Name)
            App.setActiveDocument(self.doc.Name)

    def test_cross_document_body_tempovis(self):
        import Show
        other = App.newDocument("SketchEditOtherBody")
        try:
            body = other.addObject("PartDesign::Body", "Body")
            support = other.addObject("PartDesign::Feature", "Support")
            tip = other.addObject("PartDesign::Feature", "Tip")
            body.addObject(support)
            body.addObject(tip)
            support.ViewObject.Visibility = False
            tip.ViewObject.Visibility = True
            self.recompute_gui_document(other)
            save_document_as(other, os.path.join(os.path.dirname(self.path), "OtherBody.FCStd"))
            self.assertFalse(Gui.getDocument(other.Name).Modified)
            App.setActiveDocument(self.doc.Name)
            original = Show.TempoVis.activateWorkbench

            def activate(tv, name):
                original(tv, name)
                tv.show(support)

            with patch.object(Show.TempoVis, "activateWorkbench", activate):
                self.assertTrue(Gui.activeDocument().setEdit(self.sketch.Name))
                self.assertTrue(support.ViewObject.Visibility)
                self.assertFalse(tip.ViewObject.Visibility)
                self.assertFalse(Gui.getDocument(other.Name).Modified)
                Gui.activeDocument().resetEdit()
            self.assertFalse(support.ViewObject.Visibility)
            self.assertTrue(tip.ViewObject.Visibility)
            self.assertFalse(Gui.getDocument(other.Name).Modified)
        finally:
            Gui.getDocument(self.doc.Name).resetEdit()
            App.closeDocument(other.Name)
            App.setActiveDocument(self.doc.Name)

    def test_untouched_preserves_existing_document_change(self):
        self.doc.Label = "Unsaved document label"
        self.assertTrue(Gui.activeDocument().Modified)
        self.edit_and_close(command=False)
        self.assertTrue(Gui.activeDocument().Modified)
        self.assertEqual(self.doc.Label, "Unsaved document label")
        self.assertEqual(self.doc.UndoCount, self.undo_count)
        self.assertEqual(self.observer.events, [])
