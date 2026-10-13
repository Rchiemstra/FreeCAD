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
from draftutils import gui_utils


class DraftOwnerThreadFactories(unittest.TestCase):
    def setUp(self):
        self.workbench = Gui.activeWorkbench().name()
        Gui.activateWorkbench("DraftWorkbench")

    def tearDown(self):
        Gui.activateWorkbench(self.workbench)

    def _cases(self, draft, base, path, circle, points):
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
            ("make_text", lambda: draft.make_text("Owner", screen=True, height=8)),
            ("make_dimension", lambda: draft.make_dimension(vertices[0], vertices[1])),
            (
                "make_linear_dimension",
                lambda: draft.make_linear_dimension(vertices[0], vertices[1]),
            ),
            ("make_linear_dimension_obj", lambda: draft.make_linear_dimension_obj(base)),
            ("make_radial_dimension_obj", lambda: draft.make_radial_dimension_obj(circle)),
            ("make_angular_dimension", lambda: draft.make_angular_dimension()),
            ("make_label", lambda: draft.make_label(target_object=base)),
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
            ("make_layer", lambda: draft.make_layer()),
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
                    cases = self._cases(Draft, base, path, circle, points)
                    name, call = cases[index]
                    obj = call()
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
                    self.assertFalse(
                        [item.Name for item in doc.Objects if "Invalid" in item.State], name
                    )
            finally:
                close_document(doc)
                Gui.updateGui()

    def test_factories_on_document_owner_thread(self):
        self._check_factories(True)

    def test_factories_on_main_thread(self):
        self._check_factories(False)
