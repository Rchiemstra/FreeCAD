# SPDX-License-Identifier: LGPL-2.1-or-later
"""OK is a document completion boundary, including when another recompute owns the lane."""
import contextlib
import math
import threading
import time
import unittest

import FreeCAD as App
import FreeCADGui as Gui
import Part
from PySide import QtGui
from Test.GuiRecompute import (
    _left_commit, _pump_gui_until, _retry_while_lane_busy,
    close_document, recompute_document,
)


class CopyShape:
    def execute(self, obj):
        obj.Shape = obj.Source.Shape


class TestTaskFeatureCompletion(unittest.TestCase):
    def setUp(self):
        Gui.activateWorkbench("PartDesignWorkbench")
        self.docs = []
        self.doc, self.body = self.new_body("TaskCompletion")
        self.blocker = self.doc.addObject("App::FeaturePython", "LaneBlocker")
        recompute_document(self.doc)

    def new_body(self, name):
        doc = App.newDocument(name)
        self.docs.append(doc)
        doc.UndoMode = 1
        body = doc.addObject("PartDesign::Body", "Body")
        Gui.activeDocument().activeView().setActiveObject("pdbody", body)
        return doc, body

    def tearDown(self):
        for doc in reversed(self.docs):
            try:
                if doc.Name in App.listDocuments():
                    App.setActiveDocument(doc.Name)
                    Gui.getDocument(doc.Name).resetEdit()
                    if doc.HasPendingTransaction:
                        doc.abortTransaction()
                    close_document(doc)
            except ReferenceError:
                pass
        Gui.Selection.clearSelection()

    def settle(self, doc=None):
        doc = doc or self.doc
        h = _retry_while_lane_busy(doc.recomputeAsync, 30)
        _pump_gui_until(lambda: h.done() and _left_commit(doc), 30, h.status)

    @contextlib.contextmanager
    def busy(self):
        entered, release = threading.Event(), threading.Event()

        class Block:
            def execute(self, obj):
                entered.set()
                if not release.wait(20):
                    raise RuntimeError("lane blocker timed out")

        self.blocker.Proxy = Block()
        self.blocker.touch()
        h = _retry_while_lane_busy(lambda: self.doc.recomputeAsync([self.blocker]), 30)
        try:
            _pump_gui_until(entered.is_set, 5)
            yield
        finally:
            release.set()
            _pump_gui_until(lambda: h.done() and _left_commit(self.doc), 30, h.status)

    def feature(self, kind):
        if kind in ("Fillet", "Chamfer"):
            base = self.body.newObject("PartDesign::AdditiveBox", "Box")
            base.Length = base.Width = base.Height = 10
            recompute_document(self.doc)
            feature = self.body.newObject("PartDesign::" + kind, kind)
            feature.Base = (base, ["Edge1"])
            if kind == "Fillet":
                feature.Radius = 1
            else:
                feature.Size = 1
        else:
            if kind == "Pocket":
                base = self.body.newObject("PartDesign::AdditiveBox", "Box")
                base.Length = base.Width = 20
                base.Height = 4
                base.Placement.Base = App.Vector(-10, -10, -4)
            sketch = self.body.newObject("Sketcher::SketchObject", "Nominal")
            sketch.addGeometry(Part.Circle(App.Vector(), App.Vector(0, 0, 1), 5), False)
            feature = self.body.newObject("PartDesign::" + kind, kind)
            feature.Profile = sketch
            feature.Length = 2
        recompute_document(self.doc)
        copy = self.doc.addObject("Part::FeaturePython", "Downstream")
        copy.addProperty("App::PropertyLink", "Source")
        copy.Source = feature
        copy.Proxy = CopyShape()
        recompute_document(self.doc)
        Gui.Selection.clearSelection()
        self.doc.openTransaction("Edit feature")
        Gui.getDocument(self.doc.Name).setEdit(feature.Name)
        self.settle()
        return feature, copy

    def change_tolerance(self, value):
        field = Gui.getMainWindow().findChild(QtGui.QWidget, "fittingToleranceEdit")
        self.assertIsNotNone(field)
        update = Gui.getMainWindow().findChild(QtGui.QCheckBox, "checkBoxUpdateView")
        update.setChecked(False)
        editor = field.findChild(QtGui.QLineEdit)
        editor.setText(str(value) + " mm")

    def wait_closed(self):
        _pump_gui_until(lambda: not Gui.Control.activeDialog(), 8,
                        lambda: [(o.Name, o.State) for o in self.doc.Objects])

    def assert_complete(self, feature, copy):
        self.assertFalse(self.doc.HasPendingTransaction)
        self.assertFalse(self.doc.mustExecute(), [(o.Name, o.State) for o in self.doc.Objects])
        self.assertTrue(feature.Shape.isValid())
        self.assertAlmostEqual(self.body.Shape.Volume, feature.Shape.Volume)
        self.assertAlmostEqual(copy.Shape.Volume, feature.Shape.Volume)
        self.assertAlmostEqual(copy.Shape.common(feature.Shape).Volume, feature.Shape.Volume)

    def busy_ok(self, kind):
        feature, copy = self.feature(kind)
        if kind in ("Pad", "Pocket"):
            self.change_tolerance(0.4)
        with self.busy():
            Gui.Control.activeTaskDialog().accept()
            self.assertTrue(Gui.Control.activeDialog())
        self.wait_closed()
        self.assert_complete(feature, copy)
        if kind in ("Pad", "Pocket"):
            volume = math.pi * (4.8 if kind == "Pad" else 5.2)**2 * 2
            self.assertAlmostEqual(feature.Shape.Volume, volume if kind == "Pad" else 1600-volume)

    def testFilletOKWhileBusy(self):
        self.busy_ok("Fillet")

    def testChamferOKWhileBusy(self):
        self.busy_ok("Chamfer")

    def testPadOKWhileBusy(self):
        self.busy_ok("Pad")

    def testPocketOKWhileBusyCompletesDocument(self):
        self.busy_ok("Pocket")

    def testPadGUI(self):
        sketch = self.body.newObject("Sketcher::SketchObject", "Nominal")
        sketch.addGeometry(Part.Circle(App.Vector(), App.Vector(0, 0, 1), 5), False)
        recompute_document(self.doc)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(sketch)
        Gui.runCommand("PartDesign_Pad")
        self.settle()
        self.change_tolerance(0.4)
        Gui.Control.activeTaskDialog().accept()
        self.wait_closed()
        pad = self.doc.getObject("Pad")
        self.assertFalse(self.doc.mustExecute())
        self.assertAlmostEqual(pad.Shape.Volume, math.pi * 4.8**2 * pad.Length.Value)
        self.assertAlmostEqual(self.body.Shape.Volume, pad.Shape.Volume)

    def testPendingCancel(self):
        feature, copy = self.feature("Pad")
        before = feature.Shape.Volume
        self.change_tolerance(0.4)
        with self.busy():
            Gui.Control.activeTaskDialog().accept()
            Gui.Control.activeTaskDialog().reject()
        self.assertFalse(Gui.Control.activeDialog())
        self.settle()
        self.assertAlmostEqual(feature.FittingTolerance.Value, 0)
        self.assertAlmostEqual(feature.Shape.Volume, before)
        self.assertFalse(self.doc.HasPendingTransaction)

    def testDocumentCloseDuringDeferredOK(self):
        self.feature("Pad")
        with self.busy():
            Gui.Control.activeTaskDialog().accept()
            # Closing the editor is the first step of document close; it must
            # dispose its queued OK before the close can acquire the busy lane.
            Gui.getDocument(self.doc.Name).resetEdit()
        name = self.doc.Name
        close_document(self.doc)
        self.assertNotIn(name, App.listDocuments())
        self.assertFalse(Gui.Control.activeDialog())

    def testDocumentSwitchDuringDeferredOK(self):
        feature, copy = self.feature("Pad")
        other, body = self.new_body("OtherTask")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        recompute_document(other)
        other.openTransaction("Other editor")
        Gui.getDocument(other.Name).setEdit(box.Name)
        other_dialog = Gui.Control.activeTaskDialog()
        App.setActiveDocument(self.doc.Name)
        self.change_tolerance(0.4)
        with self.busy():
            Gui.Control.activeTaskDialog().accept()
            App.setActiveDocument(other.Name)
        _pump_gui_until(lambda: not self.doc.HasPendingTransaction, 8)
        self.assertIsNotNone(Gui.Control.activeTaskDialog())
        self.assertEqual(Gui.Control.activeTaskDialog(), other_dialog)
        self.assertTrue(other.HasPendingTransaction)
        self.assert_complete(feature, copy)
        App.setActiveDocument(self.doc.Name)
        self.assertFalse(Gui.Control.activeDialog())

    def testFeatureErrorOKRemainsEditable(self):
        feature, copy = self.feature("Pad")
        self.change_tolerance(11)
        with self.busy():
            Gui.Control.activeTaskDialog().accept()
        _pump_gui_until(lambda: "Invalid" in feature.State and _left_commit(self.doc), 8)
        start = time.monotonic()
        _pump_gui_until(lambda: time.monotonic() - start > 0.3, 2)
        self.assertTrue(Gui.Control.activeDialog())
        self.assertTrue(self.doc.HasPendingTransaction)
        self.change_tolerance(0.4)
        Gui.Control.activeTaskDialog().accept()
        self.wait_closed()
        self.assert_complete(feature, copy)
        self.assertAlmostEqual(feature.Shape.Volume, math.pi * 4.8**2 * 2)
