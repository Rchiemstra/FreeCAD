# SPDX-License-Identifier: LGPL-2.1-or-later
"""Opening a saved sketch must not create document changes (issue #6290)."""

import os
import tempfile

import FreeCAD as App
import FreeCADGui as Gui
import Part
from PySide import QtGui

from SketcherTests.GuiTestCase import SketcherGuiTestCase
from Test.GuiRecompute import save_document_as, wait_for_mutation_ready


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
        save_document_as(self.doc, os.path.join(directory.name, "SketchEditClean.FCStd"))
        self.flush_gui(100)
        wait_for_mutation_ready(self.doc)
        self.assertFalse(Gui.activeDocument().Modified)
        self.assertFalse("Touched" in self.sketch.State)
        self.undo_count = self.doc.UndoCount
        self.observer = _RecomputeObserver(self.doc)
        App.addDocumentObserver(self.observer)
        self.addCleanup(App.removeDocumentObserver, self.observer)

    def edit_and_close(self, command, change=False):
        self.assertTrue(Gui.activeDocument().setEdit(self.sketch.Name))
        # ZIP timestamps have two-second resolution; exercise different timestamps.
        self.flush_gui(2200)
        self.assertIsNotNone(Gui.activeDocument().getInEdit())
        if change:
            # Match a normal Sketcher tool's transaction, with one persistent edit.
            self.doc.openTransaction("Add line")
            self.sketch.addGeometry(
                Part.LineSegment(App.Vector(10, 0, 0), App.Vector(10, 10, 0)), False
            )
            self.doc.commitTransaction()
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
        self.assertEqual(self.sketch.GeometryCount, 2)

    def test_changed_leave_command(self):
        self.edit_and_close(command=True, change=True)
        self.assertTrue(Gui.activeDocument().Modified)
        self.assertEqual(self.doc.UndoCount, self.undo_count + 1)
        self.assertEqual(self.sketch.GeometryCount, 2)

    def test_untouched_close_button(self):
        self.edit_and_close(command="accept")
        self.assert_clean()

    def test_changed_close_button(self):
        self.edit_and_close(command="accept", change=True)
        self.assertTrue(Gui.activeDocument().Modified)
        self.assertEqual(self.doc.UndoCount, self.undo_count + 1)
        self.assertEqual(self.sketch.GeometryCount, 2)

    def test_cancel_restores_backup(self):
        self.edit_and_close(command="cancel", change=True)
        self.assertEqual(self.sketch.GeometryCount, 1)
        self.assertEqual(self.observer.events, [])

    def test_untouched_preserves_existing_document_change(self):
        self.doc.Label = "Unsaved document label"
        self.assertTrue(Gui.activeDocument().Modified)
        self.edit_and_close(command=False)
        self.assertTrue(Gui.activeDocument().Modified)
        self.assertEqual(self.doc.Label, "Unsaved document label")
        self.assertEqual(self.doc.UndoCount, self.undo_count)
        self.assertEqual(self.observer.events, [])
