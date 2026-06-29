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


def _line(start, end):
    return Part.LineSegment(FreeCAD.Vector(*start, 0), FreeCAD.Vector(*end, 0))


class TestSketcherOffsetGui(unittest.TestCase):
    def setUp(self):
        if not GUI_AVAILABLE:
            self.skipTest("GUI not available")

        FreeCADGui.activateWorkbench("SketcherWorkbench")
        self.doc = FreeCAD.newDocument("TestSketcherOffsetGui")
        self.sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        self.doc.recompute()

    def tearDown(self):
        if not GUI_AVAILABLE:
            return

        gui_doc = FreeCADGui.ActiveDocument
        if gui_doc is not None:
            gui_doc.resetEdit()

        if self.doc.Name in FreeCAD.listDocuments():
            FreeCAD.closeDocument(self.doc.Name)

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
        self.pump(250)
        view = FreeCADGui.ActiveDocument.ActiveView
        view.viewTop()
        FreeCADGui.SendMsgToActiveView("ViewFit")
        self.pump(150)
        return view

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
        screen_point = QtCore.QPoint(*view.getPointOnScreen(click_point))
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
        self.doc.recompute()

        return before_geometry, before_constraints

    def assert_solves_without_conflicts(self):
        status = self.sketch.solve()
        self.assertEqual(status, 0, "Expected the offset sketch to solve")

    def new_constraint_types(self, before_constraints):
        return [constraint.Type for constraint in self.sketch.Constraints[before_constraints:]]

    @unittest.skipIf(not GUI_AVAILABLE, "GUI not available")
    def test_offset_single_line_adds_geometry(self):
        self.sketch.addGeometry(_line((-10, 0), (10, 0)))
        self.doc.recompute()

        before_geometry, _ = self.run_offset_from_selection(
            ["Edge1"],
            FreeCAD.Vector(0, 5, 0),
        )

        self.assertGreater(self.sketch.GeometryCount, before_geometry)
        self.assert_solves_without_conflicts()

    @unittest.skipIf(not GUI_AVAILABLE, "GUI not available")
    def test_offset_closed_rectangle_arc_mode_adds_connected_profile(self):
        self.add_rectangle()
        self.doc.recompute()

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
        self.doc.recompute()

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
        self.doc.recompute()

        before_geometry, _ = self.run_offset_from_selection(
            ["Edge1"],
            FreeCAD.Vector(8, 0, 0),
        )

        self.assertGreater(self.sketch.GeometryCount, before_geometry)
        self.assert_solves_without_conflicts()

    @unittest.skipIf(not GUI_AVAILABLE, "GUI not available")
    def test_constrained_clearance_rectangle_adds_driving_clearance_constraints(self):
        self.add_rectangle()
        self.doc.recompute()

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
        self.doc.recompute()

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
