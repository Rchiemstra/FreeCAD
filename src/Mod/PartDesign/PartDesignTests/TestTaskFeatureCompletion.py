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
from PySide import QtCore, QtGui
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
                    Gui.Control.closeDialog(Gui.getDocument(doc.Name))
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
            yield release, h
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

    def change_length(self, value):
        field = Gui.getMainWindow().findChild(QtGui.QWidget, "lengthEdit")
        self.assertIsNotNone(field)
        update = Gui.getMainWindow().findChild(QtGui.QCheckBox, "checkBoxUpdateView")
        self.assertIsNotNone(update)
        update.setChecked(False)
        editor = field.findChild(QtGui.QLineEdit)
        self.assertIsNotNone(editor)
        editor.setFocus()
        editor.setText(str(value) + " mm")
        QtGui.QApplication.sendEvent(field, QtGui.QFocusEvent(QtCore.QEvent.FocusOut))

    def change_tolerance(self, value):
        field = Gui.getMainWindow().findChild(QtGui.QWidget, "fittingToleranceEdit")
        self.assertIsNotNone(field)
        update = Gui.getMainWindow().findChild(QtGui.QCheckBox, "checkBoxUpdateView")
        self.assertIsNotNone(update)
        update.setChecked(False)
        editor = field.findChild(QtGui.QLineEdit)
        self.assertIsNotNone(editor)
        editor.setFocus()
        editor.setText(str(value) + " mm")
        QtGui.QApplication.sendEvent(field, QtGui.QFocusEvent(QtCore.QEvent.FocusOut))

    def wait_closed(self):
        _pump_gui_until(lambda: not Gui.Control.activeDialog(), 8,
                        lambda: [(o.Name, o.State) for o in self.doc.Objects])

    def assert_complete(self, feature, copy):
        self.assertFalse(self.doc.HasPendingTransaction)
        self.assertFalse(self.doc.mustExecute(), [(o.Name, o.State) for o in self.doc.Objects])
        self.assertTrue(feature.Shape.isValid())
        self.assertAlmostEqual(self.body.Shape.Volume, feature.Shape.Volume)
        self.assertAlmostEqual(copy.Shape.Volume, feature.Shape.Volume)
        self.assertTrue(copy.Shape.isValid())
        # The consumer lives outside the Body's placement frame. Compare
        # intrinsic geometry as well as volume, independently of that frame.
        self.assertEqual(len(copy.Shape.Faces), len(feature.Shape.Faces))
        self.assertEqual(len(copy.Shape.Edges), len(feature.Shape.Edges))
        for actual, expected in zip(sorted(e.Length for e in copy.Shape.Edges),
                                    sorted(e.Length for e in feature.Shape.Edges)):
            self.assertAlmostEqual(actual, expected)

    def busy_ok(self, kind):
        feature, copy = self.feature(kind)
        if kind in ("Pad", "Pocket"):
            self.change_length(3)
            self.change_tolerance(0.4)
        with self.busy():
            Gui.Control.activeTaskDialog().accept()
            Gui.Control.activeTaskDialog().accept()
            self.assertTrue(Gui.Control.activeDialog())
        self.wait_closed()
        self.assert_complete(feature, copy)
        if kind in ("Pad", "Pocket"):
            volume = math.pi * (4.8 if kind == "Pad" else 5.2)**2 * 3
            self.assertAlmostEqual(feature.Shape.Volume, volume if kind == "Pad" else 1600-volume)

    def testPocketPreviewWhileBusy(self):
        feature, copy = self.feature("Pocket")
        self.change_length(3)
        self.change_tolerance(0.4)
        update = Gui.getMainWindow().findChild(QtGui.QCheckBox, "checkBoxUpdateView")
        with self.busy():
            update.setChecked(True)
            self.assertIn("Touched", feature.State)
        _pump_gui_until(
            lambda: _left_commit(self.doc) and "Touched" not in feature.State, 30,
            lambda: {"feature": feature.State, "status": feature.getStatusString()},
        )
        self.assertAlmostEqual(feature.Shape.Volume, 1600 - math.pi * 5.2**2 * 3)
        self.assertTrue(Gui.Control.activeDialog())
        self.assertTrue(self.doc.HasPendingTransaction)
        Gui.Control.activeTaskDialog().accept()
        self.wait_closed()
        self.assert_complete(feature, copy)

    def testPocketOKImmediatelyAfterPreview(self):
        feature, copy = self.feature("Pocket")
        self.change_length(3)
        self.change_tolerance(0.4)
        update = Gui.getMainWindow().findChild(QtGui.QCheckBox, "checkBoxUpdateView")
        update.setChecked(True)
        Gui.Control.activeTaskDialog().accept()
        self.wait_closed()
        self.assert_complete(feature, copy)
        self.assertAlmostEqual(feature.Shape.Volume, 1600 - math.pi * 5.2**2 * 3)

    def dress_up_ok(self, kind):
        feature, copy = self.feature(kind)
        Gui.Control.activeTaskDialog().accept()
        self.wait_closed()
        self.assert_complete(feature, copy)

    def testFilletOK(self):
        self.dress_up_ok("Fillet")

    def testChamferOK(self):
        self.dress_up_ok("Chamfer")

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
        self.change_length(3)
        self.change_tolerance(0.4)
        Gui.Control.activeTaskDialog().accept()
        self.wait_closed()
        pad = self.doc.getObject("Pad")
        self.assertFalse(self.doc.mustExecute())
        self.assertAlmostEqual(pad.Shape.Volume, math.pi * 4.8**2 * 3)
        self.assertAlmostEqual(self.body.Shape.Volume, pad.Shape.Volume)

    def testPendingCancel(self):
        feature, copy = self.feature("Pad")
        before = feature.Shape.Volume
        self.change_length(3)
        self.change_tolerance(0.4)
        with self.busy():
            Gui.Control.activeTaskDialog().accept()
            Gui.Control.activeTaskDialog().reject()
        self.wait_closed()
        self.settle()
        self.assertAlmostEqual(feature.Length.Value, 2)
        self.assertAlmostEqual(feature.FittingTolerance.Value, 0)
        self.assertAlmostEqual(feature.Shape.Volume, before)
        self.assertFalse(self.doc.HasPendingTransaction)

    def testDocumentCloseDuringDeferredOK(self):
        self.feature("Pad")
        with self.busy():
            Gui.Control.activeTaskDialog().accept()
            # Closing the editor is the first step of document close; it must
            # dispose its queued OK before the close can acquire the busy lane.
            Gui.Control.closeDialog(Gui.getDocument(self.doc.Name))
        name = self.doc.Name
        close_document(self.doc)
        self.assertNotIn(name, App.listDocuments())
        self.assertFalse(Gui.Control.activeDialog())

    def testDocumentSwitchDuringDeferredOK(self):
        other, body = self.new_body("OtherTask")
        box = body.newObject("PartDesign::AdditiveBox", "Box")
        recompute_document(other)
        App.setActiveDocument(self.doc.Name)
        feature, copy = self.feature("Pad")
        App.setActiveDocument(other.Name)

        class OtherPanel:
            def __init__(self):
                self.form = QtGui.QWidget()

        panel = OtherPanel()
        Gui.Control.showDialog(panel, Gui.getDocument(other.Name))
        self.assertIsNotNone(Gui.Control.activeTaskDialog(Gui.getDocument(other.Name)))
        App.setActiveDocument(self.doc.Name)
        self.change_length(3)
        self.change_tolerance(0.4)
        with self.busy() as (release, handle):
            Gui.Control.activeTaskDialog().accept()
            self.assertTrue(self.doc.HasPendingTransaction)
            release.set()
            # Let the owner leave its prepared commit before activation, but
            # do not deliver the queued OK timer until the user has switched.
            deadline = time.monotonic() + 5
            while not (handle.done() and _left_commit(self.doc)):
                if time.monotonic() > deadline:
                    self.fail("owner did not release the document activation barrier")
                time.sleep(0.001)
            App.setActiveDocument(other.Name)
            self.assertEqual(App.activeDocument(), other)
        _pump_gui_until(
            lambda: not Gui.Control.activeDialog(Gui.getDocument(self.doc.Name)), 8
        )
        self.assertIsNotNone(Gui.Control.activeTaskDialog(Gui.getDocument(other.Name)))
        self.assert_complete(feature, copy)
        App.setActiveDocument(self.doc.Name)
        self.assertFalse(Gui.Control.activeDialog())

    def testFeatureErrorOKRemainsEditable(self):
        feature, copy = self.feature("Pad")
        self.change_length(3)
        self.change_tolerance(0.4)
        # An empty profile makes an ordinary Pad fail at recompute time.
        sketch = feature.Profile[0]
        sketch.delGeometry(0)
        with self.busy():
            Gui.Control.activeTaskDialog().accept()
        _pump_gui_until(lambda: "Invalid" in feature.State and _left_commit(self.doc), 8)
        start = time.monotonic()
        _pump_gui_until(lambda: time.monotonic() - start > 0.3, 2)
        self.assertTrue(Gui.Control.activeDialog())
        self.assertTrue(self.doc.HasPendingTransaction)
        sketch.addGeometry(Part.Circle(App.Vector(), App.Vector(0, 0, 1), 5), False)
        self.change_length(3)
        self.change_tolerance(0.4)
        Gui.Control.activeTaskDialog().accept()
        self.wait_closed()
        self.assert_complete(feature, copy)
        self.assertAlmostEqual(feature.Shape.Volume, math.pi * 4.8**2 * 3)

    def testToleranceErrorOKRemainsEditable(self):
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
