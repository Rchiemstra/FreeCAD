# SPDX-License-Identifier: LGPL-2.1-or-later

import math
import time
import unittest

import FreeCAD
import FreeCADGui
import Part
import Sketcher
import SketcherGui
from PySide6 import QtCore, QtWidgets


class SketcherGuiTestCases(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if not FreeCAD.GuiUp:
            raise unittest.SkipTest("Cannot run GUI tests in a CLI environment.")

        FreeCADGui.getMainWindow().show()
        cls.pump_gui_events()

    @staticmethod
    def pump_gui_events(iterations=6, delay=0.01):
        app = QtWidgets.QApplication.instance()
        for _ in range(iterations):
            app.processEvents(QtCore.QEventLoop.AllEvents, int(delay * 1000))
            if delay > 0.0:
                time.sleep(delay)

    def wait_until(self, predicate, timeout_ms=2000, step_ms=50):
        remaining = timeout_ms
        while remaining > 0:
            if predicate():
                return True
            self.pump_gui_events(iterations=1, delay=step_ms / 1000.0)
            remaining -= step_ms
        return predicate()

    @staticmethod
    def build_issue_25840_sketch(sketch):
        # Mirrors the uploaded repro geometry from issue #25840.
        first_line = sketch.addGeometry(
            Part.LineSegment(
                FreeCAD.Vector(-17.60407066, 31.05172348, 0.0),
                FreeCAD.Vector(44.00962448, -33.86270142, 0.0),
            ),
            False,
        )
        second_line = sketch.addGeometry(
            Part.LineSegment(
                FreeCAD.Vector(50.4888, 6.2351, 0.0),
                FreeCAD.Vector(30.973626440922192, -20.12834717, 0.0),
            ),
            False,
        )
        constraint_id = sketch.addConstraint(
            Sketcher.Constraint("PointOnObject", second_line, 2, first_line)
        )
        return constraint_id, FreeCAD.Vector(30.973626440922192, -20.12834717, 0.0)

    @staticmethod
    def classify_preselection(info, expected_constraint_name):
        if not info or not info["ObjectName"]:
            return "none"

        names = info.get("SubElementNames") or []
        if expected_constraint_name in names:
            return "target_constraint"
        if any(name.startswith("Constraint") for name in names):
            return "other_constraint"
        if any(name.startswith("Vertex") for name in names):
            return "vertex"
        if any(name.startswith("Edge") for name in names):
            return "edge"
        if any(name.endswith("_Axis") for name in names):
            return "axis"
        return "other"

    @classmethod
    def scan_preselection_at_viewport(cls, center_coin, expected_constraint_name, span=16, step=2):
        counts = {
            "target_constraint": 0,
            "other_constraint": 0,
            "edge": 0,
            "vertex": 0,
            "other": 0,
            "none": 0,
        }

        for dy in range(-span, span + 1, step):
            for dx in range(-span, span + 1, step):
                coin_point = (center_coin[0] + dx, center_coin[1] + dy)
                info = SketcherGui.getActiveSketchPreselection(coin_point)
                kind = cls.classify_preselection(info, expected_constraint_name)
                counts[kind] += 1

        return counts

    @classmethod
    def find_constraint_probe_viewport_point(
        cls,
        view,
        seed_world_point,
        expected_constraint_name,
        span=64,
        step=8,
    ):
        center_coin = tuple(int(value) for value in view.getPointOnViewport(seed_world_point))
        if center_coin == (0, 0):
            return None

        sum_x = 0
        sum_y = 0
        target_count = 0

        for dy in range(-span, span + 1, step):
            for dx in range(-span, span + 1, step):
                coin_point = (center_coin[0] + dx, center_coin[1] + dy)
                info = SketcherGui.getActiveSketchPreselection(coin_point)
                if cls.classify_preselection(info, expected_constraint_name) != "target_constraint":
                    continue

                sum_x += coin_point[0]
                sum_y += coin_point[1]
                target_count += 1

        if target_count == 0:
            return None

        return (
            int(round(sum_x / target_count)),
            int(round(sum_y / target_count)),
        )

    @staticmethod
    def _datum_annotation_probe_points(projected_coin):
        if projected_coin == (0, 0):
            return []
        x, y = projected_coin
        return [
            projected_coin,
            (x - 1, y),
            (x + 1, y),
            (x, y - 1),
            (x, y + 1),
        ]

    def wait_for_datum_annotation_at_coin(
        self,
        view,
        projected_coin,
        expected_constraint_name,
        timeout_ms=2000,
    ):
        """Redraw without changing the camera, then wait for datum text at the overlap pixel.

        Polls only the projected overlap pixel (optional ±1 px rounding). Requires
        ConstraintKind == DatumAnnotation so dimension-line hits cannot pass.
        """
        found = {"coin": None}

        def datum_annotation_is_pickable():
            view.redraw()
            for coin_point in self._datum_annotation_probe_points(projected_coin):
                info = SketcherGui.getActiveSketchPreselection(coin_point)
                if info is None:
                    continue
                if self.classify_preselection(info, expected_constraint_name) != "target_constraint":
                    continue
                if info.get("ConstraintKind") != "DatumAnnotation":
                    continue
                found["coin"] = coin_point
                return True
            return False

        view.redraw()
        self.wait_until(datum_annotation_is_pickable, timeout_ms=timeout_ms)
        return found["coin"]

    @classmethod
    def configure_view_state(cls, view, tilt=None):
        view.viewTop()
        cls.pump_gui_events()
        view.fitAll()
        cls.pump_gui_events()

        if tilt is not None:
            base_rotation = view.getCameraOrientation()
            view.setCameraOrientation(tilt.multiply(base_rotation))
            cls.pump_gui_events()
            view.fitAll()
            cls.pump_gui_events()

    @staticmethod
    def constraint_share(counts):
        total_hits = (
            counts["target_constraint"]
            + counts["other_constraint"]
            + counts["edge"]
            + counts["vertex"]
            + counts["other"]
        )
        if total_hits == 0:
            return 0.0
        return counts["target_constraint"] / total_hits

    def setUp(self):
        self.doc = FreeCAD.newDocument("SketchGuiTest")
        self.sketch = self.doc.addObject("Sketcher::SketchObject", "Sketch")
        self.doc.recompute()

        FreeCADGui.getMainWindow().show()
        self.pump_gui_events()
        self.view = FreeCADGui.ActiveDocument.ActiveView
        if self.view:
            # Realize a non-zero viewport before setEdit. Woodpecker
            # TestSketcherGui SIGSEGV'd on first sketch edit (pipelines 354/357).
            for _ in range(25):
                try:
                    size = self.view.getSize()
                    if size and size[0] > 0 and size[1] > 0:
                        break
                except Exception:
                    pass
                self.pump_gui_events(iterations=2, delay=0.02)
            self.view.viewTop()
            self.pump_gui_events()
        print("SketchGuiTest: setEdit begin", flush=True)
        FreeCADGui.ActiveDocument.setEdit(self.sketch.Name)
        print("SketchGuiTest: setEdit done", flush=True)
        self.pump_gui_events()

        self.view = FreeCADGui.ActiveDocument.ActiveView
        if self.view:
            self.view.viewTop()
            self.view.fitAll()
            self.pump_gui_events()

    def _restore_finite_camera_height(self, view):
        """Cleanup-only: do not call this to make a recovery or projection assertion pass.

        Production Inf-camera recovery lives in ViewProviderSketch::setEditViewer.
        This helper exists so a poisoned ortho height cannot leak into later
        classes (WP363 Offset after Inf-camera). It is fixture teardown, not proof.
        """
        camera = view.getCameraNode()
        if camera is None or not hasattr(camera, "height"):
            return
        height = float(camera.height.getValue())
        if not math.isfinite(height) or height <= 0.0:
            camera.height.setValue(200.0)

    def _ortho_camera_height(self, view):
        camera = view.getCameraNode()
        if camera is None or not hasattr(camera, "height"):
            return None
        return float(camera.height.getValue())

    def _preselection_matches(self, info, wanted, object_name=None):
        if object_name is not None and (not info or info.get("ObjectName") != object_name):
            return False
        return self.classify_preselection(info, "Constraint0") == wanted

    def _find_preselection_near_viewport(
        self,
        seed_coin,
        wanted,
        object_name=None,
        span=16,
        step=2,
    ):
        """Bounded preselection search around an already-projected seed pixel.

        The seed must be the requested world point's projection, not the
        viewport centre and not a substitute for a (0, 0) sentinel. The hit
        must keep the intended object and subelement kind.
        """
        if seed_coin == (0, 0):
            return None
        for dy in range(-span, span + 1, step):
            for dx in range(-span, span + 1, step):
                point = (seed_coin[0] + dx, seed_coin[1] + dy)
                info = SketcherGui.getActiveSketchPreselection(point)
                if self._preselection_matches(info, wanted, object_name):
                    return point
        return None

    def project_world_to_viewport(self, view, world_point, attempts=8):
        """Project the requested world point to Coin pixels.

        (0, 0) is the C++ failure sentinel. This helper does not restore camera
        height, does not call viewTop/fitAll, and does not scan for a nearby
        edge. An arbitrary edge near the viewport centre is not this point.
        """
        last = (0, 0)
        for _ in range(attempts):
            last = tuple(int(value) for value in view.getPointOnViewport(world_point))
            if last != (0, 0):
                return last
            self.pump_gui_events(iterations=8, delay=0.02)
        return last

    def tearDown(self):
        try:
            if getattr(self, "view", None):
                # Cleanup after the test body, not part of the recovery proof.
                self._restore_finite_camera_height(self.view)
        except Exception:
            pass
        FreeCADGui.Selection.clearPreselection()
        FreeCADGui.Selection.clearSelection()
        if FreeCADGui.ActiveDocument:
            FreeCADGui.ActiveDocument.resetEdit()
        self.pump_gui_events()

        if self.doc is not None:
            document_name = self.doc.Name
            self.doc = None
            FreeCAD.closeDocument(document_name)
            self.pump_gui_events()

    def testPointOnObjectPreselectionMatchesTiltedHitArea(self):
        constraint_id, self.probe_point = self.build_issue_25840_sketch(self.sketch)
        self.expected_constraint_name = f"Constraint{constraint_id + 1}"
        self.doc.recompute()
        self.pump_gui_events()

        tilt_y = FreeCAD.Rotation(FreeCAD.Vector(0, 1, 0), 2.0)

        counts_by_state = {}
        for name, tilt in {
            "exact_top": None,
            "tilt_y_2deg": tilt_y,
        }.items():
            self.configure_view_state(self.view, tilt)
            probe_point = self.find_constraint_probe_viewport_point(
                self.view,
                self.probe_point,
                self.expected_constraint_name,
            )
            self.assertIsNotNone(probe_point)
            counts_by_state[name] = self.scan_preselection_at_viewport(
                probe_point,
                self.expected_constraint_name,
                span=12,
            )

        exact_top = counts_by_state["exact_top"]
        tilt_y = counts_by_state["tilt_y_2deg"]
        max_tilt_hits = tilt_y["target_constraint"]
        exact_top_share = self.constraint_share(exact_top)

        detail = (
            f"exact_top={exact_top}, tilt_y_2deg={tilt_y}, "
            f"constraint_share={exact_top_share:.3f}"
        )

        self.assertGreater(max_tilt_hits, 0, detail)
        self.assertGreater(exact_top["target_constraint"], 0, detail)
        self.assertGreaterEqual(
            exact_top["target_constraint"],
            int(max_tilt_hits * 0.8),
            detail,
        )
        self.assertGreaterEqual(
            exact_top_share,
            0.20,
            detail,
        )

    def testPointMarkerWinsOverOverlappingConstraintLabel(self):
        start_point = FreeCAD.Vector(80.0, 100.0, 0.0)
        end_point = FreeCAD.Vector(120.0, 140.0, 0.0)
        marker_point = FreeCAD.Vector(92.0, 88.0, 0.0)

        line_id = self.sketch.addGeometry(
            Part.LineSegment(start_point, end_point),
            False,
        )
        self.sketch.addGeometry(Part.Point(marker_point), False)
        self.doc.recompute()
        self.pump_gui_events()

        self.configure_view_state(self.view)
        marker_coin = self.project_world_to_viewport(self.view, marker_point)
        self.assertNotEqual(
            marker_coin,
            (0, 0),
            f"expected a usable projection of {marker_point}, not the (0, 0) sentinel",
        )

        vertex_offsets = []
        for dy in range(-12, 13, 2):
            for dx in range(-12, 13, 2):
                probe_coin = (marker_coin[0] + dx, marker_coin[1] + dy)
                probe_info = SketcherGui.getActiveSketchPreselection(probe_coin)
                probe_kind = self.classify_preselection(probe_info, "Constraint0")
                if probe_kind == "vertex":
                    vertex_offsets.append((dx, dy))

        constraint_id = self.sketch.addConstraint(
            Sketcher.Constraint("Distance", line_id, 1, line_id, 2, 40.0)
        )
        self.sketch.setLabelDistance(constraint_id, -12.0 * math.sqrt(2.0))
        self.sketch.setLabelPosition(constraint_id, 0.0)
        self.expected_constraint_name = f"Constraint{constraint_id + 1}"
        self.doc.recompute()
        self.pump_gui_events()

        marker_info = SketcherGui.getActiveSketchPreselection(marker_coin)
        marker_kind = self.classify_preselection(marker_info, self.expected_constraint_name)

        probe_results = []
        for dx, dy in vertex_offsets:
            probe_coin = (marker_coin[0] + dx, marker_coin[1] + dy)
            probe_info = SketcherGui.getActiveSketchPreselection(probe_coin)
            probe_kind = self.classify_preselection(probe_info, self.expected_constraint_name)
            probe_results.append((dx, dy, probe_kind, probe_info))

        unexpected_probe_results = [result for result in probe_results if result[2] != "vertex"]

        detail = (
            f"marker_info={marker_info}, vertex_offsets={vertex_offsets}, "
            f"probe_results={probe_results}, marker_coin={marker_coin}"
        )

        self.assertGreater(len(vertex_offsets), 0, detail)
        self.assertEqual(marker_kind, "vertex", detail)
        self.assertEqual(unexpected_probe_results, [], detail)

    def testCurveWinsOverOverlappingDistanceDimensionLine(self):
        start_point = FreeCAD.Vector(80.0, 100.0, 0.0)
        end_point = FreeCAD.Vector(130.0, 100.0, 0.0)
        midpoint = (start_point + end_point) * 0.5

        line_id = self.sketch.addGeometry(
            Part.LineSegment(start_point, end_point),
            False,
        )
        self.doc.recompute()
        self.pump_gui_events()

        self.configure_view_state(self.view)
        midpoint_coin = self.project_world_to_viewport(self.view, midpoint)
        self.assertNotEqual(
            midpoint_coin,
            (0, 0),
            f"expected a usable projection of {midpoint}, not the (0, 0) sentinel",
        )

        edge_offsets = []
        for dy in range(-10, 11, 2):
            for dx in range(-16, 17, 2):
                probe_coin = (midpoint_coin[0] + dx, midpoint_coin[1] + dy)
                probe_info = SketcherGui.getActiveSketchPreselection(probe_coin)
                probe_kind = self.classify_preselection(probe_info, "Constraint0")
                if probe_kind == "edge":
                    edge_offsets.append((dx, dy))

        constraint_id = self.sketch.addConstraint(
            Sketcher.Constraint(
                "Distance",
                line_id,
                1,
                line_id,
                2,
                start_point.distanceToPoint(end_point),
            )
        )
        self.sketch.setLabelDistance(constraint_id, 0.0)
        self.sketch.setLabelPosition(constraint_id, 12.0)
        self.expected_constraint_name = f"Constraint{constraint_id + 1}"
        self.doc.recompute()
        self.pump_gui_events()

        probe_results = []
        for dx, dy in edge_offsets:
            probe_coin = (midpoint_coin[0] + dx, midpoint_coin[1] + dy)
            probe_info = SketcherGui.getActiveSketchPreselection(probe_coin)
            probe_kind = self.classify_preselection(probe_info, self.expected_constraint_name)
            probe_results.append((dx, dy, probe_kind, probe_info))

        unexpected_probe_results = [result for result in probe_results if result[2] != "edge"]

        detail = (
            f"edge_offsets={edge_offsets}, probe_results={probe_results}, "
            f"midpoint_coin={midpoint_coin}"
        )

        self.assertGreater(len(edge_offsets), 0, detail)
        self.assertEqual(unexpected_probe_results, [], detail)

    def testDistanceDatumTextWinsOverOverlappingCurve(self):
        start_point = FreeCAD.Vector(80.0, 100.0, 0.0)
        end_point = FreeCAD.Vector(130.0, 100.0, 0.0)
        midpoint = (start_point + end_point) * 0.5

        line_id = self.sketch.addGeometry(
            Part.LineSegment(start_point, end_point),
            False,
        )
        self.doc.recompute()
        self.pump_gui_events()

        self.configure_view_state(self.view)
        midpoint_coin = self.project_world_to_viewport(self.view, midpoint)
        self.assertNotEqual(
            midpoint_coin,
            (0, 0),
            f"expected a usable projection of {midpoint}, not the (0, 0) sentinel",
        )

        before_info = SketcherGui.getActiveSketchPreselection(midpoint_coin)
        before_kind = self.classify_preselection(before_info, "Constraint0")

        constraint_id = self.sketch.addConstraint(
            Sketcher.Constraint(
                "Distance",
                line_id,
                1,
                line_id,
                2,
                start_point.distanceToPoint(end_point),
            )
        )
        self.sketch.setLabelDistance(constraint_id, 0.0)
        self.sketch.setLabelPosition(constraint_id, 0.0)
        self.expected_constraint_name = f"Constraint{constraint_id + 1}"
        self.doc.recompute()
        self.pump_gui_events()

        text_coin = self.wait_for_datum_annotation_at_coin(
            self.view,
            midpoint_coin,
            self.expected_constraint_name,
        )
        after_info = (
            SketcherGui.getActiveSketchPreselection(text_coin) if text_coin is not None else None
        )
        after_kind = self.classify_preselection(after_info, self.expected_constraint_name)

        detail = (
            f"before_info={before_info}, after_info={after_info}, "
            f"midpoint_coin={midpoint_coin}, text_coin={text_coin}"
        )

        self.assertEqual(before_kind, "edge", detail)
        self.assertIsNotNone(text_coin, detail)
        self.assertEqual(after_kind, "target_constraint", detail)
        self.assertEqual((after_info or {}).get("ConstraintKind"), "DatumAnnotation", detail)

    def testAngleDatumTextWinsOverHorizontalAxis(self):
        first_line = self.sketch.addGeometry(
            Part.LineSegment(
                FreeCAD.Vector(0.0, 0.0, 0.0),
                FreeCAD.Vector(50.0, 50.0 * math.tan(math.radians(30.0)), 0.0),
            ),
            False,
        )
        second_line = self.sketch.addGeometry(
            Part.LineSegment(
                FreeCAD.Vector(0.0, 0.0, 0.0),
                FreeCAD.Vector(50.0, -50.0 * math.tan(math.radians(30.0)), 0.0),
            ),
            False,
        )
        self.doc.recompute()
        self.pump_gui_events()

        self.configure_view_state(self.view)

        # The angle bisector is the horizontal axis, so the label text will be centered at
        # x=20, y=0.
        text_center = FreeCAD.Vector(20.0, 0.0, 0.0)
        text_center_coin = self.project_world_to_viewport(self.view, text_center)
        self.assertNotEqual(
            text_center_coin,
            (0, 0),
            f"expected a usable projection of {text_center}, not the (0, 0) sentinel",
        )
        before_info = SketcherGui.getActiveSketchPreselection(text_center_coin)
        before_kind = self.classify_preselection(before_info, "Constraint0")

        constraint_id = self.sketch.addConstraint(
            Sketcher.Constraint(
                "Angle",
                first_line,
                1,
                second_line,
                1,
                math.radians(-60.0),
            )
        )
        self.sketch.setLabelDistance(constraint_id, 10.0)
        self.expected_constraint_name = f"Constraint{constraint_id + 1}"
        self.doc.recompute()
        self.pump_gui_events()

        text_coin = self.wait_for_datum_annotation_at_coin(
            self.view,
            text_center_coin,
            self.expected_constraint_name,
        )
        info = SketcherGui.getActiveSketchPreselection(text_coin) if text_coin is not None else None
        kind = self.classify_preselection(info, self.expected_constraint_name)

        detail = (
            f"before_info={before_info}, before_kind={before_kind}, "
            f"info={info}, kind={kind}, text_center={text_center}, text_coin={text_coin}"
        )

        self.assertEqual(before_kind, "axis", detail)
        self.assertIsNotNone(text_coin, detail)
        self.assertEqual(kind, "target_constraint", detail)
        self.assertEqual((info or {}).get("ConstraintKind"), "DatumAnnotation", detail)

    def test_inf_camera_recovers_finite_pick_and_edge_preselection(self):
        start_point = FreeCAD.Vector(80.0, 100.0, 0.0)
        end_point = FreeCAD.Vector(130.0, 100.0, 0.0)
        midpoint = (start_point + end_point) * 0.5
        self.sketch.addGeometry(Part.LineSegment(start_point, end_point), False)
        self.doc.recompute()
        self.pump_gui_events()

        camera = self.view.getCameraNode()
        self.assertIsNotNone(camera)
        try:
            camera.height.setValue(float("inf"))
            # Observe Inf before the camera sensor recovers it. Do not pump
            # first — onCameraChanged now restores during an open edit.
            self.assertFalse(math.isfinite(self._ortho_camera_height(self.view)))

            # Production recovery during an already-open edit. Do not
            # viewTop/fitAll, do not helper-restore, and do not scan for an
            # edge first.
            self.pump_gui_events()
            self.view = FreeCADGui.ActiveDocument.ActiveView

            recovered_height = self._ortho_camera_height(self.view)
            self.assertIsNotNone(recovered_height)
            self.assertTrue(
                math.isfinite(recovered_height) and recovered_height > 0.0,
                recovered_height,
            )

            # Keep setEditViewer recovery: re-poison, observe Inf without
            # pumping, then resetEdit/setEdit (sensor is detached on unset).
            camera = self.view.getCameraNode()
            camera.height.setValue(float("inf"))
            self.assertFalse(math.isfinite(self._ortho_camera_height(self.view)))
            FreeCADGui.ActiveDocument.resetEdit()
            self.pump_gui_events()
            FreeCADGui.ActiveDocument.setEdit(self.sketch.Name)
            self.pump_gui_events()
            self.view = FreeCADGui.ActiveDocument.ActiveView

            recovered_height = self._ortho_camera_height(self.view)
            self.assertIsNotNone(recovered_height)
            self.assertTrue(
                math.isfinite(recovered_height) and recovered_height > 0.0,
                recovered_height,
            )

            midpoint_coin = self.project_world_to_viewport(self.view, midpoint)
            self.assertNotEqual(midpoint_coin, (0, 0), midpoint_coin)

            info = SketcherGui.getActiveSketchPreselection(midpoint_coin)
            if not self._preselection_matches(info, "edge", self.sketch.Name):
                midpoint_coin = self._find_preselection_near_viewport(
                    midpoint_coin,
                    "edge",
                    object_name=self.sketch.Name,
                )
                self.assertIsNotNone(
                    midpoint_coin,
                    f"no Edge of {self.sketch.Name} near projected midpoint",
                )
                info = SketcherGui.getActiveSketchPreselection(midpoint_coin)

            names = (info or {}).get("SubElementNames") or []
            detail = f"info={info}, midpoint_coin={midpoint_coin}, height={recovered_height}"
            self.assertEqual((info or {}).get("ObjectName"), self.sketch.Name, detail)
            self.assertTrue(any(name.startswith("Edge") for name in names), detail)
            self.assertEqual(
                self.classify_preselection(info, "Constraint0"),
                "edge",
                detail,
            )
        finally:
            try:
                self._restore_finite_camera_height(self.view)
            except Exception:
                pass
