# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise the public Draft factories on the document lane and the GUI thread."""

import builtins
import threading
import unittest
from types import SimpleNamespace
from unittest.mock import patch

import FreeCAD as App
import FreeCADGui as Gui
import Part
from PySide import QtCore
from Test.GuiRecompute import close_document, recompute_document, _pump_gui_until
from draftutils import gui_utils, params


class DraftOwnerThreadFactories(unittest.TestCase):
    def setUp(self):
        self.workbench = Gui.activeWorkbench().name()
        Gui.activateWorkbench("DraftWorkbench")

    def tearDown(self):
        Gui.activateWorkbench(self.workbench)

    def _cases(self, draft, base, path, circle, points, owner=False):
        from draftmake.make_layer import get_layer_container

        v = App.Vector
        vertices = [v(0, 0, 0), v(10, 0, 0), v(10, 10, 0)]
        selection = SimpleNamespace(Object=base, SubElementNames=["Face1"])

        def bim_clone():
            class ComponentFixture:
                Type = "Component"

                def execute(self, obj):
                    pass

            source = base.Document.addObject("Part::FeaturePython", "BIMSource")
            source.Shape = base.Shape
            source.addProperty("App::PropertyLink", "CloneOf")
            source.Proxy = ComponentFixture()
            return draft.make_clone(source)

        cases = [
            ("make_rectangle", lambda: draft.make_rectangle(20, 10)),
            ("make_line", lambda: draft.make_line(vertices[0], vertices[1])),
            ("make_wire", lambda: draft.make_wire(vertices)),
            ("make_circle", lambda: draft.make_circle(5)),
            ("make_arc_3_points", lambda: draft.make_arc_3_points(vertices)),
            (
                "make_arc_3_points/primitive",
                lambda: draft.make_arc_3_points(vertices, primitive=True),
            ),
            ("make_polygon", lambda: draft.make_polygon(5, 10)),
            ("make_ellipse", lambda: draft.make_ellipse(10, 5)),
            ("make_bspline", lambda: draft.make_bspline(vertices)),
            ("make_bez_curve", lambda: draft.make_bez_curve(vertices)),
            ("make_point", lambda: draft.make_point(1, 2, 3, color=(1, 0, 0), point_size=7)),
            (
                "make_text",
                lambda: draft.make_text(
                    "Owner", screen=True, height=8, **({"line_spacing": 1.7} if owner else {})
                ),
            ),
            ("make_dimension", lambda: draft.make_dimension(vertices[0], vertices[1])),
            (
                "make_linear_dimension",
                lambda: draft.make_linear_dimension(vertices[0], vertices[1]),
            ),
            ("make_linear_dimension_obj", lambda: draft.make_linear_dimension_obj(base)),
            ("make_radial_dimension_obj", lambda: draft.make_radial_dimension_obj(circle)),
            ("make_angular_dimension", lambda: draft.make_angular_dimension()),
            (
                "make_label",
                lambda: draft.make_label(target_object=base, **({"distance": 12} if owner else {})),
            ),
            ("make_clone", lambda: draft.make_clone(base, forcedraft=True)),
            ("make_clone/BIM", bim_clone),
            ("make_block", lambda: draft.make_block([base])),
            ("make_facebinder", lambda: draft.make_facebinder([selection])),
            (
                "make_fillet",
                lambda: draft.make_fillet(
                    [
                        Part.makeLine(vertices[0], vertices[1]),
                        Part.makeLine(vertices[1], vertices[2]),
                    ],
                    radius=1,
                ),
            ),
            (
                "make_hatch",
                lambda: draft.make_hatch(
                    [(base, ["Face1"])],
                    App.getResourceDir() + "Mod/TechDraw/PAT/FCPAT.pat",
                    "Horizontal5",
                    1,
                    0,
                ),
            ),
            (
                "make_shape_string",
                lambda: draft.make_shape_string(
                    "Owner",
                    App.getResourceDir() + "Mod/TechDraw/Resources/fonts/osifont-lgpl3fe.ttf",
                    size=10,
                ),
            ),
            ("make_shape_2d_view", lambda: draft.make_shape_2d_view(base)),
            ("make_working_plane_proxy", lambda: draft.make_working_plane_proxy(App.Placement())),
            ("get_layer_container", lambda: get_layer_container()),
            (
                "make_layer",
                lambda: draft.make_layer(**({
                    "line_color": (1.0, 0.0, 0.0),
                    "shape_color": (0.0, 1.0, 0.0),
                    "line_width": 4,
                    "draw_style": "Dashed",
                    "transparency": 50,
                } if owner else {})),
            ),
            ("make_copy/simple", lambda: draft.make_copy(base, simple_copy=True)),
        ]
        for link in (False, True):
            for name, call in (
                (
                    "make_array",
                    lambda link=link: draft.make_array(
                        base, v(10, 0, 0), v(0, 10, 0), 2, 2, use_link=link
                    ),
                ),
                ("make_ortho_array", lambda link=link: draft.make_ortho_array(base, use_link=link)),
                (
                    "make_ortho_array_2d",
                    lambda link=link: draft.make_ortho_array_2d(base, use_link=link),
                ),
                ("make_rect_array", lambda link=link: draft.make_rect_array(base, use_link=link)),
                (
                    "make_rect_array_2d",
                    lambda link=link: draft.make_rect_array_2d(base, use_link=link),
                ),
                ("make_polar_array", lambda link=link: draft.make_polar_array(base, use_link=link)),
                (
                    "make_circular_array",
                    lambda link=link: draft.make_circular_array(base, use_link=link),
                ),
                (
                    "make_path_array",
                    lambda link=link: draft.make_path_array(base, path, use_link=link),
                ),
                (
                    "make_path_twisted_array",
                    lambda link=link: draft.make_path_twisted_array(base, path, use_link=link),
                ),
                (
                    "make_point_array",
                    lambda link=link: draft.make_point_array(base, points, use_link=link),
                ),
            ):
                cases.append((name + ("/link" if link else "/shape"), call))
        return cases

    def _check_factories(self, owner):
        import Draft

        # One document per factory keeps visibility, grouping and lane state independent.
        names = [name for name, _ in self._cases(Draft, None, None, None, None)]
        for index, name in enumerate(names):
            doc = App.newDocument("DraftFactoryThread_{}_{}".format(owner, index))
            try:

                created = []

                def exercise():
                    self.assertEqual(gui_utils.on_gui_thread(), not owner)
                    base = doc.addObject("Part::Feature", "Base")
                    base.Shape = Part.makeBox(2, 3, 4)
                    path = doc.addObject("Part::Feature", "Path")
                    path.Shape = Part.makeLine(App.Vector(), App.Vector(100, 0, 0))
                    circle = doc.addObject("Part::Feature", "CircleBase")
                    circle.Shape = Part.Wire([Part.makeCircle(5)])
                    points = doc.addObject("Part::Feature", "Points")
                    points.Shape = Part.makeCompound(
                        [Part.Vertex(App.Vector()), Part.Vertex(App.Vector(10, 0, 0))]
                    )
                    cases = self._cases(Draft, base, path, circle, points, owner)
                    name, call = cases[index]
                    obj = call()
                    created.append((obj, base))
                    self.assertIsNotNone(obj, name)
                    self.assertIs(obj.Document, doc, name)
                    if name == "make_clone/BIM":
                        self.assertEqual(obj.Proxy.Type, "Clone" if owner else "Component")
                    if name == "make_working_plane_proxy":
                        obj.Proxy.execute(obj)
                    if not owner and name not in (
                        "make_arc_3_points/primitive",
                        "make_copy/simple",
                    ):
                        self.assertTrue(
                            callable(getattr(obj.ViewObject.Proxy, "getIcon", None)), name
                        )
                    if not owner and "/" in name and name.endswith(("/link", "/shape")):
                        self.assertFalse(base.ViewObject.Visibility, name)
                    if not owner:
                        if name == "make_point":
                            self.assertEqual(obj.ViewObject.PointSize, 7)
                            self.assertEqual(obj.ViewObject.PointColor[:3], (1.0, 0.0, 0.0))
                        if name == "make_text":
                            self.assertEqual(obj.ViewObject.DisplayMode, "Screen")
                            self.assertEqual(obj.ViewObject.FontSize.Value, 8)

                # Names are deterministic without creating any fixture on the GUI thread.
                with self.subTest(factory=name, owner=owner):
                    if owner:
                        errors = []

                        def attempt():
                            # Keep partially built objects until close_document; rolling
                            # back a newly attached Link extension obscures the failure.
                            try:
                                exercise()
                            except BaseException as exc:
                                errors.append(exc)

                        def worker():
                            try:
                                result = doc.commitCompatibilityMutation(
                                    attempt, structural=True, recompute=False
                                )
                                self.assertTrue(result["committed"], result)
                            except BaseException as exc:
                                errors.append(exc)

                        original_selection = Gui.Selection.clearSelection
                        original_timer = QtCore.QTimer.singleShot

                        def guarded_selection(*args, **kwargs):
                            if not gui_utils.on_gui_thread():
                                raise AssertionError("GUI selection touched by document owner")
                            return original_selection(*args, **kwargs)

                        def guarded_timer(*args, **kwargs):
                            if not gui_utils.on_gui_thread():
                                raise AssertionError("GUI timer touched by document owner")
                            return original_timer(*args, **kwargs)

                        imports = []
                        original_import = builtins.__import__

                        def owner_import(module, *args, **kwargs):
                            if module == "Arch" and not gui_utils.on_gui_thread():
                                imports.append(module)
                                raise AssertionError("owner delegated to a BIM GUI factory")
                            return original_import(module, *args, **kwargs)

                        with patch.object(
                            Gui.Selection, "clearSelection", side_effect=guarded_selection
                        ), patch.object(
                            QtCore.QTimer, "singleShot", side_effect=guarded_timer
                        ), patch(
                            "builtins.__import__", side_effect=owner_import
                        ):
                            thread = threading.Thread(target=worker)
                            thread.start()
                            _pump_gui_until(lambda: not thread.is_alive(), 60)
                            thread.join()
                        if errors:
                            raise errors[0]
                        self.assertFalse(imports, name)
                    else:
                        exercise()
                        QtCore.QCoreApplication.processEvents()
                    # Settle model execution and scene updates before tearing down
                    # the fixture, including arrays created with their native VP.
                    recompute_document(doc)
                    if owner:
                        obj, base = created[0]
                        self._assert_owner_presentation(name, obj, base)
                    self.assertFalse(
                        [item.Name for item in doc.Objects if "Invalid" in item.State], name
                    )
            finally:
                close_document(doc)
                Gui.updateGui()

    def _assert_owner_presentation(self, name, obj, base):
        providers = {
            "make_rectangle": "ViewProviderRectangle",
            "make_line": "ViewProviderWire",
            "make_wire": "ViewProviderWire",
            "make_circle": "ViewProviderDraft",
            "make_arc_3_points": "ViewProviderDraft",
            "make_polygon": "ViewProviderDraft",
            "make_ellipse": "ViewProviderDraft",
            "make_bspline": "ViewProviderBSpline",
            "make_bez_curve": "ViewProviderBezCurve",
            "make_point": "ViewProviderPoint",
            "make_text": "ViewProviderText",
            "make_dimension": "ViewProviderLinearDimension",
            "make_linear_dimension": "ViewProviderLinearDimension",
            "make_linear_dimension_obj": "ViewProviderLinearDimension",
            "make_radial_dimension_obj": "ViewProviderLinearDimension",
            "make_angular_dimension": "ViewProviderAngularDimension",
            "make_label": "ViewProviderLabel",
            "make_clone": "ViewProviderClone",
            "make_clone/BIM": "ViewProviderClone",
            "make_block": "ViewProviderDraftPart",
            "make_facebinder": "ViewProviderFacebinder",
            "make_fillet": "ViewProviderFillet",
            "make_hatch": "ViewProviderDraftHatch",
            "make_shape_string": "ViewProviderShapeString",
            "make_shape_2d_view": "ViewProviderDraftAlt",
            "make_working_plane_proxy": "ViewProviderWorkingPlaneProxy",
            "get_layer_container": "ViewProviderLayerContainer",
            "make_layer": "ViewProviderLayer",
        }
        if name.endswith(("/link", "/shape")):
            expected = "ViewProviderDraftLink" if name.endswith("/link") else "ViewProviderDraftArray"
            self.assertFalse(base.ViewObject.Visibility, name)
        else:
            expected = providers.get(name)
        if expected:
            # Model readiness can precede a queued GUI setup's next turn.
            _pump_gui_until(lambda: callable(getattr(obj.ViewObject.Proxy, "getIcon", None)), 10)
            self.assertEqual(type(obj.ViewObject.Proxy).__name__, expected, name)
            self.assertTrue(obj.ViewObject.Visibility, name)
        if name == "make_point":
            self.assertEqual(obj.ViewObject.PointSize, 7)
            self.assertEqual(obj.ViewObject.PointColor[:3], (1.0, 0.0, 0.0))
        elif name == "make_text":
            self.assertEqual(obj.ViewObject.DisplayMode, "Screen")
            self.assertEqual(obj.ViewObject.FontSize.Value, 8)
            self.assertAlmostEqual(obj.ViewObject.LineSpacing, 1.7)
        elif name == "make_label":
            self.assertEqual(obj.ViewObject.FontSize.Value, params.get_param("textheight"))
            self.assertEqual(obj.ViewObject.Justification, "Right")
        elif name == "make_radial_dimension_obj":
            self.assertEqual(obj.ViewObject.Override, "R $dim")
        elif name == "make_layer":
            self.assertEqual(obj.ViewObject.LineColor[:3], (1.0, 0.0, 0.0))
            self.assertEqual(obj.ViewObject.ShapeColor[:3], (0.0, 1.0, 0.0))
            self.assertEqual(obj.ViewObject.LineWidth, 4)
            self.assertEqual(obj.ViewObject.DrawStyle, "Dashed")
            self.assertEqual(obj.ViewObject.Transparency, 50)
        elif name == "make_shape_string":
            self.assertEqual(obj.ViewObject.PointSize, 1)

    def test_default_copies_on_document_owner_thread(self):
        import Draft

        for kind in ("text", "wire"):
            with self.subTest(kind=kind):
                doc = App.newDocument("DraftOwnerCopy")
                try:
                    if kind == "text":
                        source = Draft.make_text("Copy", height=13, screen=True)
                        source.ViewObject.addProperty("App::PropertyFloat", "LineWidth")
                        source.ViewObject.LineWidth = 5
                    else:
                        source = Draft.make_wire([App.Vector(), App.Vector(10, 0, 0)])
                        source.ViewObject.LineWidth = 6
                        source.ViewObject.addProperty("App::PropertyLength", "ArrowSize")
                        source.ViewObject.ArrowSize = 9
                    # Export assigns this source identity on first copy. Give
                    # the fixture its identity before the structural owner commit.
                    source.addProperty("App::PropertyUUID", "_ObjectUUID")
                    recompute_document(doc)
                    copies, errors = [], []

                    def copy():
                        try:
                            copies.append(Draft.make_copy(source))
                        except BaseException as exc:
                            errors.append(exc)

                    def worker():
                        try:
                            doc.commitCompatibilityMutation(copy, structural=True, recompute=False)
                        except BaseException as exc:
                            errors.append(exc)

                    original_restore = type(source.Proxy).onDocumentRestored

                    def checked_restore(proxy, obj):
                        # FreeCAD logs and absorbs Python restore exceptions. Keep
                        # them visible to this regression even if copyObject returns.
                        try:
                            return original_restore(proxy, obj)
                        except BaseException as exc:
                            errors.append(exc)
                            raise

                    with patch.object(type(source.Proxy), "onDocumentRestored", checked_restore):
                        thread = threading.Thread(target=worker)
                        thread.start()
                        _pump_gui_until(lambda: not thread.is_alive(), 60)
                        thread.join()
                        recompute_document(doc)
                    if errors:
                        raise errors[0]
                    self.assertEqual(len(copies), 1)
                    copied = copies[0]
                    self.assertEqual(type(copied.ViewObject.Proxy), type(source.ViewObject.Proxy))
                    self.assertTrue(copied.ViewObject.Visibility)
                    if kind == "text":
                        self.assertEqual(copied.ViewObject.FontSize.Value, 13)
                        self.assertEqual(copied.ViewObject.DisplayMode, "Screen")
                        self.assertEqual(copied.Text, source.Text)
                        self.assertNotIn("LineWidth", copied.ViewObject.PropertiesList)
                    else:
                        self.assertEqual(copied.ViewObject.LineWidth, 6)
                        self.assertEqual(copied.Points, source.Points)
                        self.assertNotIn("ArrowSize", copied.ViewObject.PropertiesList)
                        self.assertEqual(copied.ViewObject.ArrowSizeStart.Value, 9)
                        self.assertEqual(copied.ViewObject.ArrowSizeEnd.Value, 9)
                finally:
                    close_document(doc)
                    Gui.updateGui()

    def test_annotation_presentation_after_commit_without_recompute(self):
        import Draft
        import time

        def pump_until(predicate, timeout):
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                QtCore.QCoreApplication.processEvents(QtCore.QEventLoop.AllEvents, 25)
                if predicate():
                    return
                time.sleep(0.01)
            self.fail("presentation was not applied after owner commit")

        doc = App.newDocument("DraftOwnerAnnotations")
        errors = []
        try:
            def create():
                self.assertFalse(gui_utils.on_gui_thread())
                Draft.make_text("Committed", height=11, screen=False)
                Draft.make_point(1, 2, 3, color=(0, 0, 1), point_size=9)

            def worker():
                try:
                    result = doc.commitCompatibilityMutation(create, structural=True, recompute=False)
                    self.assertTrue(result["committed"], result)
                except BaseException as exc:
                    errors.append(exc)

            thread = threading.Thread(target=worker)
            thread.start()
            pump_until(lambda: not thread.is_alive(), 60)
            thread.join()
            if errors:
                raise errors[0]
            text, point = doc.getObject("Text"), doc.getObject("Point")
            # Main-thread factories install presentation before recompute too.
            pump_until(
                lambda: type(text.ViewObject.Proxy).__name__ == "ViewProviderText"
                and type(point.ViewObject.Proxy).__name__ == "ViewProviderPoint",
                5,
            )
            self.assertEqual(text.ViewObject.FontSize.Value, 11)
            self.assertEqual(text.ViewObject.DisplayMode, "World")
            self.assertTrue(text.ViewObject.isVisible())
            self.assertEqual(point.ViewObject.PointSize, 9)
            self.assertEqual(point.ViewObject.PointColor[:3], (0.0, 0.0, 1.0))
        finally:
            recompute_document(doc)
            close_document(doc)
            Gui.updateGui()

    def test_factories_on_document_owner_thread(self):
        self._check_factories(True)

    def test_factories_on_main_thread(self):
        self._check_factories(False)


class DraftDeferredArrayPresentation(unittest.TestCase):
    # Kept out of DraftOwnerThreadFactories: the MCP live-GUI suite replays that
    # class's exact test list.
    def setUp(self):
        self.workbench = Gui.activeWorkbench().name()
        Gui.activateWorkbench("DraftWorkbench")

    def tearDown(self):
        Gui.activateWorkbench(self.workbench)

    def test_deferred_array_presentation_keeps_model_ready(self):
        import Draft

        for kind in ("circular", "twisted", "point"):
            with self.subTest(array=kind):
                doc = App.newDocument("DraftLateArrayPresentation_" + kind)
                pending = []
                errors = []
                created = []
                try:
                    def create():
                        self.assertFalse(gui_utils.on_gui_thread())
                        base = doc.addObject("Part::Feature", "Base")
                        base.Shape = Part.makeBox(2, 3, 4)
                        if kind == "circular":
                            array = Draft.make_circular_array(base, use_link=False)
                        elif kind == "twisted":
                            path = doc.addObject("Part::Feature", "Path")
                            path.Shape = Part.makeLine(App.Vector(), App.Vector(100, 0, 0))
                            array = Draft.make_path_twisted_array(base, path, use_link=False)
                        else:
                            points = doc.addObject("Part::Feature", "Points")
                            points.Shape = Part.makeCompound(
                                [Part.Vertex(App.Vector()), Part.Vertex(App.Vector(10, 0, 0))]
                            )
                            array = Draft.make_point_array(base, points, use_link=False)
                        created.append((base, array))

                    def worker():
                        try:
                            result = doc.commitCompatibilityMutation(
                                create, structural=True, recompute=False
                            )
                            self.assertTrue(result["committed"], result)
                        except BaseException as exc:
                            errors.append(exc)

                    # Force the late Qt turn that otherwise only occasionally
                    # falls after recompute_document's last readiness check.
                    with patch.object(
                        gui_utils, "run_gui_setup",
                        side_effect=lambda obj, callback: pending.append((obj, callback)),
                    ):
                        thread = threading.Thread(target=worker)
                        thread.start()
                        _pump_gui_until(lambda: not thread.is_alive(), 60)
                        thread.join()
                    if errors:
                        raise errors[0]
                    self.assertEqual(len(pending), 1)
                    base, array = created[0]
                    recompute_document(doc)
                    self.assertFalse(doc.mustExecute())
                    base.ViewObject.DiffuseColor = [(1.0, 0.0, 0.0), (0.0, 1.0, 0.0)] * 3
                    expected_colors = gui_utils.get_diffuse_color(base) * array.Count

                    obj, callback = pending.pop()
                    gui_utils.run_gui_setup(obj, callback)
                    _pump_gui_until(
                        lambda: type(array.ViewObject.Proxy).__name__ == "ViewProviderDraftArray",
                        10,
                    )
                    self.assertFalse(doc.mustExecute(), doc.getMutationReadiness())
                    self.assertEqual(len(array.PlacementList), array.Count)
                    self.assertGreater(array.Count, 0)
                    self.assertEqual(array.ViewObject.DiffuseColor, expected_colors)
                    self.assertFalse(base.ViewObject.Visibility)
                finally:
                    # A failing pre-fix presentation callback leaves real model
                    # work behind; settle it before closing the fixture.
                    recompute_document(doc)
                    close_document(doc)
                    Gui.updateGui()
