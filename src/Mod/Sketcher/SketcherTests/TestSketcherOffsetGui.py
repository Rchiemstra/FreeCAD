# SPDX-License-Identifier: LGPL-2.1-or-later

import unittest

import FreeCAD
import Part
import Sketcher

try:
    import FreeCADGui

    GUI_AVAILABLE = FreeCADGui.getMainWindow() is not None
except (ImportError, AttributeError):
    GUI_AVAILABLE = False

from PySide import QtCore, QtGui

from SketcherTests.GuiTestCase import SketcherGuiTestCase


def _line(start, end):
    return Part.LineSegment(FreeCAD.Vector(*start, 0), FreeCAD.Vector(*end, 0))


class _OffsetComputationObserver(QtCore.QObject):
    def __init__(self):
        super().__init__()
        self.counts = []

    def eventFilter(self, obj, event):
        if (
            event.type() == QtCore.QEvent.DynamicPropertyChange
            and bytes(event.propertyName()) == b"computationCount"
        ):
            self.counts.append(obj.property("computationCount"))
        return False


class TestSketcherOffsetGui(SketcherGuiTestCase):
    def setUp(self):
        if not GUI_AVAILABLE:
            self.skipTest("GUI not available")

        super().setUp()
        FreeCADGui.activateWorkbench("SketcherWorkbench")
        self.doc = FreeCAD.newDocument("TestSketcherOffsetGui")
        self.sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        self.recompute_gui_document()

    def tearDown(self):
        if not GUI_AVAILABLE:
            return

        doc = self.doc
        self.doc = None
        self.cleanup_gui_document(doc)
        super().tearDown()

    def pump(self, timeout_ms=50):
        loop = QtCore.QEventLoop()
        QtCore.QTimer.singleShot(timeout_ms, loop.quit)
        loop.exec_()

    def wait_until(self, predicate, timeout_ms=2000, step_ms=50):
        remaining = timeout_ms
        while remaining > 0:
            if predicate():
                return True
            self.pump(step_ms)
            remaining -= step_ms
        return predicate()

    def send_mouse(self, widget, event_type, pos, button, buttons):
        global_pos = widget.mapToGlobal(pos)
        event = QtGui.QMouseEvent(
            event_type,
            pos,
            global_pos,
            button,
            buttons,
            QtCore.Qt.NoModifier,
        )
        QtGui.QApplication.sendEvent(widget, event)

    def move(self, widget, pos):
        self.send_mouse(
            widget,
            QtCore.QEvent.MouseMove,
            pos,
            QtCore.Qt.NoButton,
            QtCore.Qt.NoButton,
        )
        self.pump(80)

    def click(self, widget, pos):
        self.send_mouse(
            widget,
            QtCore.QEvent.MouseButtonPress,
            pos,
            QtCore.Qt.LeftButton,
            QtCore.Qt.LeftButton,
        )
        self.send_mouse(
            widget,
            QtCore.QEvent.MouseButtonRelease,
            pos,
            QtCore.Qt.LeftButton,
            QtCore.Qt.NoButton,
        )
        self.pump(200)

    def add_open_chain(self):
        first = self.sketch.addGeometry(_line((0, 0), (10, 0)))
        second = self.sketch.addGeometry(_line((10, 0), (10, 8)))
        self.sketch.addConstraint(Sketcher.Constraint("Coincident", first, 2, second, 1))
        return first, second

    def add_rectangle(self):
        geos = [
            _line((-10, 5), (10, 5)),
            _line((10, 5), (10, -5)),
            _line((10, -5), (-10, -5)),
            _line((-10, -5), (-10, 5)),
        ]
        first_geo = int(self.sketch.GeometryCount)
        self.sketch.addGeometry(geos, False)
        self.sketch.addConstraint(
            [
                Sketcher.Constraint("Coincident", first_geo + 0, 2, first_geo + 1, 1),
                Sketcher.Constraint("Coincident", first_geo + 1, 2, first_geo + 2, 1),
                Sketcher.Constraint("Coincident", first_geo + 2, 2, first_geo + 3, 1),
                Sketcher.Constraint("Coincident", first_geo + 3, 2, first_geo + 0, 1),
                Sketcher.Constraint("Horizontal", first_geo + 0),
                Sketcher.Constraint("Vertical", first_geo + 1),
                Sketcher.Constraint("Horizontal", first_geo + 2),
                Sketcher.Constraint("Vertical", first_geo + 3),
            ]
        )
        return list(range(first_geo, first_geo + 4))

    def add_circle(self):
        geo_id = self.sketch.addGeometry(
            Part.Circle(FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(0, 0, 1), 5.0),
            False,
        )
        self.sketch.addConstraint(Sketcher.Constraint("Radius", geo_id, 5.0))
        return geo_id

    def offset_mode_combobox(self):
        main_window = FreeCADGui.getMainWindow()
        for combo in main_window.findChildren(QtGui.QComboBox):
            items = [combo.itemText(index) for index in range(combo.count())]
            if combo.objectName() == "comboBox1" and combo.count() >= 3:
                return combo
        return None

    def offset_checkbox(self, text_fragment):
        main_window = FreeCADGui.getMainWindow()
        for checkbox in main_window.findChildren(QtGui.QCheckBox):
            if text_fragment in checkbox.text():
                return checkbox
        return None

    def describe_offset_controls(self):
        main_window = FreeCADGui.getMainWindow()
        combos = []
        for combo in main_window.findChildren(QtGui.QComboBox):
            items = [combo.itemText(index) for index in range(combo.count())]
            if items:
                combos.append(f"{combo.objectName()}={items}")
        checkboxes = [
            f"{checkbox.objectName()}={checkbox.text()!r}"
            for checkbox in main_window.findChildren(QtGui.QCheckBox)
            if checkbox.text()
        ]
        return f"comboboxes: {combos}; checkboxes: {checkboxes}"

    def set_offset_mode(self, mode_text, fallback_index=None):
        self.assertTrue(
            self.wait_until(lambda: self.offset_mode_combobox() is not None),
            "Expected the offset mode combobox to be available; "
            + self.describe_offset_controls(),
        )
        combo = self.offset_mode_combobox()
        index = combo.findText(mode_text)
        if index < 0 and fallback_index is not None:
            index = fallback_index
        self.assertGreaterEqual(index, 0, f"Expected offset mode {mode_text!r} to be available")
        combo.setCurrentIndex(index)
        self.pump(200)

    def set_offset_checkbox(self, text_fragment, checked, fallback_index=None):
        checkbox = self.offset_checkbox(text_fragment)
        if checkbox is None and fallback_index is not None:
            checkboxes = [
                candidate
                for candidate in FreeCADGui.getMainWindow().findChildren(QtGui.QCheckBox)
                if candidate.objectName().startswith("checkBoxTS")
            ]
            if fallback_index < len(checkboxes):
                checkbox = checkboxes[fallback_index]
        self.assertIsNotNone(
            checkbox,
            f"Expected offset checkbox containing {text_fragment!r}; "
            + self.describe_offset_controls(),
        )
        if checkbox.isChecked() != checked:
            checkbox.setChecked(checked)
            self.pump(200)

    def start_editing(self):
        FreeCADGui.ActiveDocument.setEdit(self.sketch.Name)
        self.recompute_gui_document()
        self.flush_gui(250)
        view = FreeCADGui.ActiveDocument.ActiveView
        view.setAnimationEnabled(False)
        view.viewTop()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        view.redraw()
        self.flush_gui(200)
        return view

    def in_view_offset_screen_point(self, view, click_point):
        """Project an Offset click to Coin pixels used as QPoint.

        After skip-bbox, ViewFit of a small rectangle can leave (0, 12) a few
        pixels past the Coin height. Keep the same offset ray and step toward
        the origin until the click is inside the viewport. Do not accept the
        (0, 0) sentinel and do not walk onto the origin.
        """
        viewport = view.graphicsView().viewport()
        rect = viewport.rect()
        preferred = QtCore.QPoint(*view.getPointOnScreen(click_point))
        if preferred != QtCore.QPoint(0, 0) and rect.contains(preferred):
            return preferred

        origin = FreeCAD.Vector(0.0, 0.0, 0.0)
        direction = click_point - origin
        last = preferred
        for step in range(1, 6):
            candidate = origin + direction * (1.0 - 0.1 * step)
            last = QtCore.QPoint(*view.getPointOnScreen(candidate))
            if last != QtCore.QPoint(0, 0) and rect.contains(last):
                return last

        self.fail(
            f"Expected offset click {preferred} from {click_point} to fall "
            f"inside the sketch viewport {rect}; last candidate {last}"
        )

    def run_offset_from_selection(
        self,
        subelement_names,
        click_point,
        *,
        mode_text=None,
        chain_link=False,
    ):
        view = self.start_editing()

        FreeCADGui.Selection.clearSelection()
        for subelement_name in subelement_names:
            FreeCADGui.Selection.addSelection(self.doc.Name, self.sketch.Name, subelement_name)
        self.pump(150)

        before_geometry = self.sketch.GeometryCount
        before_constraints = len(self.sketch.Constraints)

        FreeCADGui.runCommand("Sketcher_Offset")
        self.pump(300)

        if mode_text == "Intersection":
            self.set_offset_mode(mode_text, fallback_index=1)
        elif mode_text == "Constrained Clearance":
            self.set_offset_mode(mode_text, fallback_index=2)
        elif mode_text is not None:
            self.set_offset_mode(mode_text)

        if chain_link:
            self.set_offset_checkbox("Chain link", True, fallback_index=2)

        viewport = view.graphicsView().viewport()
        screen_point = self.in_view_offset_screen_point(view, click_point)
        self.assertTrue(
            viewport.rect().contains(screen_point),
            f"Expected {screen_point} to fall inside the sketch viewport {viewport.rect()}",
        )

        self.move(viewport, screen_point)
        self.click(viewport, screen_point)

        self.assertTrue(
            self.wait_until(lambda: self.sketch.GeometryCount > before_geometry),
            "Expected Sketcher_Offset to add geometry",
        )
        self.recompute_gui_document()

        return before_geometry, before_constraints

    def assert_solves_without_conflicts(self):
        status = self.sketch.solve()
        self.assertEqual(status, 0, "Expected the offset sketch to solve")

    def new_constraint_types(self, before_constraints):
        return [constraint.Type for constraint in self.sketch.Constraints[before_constraints:]]

    @unittest.skipIf(not GUI_AVAILABLE, "GUI not available")
    def test_offset_single_line_adds_geometry(self):
        self.sketch.addGeometry(_line((-10, 0), (10, 0)))
        self.recompute_gui_document()

        before_geometry, _ = self.run_offset_from_selection(
            ["Edge1"],
            FreeCAD.Vector(0, 5, 0),
        )

        self.assertGreater(self.sketch.GeometryCount, before_geometry)
        self.assert_solves_without_conflicts()

    @unittest.skipIf(not GUI_AVAILABLE, "GUI not available")
    def test_offset_closed_rectangle_arc_mode_adds_connected_profile(self):
        self.add_rectangle()
        self.recompute_gui_document()

        before_geometry, before_constraints = self.run_offset_from_selection(
            ["Edge1", "Edge2", "Edge3", "Edge4"],
            FreeCAD.Vector(0, 12, 0),
        )

        self.assertGreaterEqual(self.sketch.GeometryCount - before_geometry, 4)
        self.assertIn("Tangent", self.new_constraint_types(before_constraints))
        self.assert_solves_without_conflicts()

    @unittest.skipIf(not GUI_AVAILABLE, "GUI not available")
    def test_offset_closed_rectangle_intersection_mode_adds_profile(self):
        self.add_rectangle()
        self.recompute_gui_document()

        before_geometry, before_constraints = self.run_offset_from_selection(
            ["Edge1", "Edge2", "Edge3", "Edge4"],
            FreeCAD.Vector(0, 12, 0),
            mode_text="Intersection",
        )

        self.assertGreaterEqual(self.sketch.GeometryCount - before_geometry, 4)
        self.assertIn("Coincident", self.new_constraint_types(before_constraints))
        self.assert_solves_without_conflicts()

    @unittest.skipIf(not GUI_AVAILABLE, "GUI not available")
    def test_offset_circle_adds_geometry(self):
        self.add_circle()
        self.recompute_gui_document()

        before_geometry, _ = self.run_offset_from_selection(
            ["Edge1"],
            FreeCAD.Vector(8, 0, 0),
        )

        self.assertGreater(self.sketch.GeometryCount, before_geometry)
        self.assert_solves_without_conflicts()

    @unittest.skipIf(not GUI_AVAILABLE, "GUI not available")
    def test_constrained_clearance_rectangle_adds_driving_clearance_constraints(self):
        self.add_rectangle()
        self.recompute_gui_document()

        _, before_constraints = self.run_offset_from_selection(
            ["Edge1", "Edge2", "Edge3", "Edge4"],
            FreeCAD.Vector(0, 12, 0),
            mode_text="Constrained Clearance",
        )

        new_types = self.new_constraint_types(before_constraints)
        self.assertIn("Parallel", new_types)
        self.assertTrue(
            any(
                constraint_type in {"Distance", "DistanceX", "DistanceY"}
                for constraint_type in new_types
            ),
            "Expected constrained clearance to add a distance constraint",
        )
        self.assert_solves_without_conflicts()

    @unittest.skipIf(not GUI_AVAILABLE, "GUI not available")
    def test_chain_link_offsets_connected_open_edges_from_single_selected_edge(self):
        self.add_open_chain()
        self.recompute_gui_document()

        before_geometry, _ = self.run_offset_from_selection(
            ["Edge1"],
            FreeCAD.Vector(4, 4, 0),
            chain_link=True,
        )

        self.assertGreaterEqual(
            self.sketch.GeometryCount - before_geometry,
            2,
            "Expected chain link to offset more than the single selected edge",
        )
        self.assert_solves_without_conflicts()

    # The B-spline route uses actual Qt controls, with no Python implementation of the fit.
    def spline_control(self, widget_type, suffix):
        controls = FreeCADGui.getMainWindow().findChildren(widget_type, "BSplineOffset" + suffix)
        return next((control for control in controls if control.isVisible()), None)

    @staticmethod
    def spline_signature(curve):
        return (
            tuple((p.x, p.y, p.z) for p in curve.getPoles()),
            tuple(curve.getWeights()),
            tuple(curve.getKnots()),
            tuple(curve.getMultiplicities()),
            curve.Degree,
            curve.isPeriodic(),
        )

    def offset_snapshot(self):
        return (
            tuple(g.toShape().exportBrepToString() for g in self.sketch.Geometry),
            tuple(str(c) for c in self.sketch.Constraints),
            tuple(self.sketch.getConstruction(i) for i in range(self.sketch.GeometryCount)),
            self.doc.UndoCount,
            self.doc.RedoCount,
            tuple(self.doc.UndoNames),
            tuple(self.doc.RedoNames),
        )

    def start_spline_offset(
        self, *, reversed_source=False, rational=False, closed=False, smooth_closed=False,
        periodic=True, self_crossing=False
    ):
        curve = Part.BSplineCurve()
        if self_crossing:
            import math

            poles = []
            for i in range(16):
                t = i * math.pi / 8
                poles.append(FreeCAD.Vector(
                    10 * math.cos(t), 2.5 * math.sin(t) + 3.1 * math.sin(3 * t), 0
                ))
            curve.buildFromPolesMultsKnots(
                poles, [1] * 17, list(range(17)), True, 3, [1.0] * 16
            )
        elif smooth_closed:
            import math

            curve.buildFromPolesMultsKnots(
                [
                    FreeCAD.Vector(10 * math.cos(i * math.pi / 4),
                                   10 * math.sin(i * math.pi / 4), 0)
                    for i in range(8)
                ],
                [1] * 9,
                list(range(9)),
                True,
                3,
            )
            if not periodic:
                curve.segment(curve.FirstParameter, curve.LastParameter)
        elif rational:
            curve.buildFromPolesMultsKnots(
                [FreeCAD.Vector(10, 0, 0), FreeCAD.Vector(10, 10, 0), FreeCAD.Vector(0, 10, 0)],
                [3, 3],
                [0.0, 1.0],
                False,
                2,
                [1.0, 2.0**-0.5, 1.0],
            )
        else:
            curve.buildFromPolesMultsKnots(
                [
                    FreeCAD.Vector(0, 0, 0),
                    FreeCAD.Vector(3, 4, 0),
                    FreeCAD.Vector(7, 4, 0),
                    FreeCAD.Vector(0 if closed else 10, 0, 0),
                ],
                [4, 4],
                [0.0, 1.0],
                False,
                3,
            )
        if reversed_source:
            curve.reverse()
        source_id = self.sketch.addGeometry(curve, False)
        self.recompute_gui_document()
        self.start_editing()
        self.select_offset_edges(["Edge" + str(source_id + 1)])
        self.assertIsNotNone(
            self.spline_control(QtGui.QWidget, "Panel"),
            "A single B-spline must dispatch to the numeric handler",
        )
        self.assertEqual(self.spline_quantity("Tolerance"), 0.01)
        return self.sketch.Geometry[source_id]

    @staticmethod
    def selection_signature():
        return tuple(
            (item.DocumentName, item.ObjectName, tuple(item.SubElementNames))
            for item in FreeCADGui.Selection.getSelectionEx()
        )

    def spline_quantity(self, suffix, value=None):
        control = self.spline_control(QtGui.QAbstractSpinBox, suffix)
        if value is not None:
            control.setProperty("rawValue", value)
        return control.property("rawValue")

    def select_offset_edges(self, edges):
        FreeCADGui.Selection.clearSelection()
        for edge in edges:
            FreeCADGui.Selection.addSelection(self.doc.Name, self.sketch.Name, edge)
        self.selection_before_offset = self.selection_signature()
        FreeCADGui.runCommand("Sketcher_Offset")
        self.pump(100)

    def spline_button(self, suffix):
        self.spline_control(QtGui.QPushButton, suffix).click()
        self.pump(80)

    def assert_spline_distance(self, source, distance, tolerance=0.01):
        status = self.spline_control(QtGui.QLabel, "Status")
        self.assertEqual(
            self.sketch.GeometryCount, 2, status.text() if status else "Offset result missing"
        )
        output = self.sketch.Geometry[1]
        self.assertIsInstance(output, Part.BSplineCurve)
        self.assertFalse(self.sketch.getConstruction(1))
        self.assertEqual([(c.Type, c.First) for c in self.sketch.Constraints], [("Block", 1)])
        self.assertEqual(
            self.spline_signature(source), self.spline_signature(self.sketch.Geometry[0])
        )
        source_shape = source.toShape()
        for i in range(201):
            u = source.FirstParameter + (source.LastParameter - source.FirstParameter) * i / 200.0
            normal = source.tangent(u)[0].cross(FreeCAD.Vector(0, 0, 1))
            expected = source.value(u) + normal * distance
            actual = output.value(u)
            self.assertLessEqual((actual - expected).Length, tolerance)
            physical_distance = Part.Vertex(actual).distToShape(source_shape)[0]
            self.assertAlmostEqual(physical_distance, abs(distance), delta=tolerance)
        self.assert_solves_without_conflicts()

    def check_spline_offset(
        self, distance=1.0, reverse=False, reversed_source=False, rational=False
    ):
        source = self.start_spline_offset(reversed_source=reversed_source, rational=rational)
        before = self.offset_snapshot()
        self.spline_quantity("Distance", distance)
        self.spline_control(QtGui.QCheckBox, "Reverse").setChecked(reverse)
        self.assertEqual(before, self.offset_snapshot())
        panel = self.spline_control(QtGui.QWidget, "Panel")
        observer = _OffsetComputationObserver()
        panel.installEventFilter(observer)
        self.spline_button("Update")
        status = self.spline_control(QtGui.QLabel, "Status").text()
        self.assertIn("Achieved maximum error:", status)
        self.assertEqual(panel.property("computationCount"), 1)
        self.assertEqual(
            before, self.offset_snapshot(), "Preview must leave the document and undo untouched"
        )
        self.spline_button("Update")
        self.assertEqual(panel.property("computationCount"), 1)
        self.spline_button("Apply")
        self.assertEqual(observer.counts, [1], "Apply must reuse the validated preview")
        self.assert_spline_distance(source, -distance if reverse else distance)
        self.assertEqual(self.doc.UndoCount, before[3] + 1)
        self.undo_gui_document()
        self.assertEqual(self.sketch.GeometryCount, 1)
        self.assertEqual(len(self.sketch.Constraints), 0)
        self.assertEqual(
            self.spline_signature(source), self.spline_signature(self.sketch.Geometry[0])
        )
        self.redo_gui_document()
        self.assert_spline_distance(source, -distance if reverse else distance)

    def test_bspline_offset_positive_preview_apply_undo_redo(self):
        self.check_spline_offset()

    def test_bspline_offset_quantity_controls_follow_unit_schema(self):
        schema = FreeCAD.Units.getSchema()
        try:
            FreeCAD.Units.setSchema(3)  # Imperial decimal
            self.start_spline_offset()
            for suffix in ("Distance", "Tolerance"):
                control = self.spline_control(QtGui.QAbstractSpinBox, suffix)
                self.assertIn("in", control.text())
            self.assertEqual(self.spline_quantity("Distance"), 1.0)
            self.spline_button("Update")
            self.assertIn("in", self.spline_control(QtGui.QLabel, "Status").text())
            self.spline_button("Cancel")
            self.assertEqual(self.selection_before_offset, self.selection_signature())
        finally:
            FreeCAD.Units.setSchema(schema)

    def test_bspline_offset_negative_preview_apply_undo_redo(self):
        self.check_spline_offset(distance=-1.0)

    def test_bspline_offset_reverse_side_preview_apply_undo_redo(self):
        self.check_spline_offset(reverse=True)

    def test_bspline_offset_negative_reverse_side_preview_apply_undo_redo(self):
        self.check_spline_offset(distance=-1.0, reverse=True)

    def test_bspline_offset_reversed_source_preview_apply_undo_redo(self):
        self.check_spline_offset(reversed_source=True)

    def test_bspline_offset_rational_preview_apply_undo_redo(self):
        self.check_spline_offset(rational=True)

    def test_bspline_offset_cancel_preview_preserves_document_and_history(self):
        self.start_spline_offset()
        before = self.offset_snapshot()
        viewport = FreeCADGui.ActiveDocument.ActiveView.graphicsView().viewport()
        self.move(viewport, viewport.rect().center())
        self.assertEqual(
            self.spline_control(QtGui.QWidget, "Panel").property("computationCount"), 0
        )
        self.assertEqual(before, self.offset_snapshot())
        retained_apply = self.spline_control(QtGui.QPushButton, "Apply")
        self.spline_button("Update")
        self.spline_button("Cancel")
        self.assertEqual(self.selection_before_offset, self.selection_signature())
        self.assertEqual(before, self.offset_snapshot())
        self.assertIsNone(self.spline_control(QtGui.QWidget, "Panel"))
        # The task box keeps its hidden widget. Its old callbacks must be disconnected.
        retained_apply.click()
        self.pump(80)
        self.assertEqual(before, self.offset_snapshot())

    def test_bspline_offset_invalid_options_preserve_document_and_history(self):
        self.start_spline_offset()
        before = self.offset_snapshot()
        for distance, tolerance in [(0.0, 0.01), (1.0, 0.0), (1.0, 0.6)]:
            with self.subTest(distance=distance, tolerance=tolerance):
                self.spline_quantity("Distance", distance)
                self.spline_quantity("Tolerance", tolerance)
                self.spline_button("Update")
                message = self.spline_control(QtGui.QLabel, "Status").text()
                self.assertIn("less than half", message)
                self.assertNotIn("Achieved maximum error:", message)
                self.spline_button("Apply")
                self.assertEqual(message, self.spline_control(QtGui.QLabel, "Status").text())
                self.assertEqual(before, self.offset_snapshot())
                self.assertEqual(self.selection_before_offset, self.selection_signature())
        self.spline_button("Cancel")
        self.assertEqual(self.selection_before_offset, self.selection_signature())
        self.assertEqual(before, self.offset_snapshot())

    def test_bspline_offset_closed_curve_shows_helper_message_without_mutation(self):
        self.start_spline_offset(closed=True)
        before = self.offset_snapshot()
        self.spline_button("Apply")
        self.assertIn("G0-only corner", self.spline_control(QtGui.QLabel, "Status").text())
        self.assertEqual(before, self.offset_snapshot())
        self.spline_button("Cancel")
        self.assertEqual(self.selection_before_offset, self.selection_signature())
        self.assertEqual(before, self.offset_snapshot())

    def check_closed_spline_offset(self, periodic=True, reversed_source=False):
        source = self.start_spline_offset(
            smooth_closed=True, periodic=periodic, reversed_source=reversed_source
        )
        before = self.offset_snapshot()
        label = self.spline_control(QtGui.QLabel, "Side")
        self.assertIsNotNone(label)
        self.assertIn("Inside / outside", label.text())
        self.spline_button("Update")
        self.assertEqual(label.text(), "Side: Inside" if reversed_source else "Side: Outside")
        self.assertEqual(before, self.offset_snapshot())
        self.spline_control(QtGui.QCheckBox, "Reverse").setChecked(True)
        self.assertEqual(label.text(), "Side: Outside" if reversed_source else "Side: Inside")
        self.spline_button("Update")
        self.assertIn("Achieved maximum error:", self.spline_control(QtGui.QLabel, "Status").text())
        self.spline_button("Apply")
        self.assert_spline_distance(source, -1.0)
        self.assertTrue(self.sketch.Geometry[1].isClosed())
        self.assertEqual(self.sketch.Geometry[1].isPeriodic(), periodic)
        self.assertEqual(self.doc.UndoCount, before[3] + 1)
        self.undo_gui_document()
        self.assertEqual(self.sketch.GeometryCount, 1)
        self.assertEqual(len(self.sketch.Constraints), 0)
        self.redo_gui_document()
        self.assertTrue(self.sketch.Geometry[1].isClosed())
        self.assertEqual(self.sketch.Geometry[1].isPeriodic(), periodic)
        self.assertEqual([(c.Type, c.First) for c in self.sketch.Constraints], [("Block", 1)])

    def test_bspline_offset_periodic_closed_apply_undo(self):
        self.check_closed_spline_offset()

    def test_bspline_offset_clamped_closed_apply_undo(self):
        self.check_closed_spline_offset(periodic=False)

    def test_bspline_offset_closed_reversed_orientation(self):
        self.check_closed_spline_offset(reversed_source=True)

    def test_bspline_offset_self_crossing_source_has_no_side(self):
        self.start_spline_offset(self_crossing=True)
        self.spline_quantity("Distance", 0.1)
        before = self.offset_snapshot()
        for action in ("Update", "Apply"):
            self.spline_button(action)
            message = self.spline_control(QtGui.QLabel, "Status").text()
            self.assertIn("Source", message)
            self.assertIn("crossing", message)
            side = self.spline_control(QtGui.QLabel, "Side").text()
            self.assertNotIn("Side: Outside", side)
            self.assertNotIn("Side: Inside", side)
            self.assertEqual(before, self.offset_snapshot())
        self.spline_button("Cancel")
        self.assertEqual(before, self.offset_snapshot())
        self.assertEqual(self.selection_before_offset, self.selection_signature())

    def test_bspline_offset_closed_inversion_preserves_history(self):
        self.start_spline_offset(smooth_closed=True)
        self.spline_quantity("Distance", -20.0)
        before = self.offset_snapshot()
        self.spline_button("Apply")
        self.assertIn("singular", self.spline_control(QtGui.QLabel, "Status").text().lower())
        self.assertEqual(before, self.offset_snapshot())
        self.spline_button("Cancel")
        self.assertEqual(before, self.offset_snapshot())

    def test_bspline_offset_mixed_edges_rejected_before_filtering(self):
        self.start_spline_offset()
        self.spline_button("Cancel")
        self.sketch.addGeometry(_line((0, -5), (10, -5)))
        self.recompute_gui_document()
        before = self.offset_snapshot()
        self.select_offset_edges(["Edge1", "Edge2"])
        self.assertIsNone(self.spline_control(QtGui.QWidget, "Panel"))
        self.assertIsNone(
            self.offset_mode_combobox(), "Mixed edges must not start the analytic handler"
        )
        self.assertEqual(before, self.offset_snapshot())

    def test_bspline_offset_external_edge_rejected(self):
        self.start_spline_offset()
        self.spline_button("Cancel")
        support = self.doc.addObject("Part::Feature", "Support")
        support.Shape = self.sketch.Geometry[0].toShape()
        self.recompute_gui_document()
        self.sketch.addExternal(support.Name, "Edge1")
        self.recompute_gui_document()
        before = self.offset_snapshot()
        for edges in (["ExternalEdge1"], ["Edge1", "ExternalEdge1"]):
            self.select_offset_edges(edges)
            self.assertIsNone(self.spline_control(QtGui.QWidget, "Panel"))
            self.assertIsNone(self.offset_mode_combobox())
            self.assertEqual(before, self.offset_snapshot())
            self.assertEqual(self.selection_before_offset, self.selection_signature())

    def test_bspline_offset_changed_inputs_invalidate_preview(self):
        source = self.start_spline_offset()
        self.spline_button("Update")
        self.spline_quantity("Distance", -1.5)
        self.spline_quantity("Tolerance", 0.005)
        self.assertNotIn("Achieved", self.spline_control(QtGui.QLabel, "Status").text())
        self.spline_button("Apply")
        self.assert_spline_distance(source, -1.5, tolerance=0.005)

    def test_bspline_offset_changed_source_cancels(self):
        self.start_spline_offset()
        self.spline_button("Update")
        self.sketch.moveGeometry(0, 1, FreeCAD.Vector(-1, 0, 0), False)
        self.recompute_gui_document()
        before = self.offset_snapshot()
        self.spline_button("Apply")
        self.assertIsNone(self.spline_control(QtGui.QWidget, "Panel"))
        self.assertEqual(before, self.offset_snapshot())
        self.assertEqual(self.selection_before_offset, self.selection_signature())

    def test_bspline_offset_removed_source_does_not_retarget(self):
        self.start_spline_offset()
        self.spline_button("Update")
        # A second, identical spline must never become the source by reindexing.
        self.sketch.addGeometry(self.sketch.Geometry[0], False)
        self.sketch.delGeometry(0)
        self.recompute_gui_document()
        before = self.offset_snapshot()
        self.spline_button("Apply")
        self.assertIsNone(self.spline_control(QtGui.QWidget, "Panel"))
        self.assertEqual(before, self.offset_snapshot())
        self.assertEqual(self.selection_before_offset, self.selection_signature())

    def test_bspline_offset_source_survives_reindexing(self):
        self.sketch.addGeometry(_line((0, -5), (10, -5)))
        self.start_spline_offset()
        self.spline_button("Cancel")
        self.select_offset_edges(["Edge2"])
        self.spline_button("Update")
        self.sketch.delGeometry(0)
        self.recompute_gui_document()
        source = self.sketch.Geometry[0]
        self.spline_button("Apply")
        self.assert_spline_distance(source, 1.0)

    def test_bspline_offset_box_selection_with_control_polygon(self):
        self.start_spline_offset()
        self.spline_button("Cancel")
        self.sketch.exposeInternalGeometry(0)
        self.recompute_gui_document()
        self.assertGreater(self.sketch.GeometryCount, 1)
        edges = [
            "Edge" + str(i + 1)
            for i, geometry in enumerate(self.sketch.Geometry)
            if not isinstance(geometry, Part.Point)
        ]
        self.select_offset_edges(edges)
        self.assertIsNotNone(self.spline_control(QtGui.QWidget, "Panel"))
        before = self.offset_snapshot()
        self.spline_button("Update")
        self.assertIn("Achieved", self.spline_control(QtGui.QLabel, "Status").text())
        self.spline_button("Cancel")
        self.assertEqual(before, self.offset_snapshot())
        self.assertEqual(self.selection_before_offset, self.selection_signature())

    def test_bspline_offset_unrelated_internal_helpers_rejected(self):
        self.start_spline_offset()
        self.spline_button("Cancel")
        self.sketch.addGeometry(self.sketch.Geometry[0], False)
        self.sketch.exposeInternalGeometry(1)
        self.recompute_gui_document()
        before = self.offset_snapshot()
        self.select_offset_edges(["Edge1", "Edge3"])
        self.assertIsNone(self.spline_control(QtGui.QWidget, "Panel"))
        self.assertEqual(before, self.offset_snapshot())
        self.assertEqual(self.selection_before_offset, self.selection_signature())

    def test_bspline_offset_other_sketch_rejected(self):
        self.start_spline_offset()
        self.spline_button("Cancel")
        other = self.doc.addObject("Sketcher::SketchObject", "OtherSketch")
        other.addGeometry(self.sketch.Geometry[0], False)
        self.recompute_gui_document()
        FreeCADGui.Selection.clearSelection()
        FreeCADGui.Selection.addSelection(self.doc.Name, other.Name, "Edge1")
        selection = self.selection_signature()
        FreeCADGui.runCommand("Sketcher_Offset")
        self.pump(100)
        self.assertIsNone(self.spline_control(QtGui.QWidget, "Panel"))
        self.assertEqual(selection, self.selection_signature())

    def test_bspline_offset_rejection_after_success_preserves_redo(self):
        self.start_spline_offset()
        self.spline_button("Apply")
        self.assertEqual(self.sketch.GeometryCount, 2)
        self.undo_gui_document()
        self.select_offset_edges(["Edge1"])
        before = self.offset_snapshot()
        self.assertGreater(before[4], 0)
        self.spline_button("Update")
        self.assertEqual(before, self.offset_snapshot())
        self.spline_button("Cancel")
        self.assertEqual(before, self.offset_snapshot())
        self.assertEqual(self.selection_before_offset, self.selection_signature())
        self.select_offset_edges(["Edge1"])
        self.spline_quantity("Distance", 0.0)
        self.spline_button("Apply")
        self.spline_button("Cancel")
        self.assertEqual(self.selection_before_offset, self.selection_signature())
        self.assertEqual(before, self.offset_snapshot())
        self.redo_gui_document()
        self.assertEqual(self.sketch.GeometryCount, 2)
        self.assertEqual([(c.Type, c.First) for c in self.sketch.Constraints], [("Block", 1)])

    def test_bspline_offset_failed_solve_preserves_nonempty_redo(self):
        self.start_spline_offset()
        self.spline_button("Cancel")
        self.sketch.addConstraint(Sketcher.Constraint("DistanceX", 0, 1, 1.0))
        self.sketch.addConstraint(Sketcher.Constraint("DistanceX", 0, 1, 2.0))
        self.recompute_gui_document()
        self.assertNotEqual(self.sketch.solve(), 0)
        self.doc.openTransaction("Redo sentinel")
        self.doc.addObject("App::FeaturePython", "RedoSentinel")
        self.doc.commitTransaction()
        self.undo_gui_document()
        self.select_offset_edges(["Edge1"])
        before = self.offset_snapshot()
        self.assertGreater(before[4], 0)
        self.spline_button("Apply")
        self.assertIn("solve", self.spline_control(QtGui.QLabel, "Status").text().lower())
        self.assertEqual(before, self.offset_snapshot())
        self.spline_button("Cancel")
        self.assertEqual(self.selection_before_offset, self.selection_signature())
        self.assertEqual(before, self.offset_snapshot())
