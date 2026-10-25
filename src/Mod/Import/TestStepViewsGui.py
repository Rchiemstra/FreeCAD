# SPDX-License-Identifier: LGPL-2.1-or-later
"""Explicit saved-view activation, with independently authored STEP expectations."""
import math
import os
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch
import xml.etree.ElementTree as ET
import zipfile

import FreeCAD as App
import FreeCADGui as Gui
import ImportGui
from pivy import coin
from Test.GuiRecompute import (close_document, recompute_document, save_document, save_document_as,
                               undo_document, redo_document)

DATA = Path(__file__).with_name("TestData") / "StepViews"


class TestStepViewsGui(unittest.TestCase):
    def setUp(self):
        import StepViewsGui
        self.api = StepViewsGui
        self.existing = set(App.listDocuments())
        self.temp = tempfile.TemporaryDirectory(prefix="step-view-activation-")

    def tearDown(self):
        Gui.Selection.clearSelection()
        for name in set(App.listDocuments()) - self.existing:
            close_document(App.getDocument(name))
        Gui.updateGui()
        self.temp.cleanup()

    def load(self, unit="mm", text=None):
        doc = App.newDocument("ActivateViews")
        view = Gui.getDocument(doc.Name).activeView()
        before = self.camera(view, depth=False)
        path = DATA / ("views-centered-" + unit + ".step")
        if text is not None:
            path = Path(self.temp.name, "variant.step")
            path.write_text(text)
        ImportGui.insert(str(path), doc.Name, merge=False, useLinkGroup=True,
                         savedViews=True, semanticPMI=True)
        recompute_document(doc)
        self.assertEqual(self.camera(view, depth=False), before, "import activated a camera")
        records = [o for o in doc.StepViews.Group if o.RecordType == "SavedView"]
        return doc, view, records

    def vector(self, actual, expected):
        self.assertEqual(len(actual), len(expected))
        for a, b in zip(actual, expected):
            self.assertAlmostEqual(a, b, delta=max(1e-5, abs(b) * 1e-6))

    def camera(self, view, depth=True):
        return self.camera_node(view.getCameraNode(), depth)

    def camera_node(self, node, depth=True):
        kind = "Orthographic" if node.isOfType(coin.SoOrthographicCamera.getClassTypeId()) else "Perspective"
        fields = ("position", "orientation", "viewportMapping", "aspectRatio",
                  "nearDistance", "farDistance", "focalDistance",
                  "height" if kind == "Orthographic" else "heightAngle")
        values = []
        for field in fields:
            if not depth and field in ("nearDistance", "farDistance"):
                continue
            value = getattr(node, field).getValue()
            values.append(value.getValue() if hasattr(value, "getValue") else value)
        return kind, tuple(values)

    def saved_camera(self, path):
        with zipfile.ZipFile(path) as archive:
            xml = ET.fromstring(archive.read("GuiDocument.xml"))
        text = xml.find("Camera").attrib["settings"]
        source = coin.SoInput()
        source.setBuffer("#Inventor V2.1 ascii\n" + text)
        scene = coin.SoDB.readAll(source)
        self.assertEqual(scene.getNumChildren(), 1)
        return text, self.camera_node(scene.getChild(0), depth=False)

    def children(self, root):
        return [root.getChild(i) for i in range(root.getNumChildren())]

    def clips(self, view):
        action = coin.SoSearchAction()
        action.setType(coin.SoClipPlane.getClassTypeId())
        action.setInterest(coin.SoSearchAction.ALL)
        action.apply(view.getSceneGraph())
        return [p.getTail() for p in action.getPaths()]

    def good(self, record, view):
        self.assertTrue(self.api.activate(record, view), "activation did not apply the saved view")

    def close_view(self, view):
        from PySide import QtCore, QtWidgets
        widget = view.graphicsView()
        while widget and not isinstance(widget, QtWidgets.QMdiSubWindow):
            widget = widget.parentWidget()
        self.assertIsNotNone(widget)
        widget.close()
        QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
        Gui.updateGui()

    def test_live_widget_survives_earlier_deferred_cleanup(self):
        # Native addresses can be reused before queued wrapper cleanup runs.
        # A previous view's cleanup must never invalidate a new live widget.
        for repeat in range(64):
            doc = App.newDocument("WidgetLifetime")
            view = Gui.getDocument(doc.Name).activeView()
            widget = view.graphicsView()
            Gui.updateGui()
            with self.subTest(repeat=repeat):
                try:
                    marker = widget.property("_FreeCAD_RendererUnavailable")
                except RuntimeError:
                    self.fail("earlier cleanup invalidated a live graphics widget")
                self.assertFalse(marker)
            close_document(doc)

    def test_ordered_destroyed_callback_deletes_record_or_plane(self):
        other, other_view, other_records = self.load()
        self.good(other_records[0], other_view)
        other_state = self.api._states[id(other_view)]
        # Closing real widgets pumps rendering, which owns automatic near/far.
        other_snapshot = self.camera(other_view, depth=False), self.children(other_view.getSceneGraph())
        for plane in (False, True):
            for repeat in range(2):
                with self.subTest(plane=plane, repeat=repeat):
                    doc, survivor, records = self.load()
                    view = Gui.getDocument(doc.Name).createView("Gui::View3DInventor")
                    baseline = self.camera(survivor)
                    obj = records[0].Planes[0] if plane else records[0]
                    root = view.getSceneGraph()
                    callbacks, unavailable_access = [], []
                    # Connect BEFORE activation. QObject::destroyed runs after the
                    # Quarter render manager has gone, regardless of callback order.
                    def earlier(*_):
                        callbacks.append("earlier")
                        doc.removeObject(obj.Name)
                    view.graphicsView().destroyed.connect(earlier)
                    self.good(records[0], view)
                    self.good(records[1], survivor)
                    write = self.api._write_camera
                    def guard(target, camera):
                        if target is view:
                            unavailable_access.append("camera on destroyed viewer")
                            return  # Fail by assertion, without touching the unavailable renderer.
                        write(target, camera)
                    with patch.object(self.api, "_write_camera", side_effect=guard):
                        self.close_view(view)
                    self.assertEqual(callbacks, ["earlier"])
                    self.assertEqual(unavailable_access, [], "cleanup accessed an unavailable viewer")
                    self.assertNotIn(id(view), self.api._states)
                    self.assertFalse(any(n.getName() == "StepViewsActivation"
                                         for n in self.children(root)))
                    self.api.clear(survivor)
                    self.api.clear(survivor)
                    self.assertEqual(self.camera(survivor), baseline)
                    self.assertIs(self.api._states[id(other_view)], other_state)
                    self.assertEqual((self.camera(other_view, depth=False),
                                      self.children(other_view.getSceneGraph())), other_snapshot)
                    close_document(doc)

    def test_clear_failure_is_atomic(self):
        _, view, records = self.load()
        _, other, other_records = self.load()
        self.good(other_records[1], other)
        other_snapshot = self.camera(other), self.children(other.getSceneGraph())
        root = view.getSceneGraph()
        external = coin.SoClipPlane()
        root.insertChild(external, 0)
        baseline = self.camera(view), self.children(root)
        for boundary in ("camera", "nodes"):
            with self.subTest(boundary=boundary):
                self.good(records[1], view)  # Different camera type from the baseline.
                snapshot = self.camera(view), self.children(root)
                state = self.api._states[id(view)]
                original_state = dict(state)
                if boundary == "camera":
                    write = self.api._write_camera
                    calls = []
                    def fail_camera(*args):
                        write(*args)
                        calls.append(True)
                        if len(calls) == 1:
                            raise RuntimeError("injected after camera mutation")
                    context = patch.object(self.api, "_write_camera", side_effect=fail_camera)
                else:
                    class Root:
                        failed = False
                        def __getattr__(self, name):
                            return getattr(root, name)
                        def removeChild(self, node):
                            root.removeChild(node)
                            if not self.failed:
                                self.failed = True
                                raise RuntimeError("injected after node removal")
                    context = patch.dict(state, root=Root())
                with context:
                    with self.assertRaisesRegex(RuntimeError, "injected after"):
                        self.api.clear(view)
                self.assertEqual((self.camera(view), self.children(root)), snapshot,
                                 "clear did not roll back exact camera and node order")
                self.assertIs(self.api._states.get(id(view)), state)
                self.assertEqual(state, original_state)
                self.assertEqual((self.camera(other), self.children(other.getSceneGraph())),
                                 other_snapshot)
                self.good(records[0], view)  # A replacement after a failed clear.
                self.api.clear(view)
                self.api.clear(view)
                self.assertEqual((self.camera(view), self.children(root)), baseline)
                self.good(records[1], view)
                self.api.clear(view)  # Retry without replacement, too.
                self.assertEqual((self.camera(view), self.children(root)), baseline)

    def test_save_active_serializes_baseline_without_changing_presentation(self):
        preferences = App.ParamGet("User parameter:BaseApp/Preferences/View")
        settings = preferences.GetContents()
        for kind in ("Orthographic", "Perspective"):
            for operation in ("save", "save-as", "failed-save", "failed-save-as", "non-active-view"):
                with self.subTest(kind=kind, operation=operation):
                    doc, view, records = self.load(text=(DATA / "views-centered-mm.step").read_text())
                    os.remove(os.path.join(self.temp.name, "variant.step"))
                    view.setCameraType(kind)
                    Gui.updateGui()  # Settle queued camera-type actions before authoring the baseline.
                    node = view.getCameraNode()
                    node.position = (101, 202, 303)
                    node.orientation = (0, 0, 0, 1)
                    node.focalDistance = 32
                    node.aspectRatio = 1.25
                    node.viewportMapping = coin.SoCamera.CROP_VIEWPORT_NO_FRAME
                    if kind == "Orthographic":
                        node.height = 42
                    else:
                        node.heightAngle = 0.5
                    path = os.path.join(self.temp.name, doc.Name + ".FCStd")
                    save_document_as(doc, path)
                    saved_text, saved_before = self.saved_camera(path)
                    baseline = self.camera(view, depth=False)
                    self.assertFalse(Gui.getDocument(doc.Name).Modified)
                    # A canonical save otherwise legitimately skips an unchanged file.
                    doc.Comment = "Force serialization independently of transient activation"
                    records_before = [(o.Name, o.dumpContent()) for o in doc.Objects]
                    second = Gui.getDocument(doc.Name).createView("Gui::View3DInventor")
                    second_before = self.camera(second, depth=False)
                    # The document's active view is second. Activate it and replace;
                    # the original view must retain its own unrelated baseline.
                    baseline_text = view.getCamera()
                    self.good(records[0], view)
                    self.good(records[1], view)
                    self.good(records[0], second)
                    self.api.clear(second)
                    self.assertEqual(self.camera(second, depth=False), second_before)
                    expected_saved = saved_before if operation.startswith("failed") else baseline
                    if operation == "non-active-view":
                        Gui.updateGui()
                        second_before = self.camera(second, depth=False)
                        expected_saved = second_before
                    else:
                        self.close_view(second)
                    self.good(records[0], view)
                    Gui.updateGui()
                    presentation = self.camera(view, depth=False), self.children(view.getSceneGraph())
                    state = self.api._states[id(view)]
                    other, other_view, _ = self.load()  # Save a non-active document.
                    other_before = self.camera(other_view, depth=False)
                    modified_before = Gui.getDocument(doc.Name).Modified
                    expected_text = (saved_text if operation.startswith("failed") else
                                     second.getCamera() if operation == "non-active-view" else baseline_text)
                    if operation in ("save", "non-active-view"):
                        save_document(doc)
                    elif operation == "save-as":
                        path = os.path.join(self.temp.name, doc.Name + "-as.FCStd")
                        save_document_as(doc, path)
                    elif operation == "failed-save":
                        # A directory at the canonical file path refuses final replacement.
                        backup = path + ".original"
                        os.rename(path, backup)
                        os.mkdir(path)
                        try:
                            with self.assertRaises(RuntimeError):
                                save_document(doc)
                        finally:
                            os.rmdir(path)
                            os.rename(backup, path)
                    else:
                        blocked = os.path.join(self.temp.name, "existing.FCStd")
                        Path(blocked).write_text("preserve this destination")
                        with self.assertRaises(RuntimeError):
                            save_document_as(doc, blocked, overwrite=False)
                        self.assertEqual(Path(blocked).read_text(), "preserve this destination")
                    self.assertEqual((self.camera(view, depth=False),
                                      self.children(view.getSceneGraph())), presentation)
                    self.assertIs(self.api._states[id(view)], state)
                    self.assertEqual(self.camera(other_view, depth=False), other_before)
                    self.assertEqual(Gui.getDocument(doc.Name).Modified,
                                     modified_before if operation.startswith("failed") else False)
                    self.assertEqual([(o.Name, o.dumpContent()) for o in doc.Objects], records_before)
                    self.assertEqual(preferences.GetContents(), settings)
                    serialized, stored_camera = self.saved_camera(path)
                    # Compare the complete serialized node, including near/far;
                    # rendering can adjust those fields after native restoration.
                    def tokens(text):
                        return " ".join(line for line in text.splitlines()
                                        if not line.lstrip().startswith("#")).split()
                    if operation == "non-active-view":
                        # The unactivated active view keeps ordinary automatic depth.
                        self.assertEqual(stored_camera, expected_saved)
                    else:
                        self.assertEqual(tokens(serialized), tokens(expected_text))
                    self.api.clear(view)
                    self.assertEqual(self.camera(view, depth=False), baseline)
                    close_document(doc)
                    reopened = App.openDocument(path)
                    target = Gui.getDocument(reopened.Name).activeView()
                    self.assertFalse(self.clips(target))
                    self.assertNotIn(id(target), self.api._states)
                    actual = self.camera(target, depth=False)
                    self.assertEqual(actual, expected_saved,
                                     "save serialized the temporary activation camera")
                    close_document(reopened)
                    close_document(other)

    def test_perspective_serialization_keeps_all_camera_fields(self):
        doc, view, _ = self.load()
        view.setCameraType("Perspective")
        node = view.getCameraNode()
        node.heightAngle = 0.5
        node.aspectRatio = 1.25
        node.viewportMapping = coin.SoCamera.CROP_VIEWPORT_NO_FRAME
        before = self.camera(view)
        text = view.getCamera()
        second = Gui.getDocument(doc.Name).createView("Gui::View3DInventor")
        second.setCamera(text)
        self.assertEqual(self.camera(second), before, "perspective restore lost camera fields")

    def test_centered_retained_region_and_both_eye_sides(self):
        for unit, scale in (("mm", 1), ("inch", 25.4), ("metre", 1000)):
            for opposite in (False, True):
                text = (DATA / ("views-centered-" + unit + ".step")).read_text()
                if opposite:
                    text = text.replace("#302=CARTESIAN_POINT('',(0.,0.,0.));",
                                        "#302=CARTESIAN_POINT('',(0.,0.,100.));")
                    text = text.replace("4.,2.,.T.,80.,.T.", "4.,80.,.T.,2.,.T.")
                    text = text.replace("4.,2.,.F.,80.,.T.", "4.,80.,.T.,2.,.T.")
                else:
                    text = text.replace("4.,2.,.F.,80.,.T.", "4.,2.,.T.,80.,.T.")
                doc, view, records = self.load(unit, text=text)
                for record in records:
                    with self.subTest(unit=unit, opposite=opposite, projection=record.Projection):
                        self.assertEqual(record.PresentationStatus, "data available", record.UnavailableReason)
                        self.good(record, view)
                        clips = self.clips(view)
                        self.assertEqual(len(clips), 4)
                        def retained(point):
                            point = coin.SbVec3f(*(x*scale for x in point))
                            return all(c.plane.getValue().isInHalfSpace(point) for c in clips)
                        self.assertTrue(retained((50, 20, 30)), "fixture has no expected retained region")
                        for point in ((4, 20, 30), (101, 20, 30), (11, 20, 30), (91, 20, 30)):
                            self.assertFalse(retained(point), point)
                        self.api.clear(view)
                close_document(doc)

    def test_camera_units_orientation_projection_and_planes(self):
        for unit, scale in (("mm", 1), ("inch", 25.4), ("metre", 1000)):
            doc, view, records = self.load(unit)
            for i, record in enumerate(records):
                with self.subTest(unit=unit, projection=i):
                    self.good(record, view)
                    node = view.getCameraNode()
                    self.vector(node.position.getValue().getValue(), (10*scale, 20*scale, 30*scale))
                    rotation = node.orientation.getValue()
                    # VRS X=world Y, Y=world Z, Z=world X; the eye looks along +VRS Z.
                    for local, world in (((0, 0, -1), (1, 0, 0)),
                                         ((0, 1, 0), (0, 0, 1)),
                                         ((1, 0, 0), (0, -1, 0))):
                        self.vector(rotation.multVec(coin.SbVec3f(*local)).getValue(), world)
                    self.assertAlmostEqual(node.focalDistance.getValue(), 4*scale, delta=scale*1e-5)
                    self.assertAlmostEqual(node.aspectRatio.getValue(), 1.5)
                    self.assertEqual(node.viewportMapping.getValue(), coin.SoCamera.CROP_VIEWPORT_NO_FRAME)
                    if i == 0:
                        self.assertEqual(view.getCameraType(), "Orthographic")
                        self.assertAlmostEqual(node.height.getValue(), 12*scale, delta=scale*1e-5)
                    else:
                        self.assertEqual(view.getCameraType(), "Perspective")
                        self.assertAlmostEqual(node.heightAngle.getValue(), 2*math.atan(1.5), places=6)
                    volume = node.getViewVolume(1.5)
                    self.assertAlmostEqual(volume.getWidth()/volume.getHeight(), 1.5, places=5)
                    if i == 0:
                        self.assertAlmostEqual(volume.getHeight(), 12*scale, delta=scale*1e-4)
                    # Coin planes use n.x - distance = 0 and retain n.x >= distance.
                    expected = [((1, 0, 0), 5*scale), ((-1, 0, 0), -100*scale)]
                    if i == 0:
                        expected.append(((1, 0, 0), 12*scale))  # origin.x + front=10+2
                    expected.append(((-1, 0, 0), -90*scale))  # origin.x + back=10+80
                    clips = self.clips(view)
                    self.assertEqual(len(clips), len(expected))
                    for clip, (normal, distance) in zip(clips, expected):
                        plane = clip.plane.getValue()
                        self.vector(plane.getNormal().getValue(), normal)
                        self.assertAlmostEqual(plane.getDistanceFromOrigin(), distance, delta=scale*1e-4)
                        self.assertTrue(plane.isInHalfSpace(coin.SbVec3f(*[
                            normal[j]*(distance+scale) for j in range(3)])))
                        self.assertFalse(plane.isInHalfSpace(coin.SbVec3f(*[
                            normal[j]*(distance-scale) for j in range(3)])))
                    self.api.clear(view)

    def test_requested_view_only_two_documents_replace_clear(self):
        doc, first, records = self.load()
        second = Gui.getDocument(doc.Name).createView("Gui::View3DInventor")
        other, other_view, _ = self.load()
        active = (App.ActiveDocument, Gui.activeDocument().activeView())
        snapshots = [self.camera(v) for v in (first, second, other_view)]
        self.good(records[0], first)
        self.good(records[1], first)
        self.good(records[1], first)
        self.assertEqual(len(self.clips(first)), 3)
        self.assertEqual(self.camera(second), snapshots[1])
        self.assertEqual(self.camera(other_view), snapshots[2])
        self.assertEqual((App.ActiveDocument, Gui.activeDocument().activeView()), active)
        with self.assertRaises(ValueError):
            self.api.activate(records[0], other_view)
        self.good(records[0], second)
        self.api.clear(first)
        self.api.clear(first)
        self.assertEqual(self.camera(first), snapshots[0])
        self.assertEqual(len(self.clips(second)), 4)
        self.api.clear(second)
        self.assertEqual(self.camera(second), snapshots[1])

    def test_projection_on_opposite_side_of_view_plane(self):
        text = (DATA / "views-centered-mm.step").read_text().replace(
            "#302=CARTESIAN_POINT('',(0.,0.,0.));",
            "#302=CARTESIAN_POINT('',(0.,0.,100.));").replace(
                "4.,2.,.T.,80.,.T.,.T.", "4.,2.,.F.,80.,.F.,.T.").replace(
                    "4.,2.,.F.,80.,.T.,.F.", "4.,2.,.F.,80.,.F.,.F.")
        _, view, records = self.load(text=text)
        for i, record in enumerate(records):
            self.good(record, view)
            node = view.getCameraNode()
            self.vector(node.position.getValue().getValue(), (110, 20, 30))
            rotation = node.orientation.getValue()
            self.vector(rotation.multVec(coin.SbVec3f(0, 0, -1)).getValue(), (-1, 0, 0))
            self.vector(rotation.multVec(coin.SbVec3f(1, 0, 0)).getValue(), (0, 1, 0))
            self.assertAlmostEqual(node.focalDistance.getValue(), 96)
            if i == 1:
                self.assertAlmostEqual(node.heightAngle.getValue(), 2*math.atan(6/96), places=6)
            self.assertEqual(len(self.clips(view)), 2)

    def test_live_invalid_data_refused_unchanged(self):
        doc, view, records = self.load()
        record = records[0]
        self.good(record, view)
        before = self.camera(view), self.children(view.getSceneGraph())
        plane = record.Planes[0]
        cases = [(record, "Units", "source (unknown)"),
                 (record, "sourceLengthFactor", 0),
                 (record, "WindowSize", [0, 12]), (record, "WindowSize", [18]),
                 (record, "WindowOrigin", [-8, -6, 4]),
                 (record, "WindowRight", [0, 1, 0]),
                 (record, "ProjectionPoint", [2, 0, 0]),
                 (record, "CameraPosition", [11, 20, 30]),
                 (record, "ViewDirection", [0, 0, 0]),
                 (record, "ViewPlaneDistance", -1),
                 (record, "FrontDistance", 81),
                 (record, "BackDistance", float("nan")),
                 (record, "ClippingExpression", "(#310+#312)"),
                 (record, "ClippingExpression", "(#310*unavailable:#312)"),
                 (record, "ClippingExpression", "(#310*#999)"),
                 (record, "ClippingExpression", "#310#312"),
                 (record, "ClippingExpression", "(#310*)"),
                 (record, "Planes", [plane]),
                 (record, "UnavailableReason", "partial data"),
                 (record, "UpDirection", [1, 0, 0]),
                 (record, "RightDirection", [0, -1, 0]),
                 (record, "WindowOrigin", [-9, -6, 0]),
                 (plane, "Capping", True), (plane, "Normal", [2, 0, 0]),
                 (plane, "Offset", -6), (plane, "RetainedHalfspace", "negative"),
                 (plane, "PresentationStatus", "unavailable"),
                 (plane, "Units", "inch")]
        for obj, field, invalid in cases:
            with self.subTest(field=field, invalid=repr(invalid)):
                original = getattr(obj, field)
                try:
                    setattr(obj, field, invalid)
                    with self.assertRaises(ValueError):
                        self.api.activate(record, view)
                    self.assertEqual((self.camera(view), self.children(view.getSceneGraph())), before)
                finally:
                    setattr(obj, field, original)

    def test_unsupported_source_cases_refused(self):
        doc, view, records = self.load()
        self.good(records[0], view)
        original = (DATA / "views-mm.step").read_text()
        centered = (DATA / "views-centered-mm.step").read_text()
        variants = [original,  # asymmetric, oblique, union
                    centered.replace("#303=CARTESIAN_POINT('',(-9.,-6.,4.));",
                                     "#303=CARTESIAN_POINT('',(6.,-9.,4.));").replace(
                                         "#304=AXIS2_PLACEMENT_3D('',#303,#30,#31);",
                                         "#304=AXIS2_PLACEMENT_3D('',#303,#30,#32);"),
                    centered.replace("#301,#306,(#310,#312)", "#301,#306,(#310,#323,#312)")]
        for index, text in enumerate(variants):
            with self.subTest(variant=index):
                _, target, unavailable = self.load(text=text)
                before = self.camera(target), self.children(target.getSceneGraph())
                with self.assertRaises(ValueError):
                    self.api.activate(unavailable[0], target)
                self.assertEqual((self.camera(target), self.children(target.getSceneGraph())), before)

    def test_preexisting_clipping_and_failure_rollback(self):
        doc, view, records = self.load()
        root = view.getSceneGraph()
        external = coin.SoClipPlane()
        external.plane.setValue(coin.SbPlane(coin.SbVec3f(0, 1, 0), 7))
        root.insertChild(external, 0)
        view.toggleClippingPlane()
        before = self.camera(view), self.children(root)
        self.good(records[0], view)
        self.api.clear(view)
        self.assertEqual((self.camera(view), self.children(root)), before)
        self.assertTrue(view.hasClippingPlane())
        for existing in (False, True):
            if existing:
                self.good(records[0], view)
            snapshot = self.camera(view), self.children(root)
            for boundary in ("_apply_camera", "_install_nodes"):
                original = getattr(self.api, boundary)
                def fail(*args):
                    original(*args)
                    raise RuntimeError("injected presentation failure")
                with patch.object(self.api, boundary, side_effect=fail):
                    with self.assertRaisesRegex(RuntimeError, "injected"):
                        self.api.activate(records[1], view)
                self.assertEqual((self.camera(view), self.children(root)), snapshot)
            self.api.clear(view)
        self.assertEqual((self.camera(view), self.children(root)), before)

    def test_deleted_records_views_documents_cleanup(self):
        doc, view, records = self.load()
        before = self.camera(view)
        self.good(records[0], view)
        doc.removeObject(records[0].Name)
        Gui.updateGui()
        self.assertEqual(self.camera(view), before)
        self.assertFalse(self.clips(view))
        with self.assertRaises(ValueError):
            self.api.activate(records[0], view)
        second = Gui.getDocument(doc.Name).createView("Gui::View3DInventor")
        self.good(records[1], second)
        scene = second.getSceneGraph()
        from PySide import QtCore, QtWidgets
        widget = second.graphicsView()
        while widget and not isinstance(widget, QtWidgets.QMdiSubWindow):
            widget = widget.parentWidget()
        self.assertIsNotNone(widget)
        widget.close()
        QtCore.QCoreApplication.sendPostedEvents(None, QtCore.QEvent.DeferredDelete)
        Gui.updateGui()
        with self.assertRaises(ValueError):
            self.api.activate(records[1], second)
        with self.assertRaises(ValueError):
            self.api.clear(second)
        self.assertFalse(any(scene.getChild(i).getName() == "StepViewsActivation"
                             for i in range(scene.getNumChildren())))
        self.good(records[1], view)
        close_document(doc)
        Gui.updateGui()
        self.assertFalse(self.api._states)
        with self.assertRaises(ValueError):
            self.api.activate(records[1], view)

    def test_missing_volume_flag_refused_unchanged(self):
        doc, view, records = self.load()
        self.good(records[0], view)
        records[0].removeProperty("SidesClipping")
        before = self.camera(view), self.children(view.getSceneGraph())
        with self.assertRaises(ValueError):
            self.api.activate(records[0], view)
        self.assertEqual((self.camera(view), self.children(view.getSceneGraph())), before)

    def test_deleted_plane_releases_owned_presentation(self):
        doc, view, records = self.load()
        baseline = self.camera(view)
        self.good(records[0], view)
        doc.removeObject(records[0].Planes[0].Name)
        Gui.updateGui()
        self.assertFalse(self.clips(view))
        self.assertEqual(self.camera(view), baseline)
        with self.assertRaises(ValueError):
            self.api.activate(records[0], view)

    def test_missing_plane_capping_flag_refused_unchanged(self):
        doc, view, records = self.load()
        self.good(records[0], view)
        records[0].Planes[0].removeProperty("Capping")
        before = self.camera(view), self.children(view.getSceneGraph())
        with self.assertRaises(ValueError):
            self.api.activate(records[0], view)
        self.assertEqual((self.camera(view), self.children(view.getSceneGraph())), before)

    def test_source_free_copy_reopen_undo_and_no_document_edits(self):
        doc, view, records = self.load(text=(DATA / "views-centered-mm.step").read_text())
        os.remove(os.path.join(self.temp.name, "variant.step"))
        path = os.path.join(self.temp.name, "owned.FCStd")
        save_document_as(doc, path)
        before = [(o.Name, o.dumpContent()) for o in doc.Objects]
        preferences = App.ParamGet("User parameter:BaseApp/Preferences/View")
        settings = preferences.GetContents()
        self.assertFalse(Gui.getDocument(doc.Name).Modified)
        self.good(records[0], view)
        self.good(records[1], view)
        self.api.clear(view)
        self.assertFalse(Gui.getDocument(doc.Name).Modified)
        self.assertEqual([(o.Name, o.dumpContent()) for o in doc.Objects], before)
        self.assertEqual(preferences.GetContents(), settings)
        close_document(doc)
        doc = App.openDocument(path)
        view = Gui.getDocument(doc.Name).activeView()
        records = [o for o in doc.StepViews.Group if o.RecordType == "SavedView"]
        self.assertFalse(self.clips(view))
        self.good(records[0], view)
        dest = App.newDocument("CopiedViews")
        dest.openTransaction("copy saved views")
        dest.copyObject(doc.StepViews, True)
        dest.commitTransaction()
        target = Gui.getDocument(dest.Name).activeView()
        self.assertFalse(self.clips(target))
        undo_document(dest)
        redo_document(dest)
        self.assertFalse(self.clips(target))
        copied = [o for o in dest.StepViews.Group if o.RecordType == "SavedView"][0]
        close_document(doc)
        self.good(copied, target)
        undo_document(dest)
        self.assertFalse(self.clips(target))
        redo_document(dest)
        self.assertFalse(self.clips(target))
        copied = [o for o in dest.StepViews.Group if o.RecordType == "SavedView"][0]
        self.good(copied, target)
        partial_doc = App.newDocument("PartialViewCopy")
        partial = partial_doc.copyObject(copied, False)
        partial_target = Gui.getDocument(partial_doc.Name).activeView()
        omitted = [p.Name for p in copied.Planes]
        self.assertFalse(partial.Planes)
        for name in omitted:
            partial_doc.addObject("App::FeaturePython", name)
        with self.assertRaises(ValueError):
            self.api.activate(partial, partial_target)
        # Cross-document links are never accepted, even with matching source IDs.
        other, _, foreign = self.load()
        with self.assertRaises((ValueError, RuntimeError)):
            copied.Planes = foreign[0].Planes
            self.api.activate(copied, target)

    def test_context_action_is_explicit_and_discoverable(self):
        doc, view, records = self.load()
        self.good(records[0], view)
        self.api.clear(view)
        Gui.Selection.addSelection(records[0])
        self.assertTrue(self.api.activate_command.IsActive())
        menu = self.api.menu.modifyContextMenu("Tree")
        self.assertIn("StepViews_Activate", [item["insert"] for item in menu])
        self.assertIn("saved view", self.api.activate_command.GetResources()["MenuText"].lower())
        before = self.camera(view)
        recompute_document(doc)
        Gui.updateGui()
        self.assertEqual(self.camera(view), before)
        self.api.activate_command.Activated()
        self.assertEqual(len(self.clips(view)), 4)
        self.api.clear_command.Activated()
        self.assertEqual(self.camera(view), before)
