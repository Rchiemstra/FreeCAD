# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2026 FreeCAD Project Association                        *
# *                                                                         *
# *   This file is part of the FreeCAD CAx development system.              *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU Lesser General Public License (LGPL)    *
# *   as published by the Free Software Foundation; either version 2 of     *
# *   the License, or (at your option) any later version.                   *
# *   for detail see the LICENCE text file.                                 *
# *                                                                         *
# *   FreeCAD is distributed in the hope that it will be useful,            *
# *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
# *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
# *   GNU Library General Public License for more details.                  *
# *                                                                         *
# *   You should have received a copy of the GNU Library General Public     *
# *   License along with FreeCAD; if not, write to the Free Software        *
# *   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
# *   USA                                                                   *
# *                                                                         *
# ***************************************************************************

"""Unit tests for the Draft Workbench, GUI dimension tests."""

import math
import os
import re
import tempfile
import time
import xml.etree.ElementTree as ET

from PySide import QtCore

import Draft
import DraftVecUtils
import FreeCAD as App
import FreeCADGui as Gui
from FreeCAD import Vector

from drafttests import test_base
from Test.GuiRecompute import (
    close_document,
    recompute_off_gui_thread,
    refresh_document,
    save_document_as,
)


class DraftGuiDimension(test_base.DraftTestCaseDoc):
    """GUI regressions for Draft dimensions."""

    FREECAD_NAMESPACE = "https://www.freecad.org/ns"
    OVERSHOOT_TRANSFORM_RE = re.compile(
        r"rotate\((?P<angle>[-+0-9.eE]+),(?P<rot_x>[-+0-9.eE]+),(?P<rot_y>[-+0-9.eE]+)\)\s+"
        r"translate\((?P<tx>[-+0-9.eE]+),(?P<ty>[-+0-9.eE]+)\)"
    )

    def setUp(self):
        """Set up a temporary Draft document and keep a stable name for teardown."""
        super().setUp()
        self.doc_name = self.doc.Name

    def tearDown(self):
        """Close the temporary document even if save/reopen invalidated the Python wrapper."""
        if self.doc_name in App.listDocuments():
            close_document(App.getDocument(self.doc_name))

    def _pump_draft_view(self, timeout_seconds: float = 5.0) -> None:
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            Gui.updateGui()
            QtCore.QCoreApplication.processEvents(QtCore.QEventLoop.AllEvents, 50)
            time.sleep(0.01)

    def _wait_for_draftview_symbol_overshoots(self, draft_view, expected_count, timeout_seconds=30.0):
        deadline = time.monotonic() + timeout_seconds
        while time.monotonic() < deadline:
            if len(self.extractOvershootLines(draft_view.Symbol)) == expected_count:
                return
            self._pump_draft_view(timeout_seconds=0.2)
        self.assertEqual(len(self.extractOvershootLines(draft_view.Symbol)), expected_count)

    def getAngularOvershootState(self, dimension):
        """Return the angular overshoot scenegraph state used by the regression checks."""
        vobj = dimension.ViewObject
        proxy = vobj.Proxy
        return {
            "dim_children": proxy.marksDimOvershoot.getNumChildren(),
            "ext_children": proxy.marksExtOvershoot.getNumChildren(),
            "dim_scale_start": tuple(proxy.transDimOvershoot1.scaleFactor.getValue()),
            "dim_scale_end": tuple(proxy.transDimOvershoot2.scaleFactor.getValue()),
            "ext_scale_start": tuple(proxy.transExtOvershoot1.scaleFactor.getValue()),
            "ext_scale_end": tuple(proxy.transExtOvershoot2.scaleFactor.getValue()),
            "dim_translation_start": tuple(proxy.transDimOvershoot1.translation.getValue()),
            "dim_translation_end": tuple(proxy.transDimOvershoot2.translation.getValue()),
            "ext_translation_start": tuple(proxy.transExtOvershoot1.translation.getValue()),
            "ext_translation_end": tuple(proxy.transExtOvershoot2.translation.getValue()),
            "coord_start": tuple(proxy.coord1.point.getValues()[0].getValue()),
            "coord_end": tuple(proxy.coord2.point.getValues()[0].getValue()),
            "dim_rotation_start": tuple(proxy.transDimOvershoot1.rotation.getValue().getValue()),
            "dim_rotation_end": tuple(proxy.transDimOvershoot2.rotation.getValue().getValue()),
            "ext_rotation_start": tuple(proxy.transExtOvershoot1.rotation.getValue().getValue()),
            "ext_rotation_end": tuple(proxy.transExtOvershoot2.rotation.getValue().getValue()),
        }

    def assertRotationAlmostEqual(self, actual, expected):
        """Compare Coin quaternion values with a tolerance."""
        for actual_component, expected_component in zip(actual, expected):
            self.assertAlmostEqual(actual_component, expected_component, places=6)

    def normalizeAngleDegrees(self, angle):
        """Normalize an SVG rotation angle so wraparound is easy to compare."""
        normalized = math.fmod(angle, 360.0)
        if normalized < 0:
            normalized += 360.0
        return normalized

    def angleDeltaDegrees(self, actual, expected):
        """Return the shortest distance between two rotation angles."""
        delta = abs(self.normalizeAngleDegrees(actual) - self.normalizeAngleDegrees(expected))
        return min(delta, 360.0 - delta)

    def makeAngularDimension(self):
        """Create a stable angular dimension setup shared by the GUI regressions."""
        dimension = Draft.make_angular_dimension(Vector(0, 0, 0), [20, 70], Vector(3, 1, 0))
        vobj = dimension.ViewObject
        vobj.DisplayMode = "World"
        vobj.ArrowTypeStart = "Dot"
        vobj.ArrowTypeEnd = "Dot"
        return dimension

    def getAngularSvg(self, dimension):
        """Return the Draft SVG body used by TechDraw DraftView for this dimension."""
        Gui.updateGui()
        return Draft.get_svg(dimension, direction=Vector(0, 0, 1), techdraw=True)

    def extractOvershootLines(self, svg_text):
        """Extract overshoot line transforms from Draft SVG output."""
        root = ET.fromstring(f'<svg xmlns:freecad="{self.FREECAD_NAMESPACE}">{svg_text}</svg>')
        lines = []
        for element in root.iter():
            if not element.tag.endswith("line"):
                continue
            skip_value = next(
                (
                    value
                    for key, value in element.attrib.items()
                    if key == "freecad:skip" or key == "skip" or key.endswith("}skip")
                ),
                None,
            )
            if skip_value != "1":
                continue

            x1 = float(element.attrib.get("x1", "nan"))
            y1 = float(element.attrib.get("y1", "nan"))
            x2 = float(element.attrib.get("x2", "nan"))
            y2 = float(element.attrib.get("y2", "nan"))
            if abs(x1) > 1e-9 or abs(y1) > 1e-9 or abs(y2) > 1e-9 or x2 >= 0:
                continue

            transform = element.attrib.get("transform", "")
            match = self.OVERSHOOT_TRANSFORM_RE.fullmatch(transform)
            self.assertIsNotNone(match, msg=f"unexpected overshoot transform: {transform}")

            tx = float(match.group("tx"))
            ty = float(match.group("ty"))
            rot_x = float(match.group("rot_x"))
            rot_y = float(match.group("rot_y"))
            self.assertAlmostEqual(tx, rot_x, places=6)
            self.assertAlmostEqual(ty, rot_y, places=6)

            lines.append(
                {
                    "anchor": (tx, ty),
                    "angle": self.normalizeAngleDegrees(float(match.group("angle"))),
                    "length": abs(x2),
                }
            )
        return lines

    def getExpectedAngularSvgOvershoots(self, dimension):
        """Compute the expected TechDraw overshoot lines from the angular geometry."""
        vobj = dimension.ViewObject
        proxy = vobj.Proxy
        pointratio = 0.75
        expected = []

        if vobj.DimOvershoot.Value:
            shootsize = vobj.DimOvershoot.Value / pointratio
            tangent1 = proxy.circle.tangentAt(proxy.circle.FirstParameter)
            tangent2 = proxy.circle.tangentAt(proxy.circle.LastParameter)
            if not DraftVecUtils.isNull(tangent1):
                expected.append(
                    {
                        "anchor": (float(proxy.p2.x), float(proxy.p2.y)),
                        "angle": self.normalizeAngleDegrees(
                            math.degrees(-DraftVecUtils.angle(tangent1))
                        ),
                        "length": shootsize,
                    }
                )
            if not DraftVecUtils.isNull(tangent2):
                expected.append(
                    {
                        "anchor": (float(proxy.p3.x), float(proxy.p3.y)),
                        "angle": self.normalizeAngleDegrees(
                            math.degrees(-DraftVecUtils.angle(tangent2) + math.pi)
                        ),
                        "length": shootsize,
                    }
                )

        if vobj.ExtOvershoot.Value:
            shootsize = vobj.ExtOvershoot.Value / pointratio
            ext1 = proxy.p1 - proxy.p2
            ext2 = proxy.p4 - proxy.p3
            if not DraftVecUtils.isNull(ext1):
                expected.append(
                    {
                        "anchor": (float(proxy.p2.x), float(proxy.p2.y)),
                        "angle": self.normalizeAngleDegrees(
                            math.degrees(-DraftVecUtils.angle(ext1))
                        ),
                        "length": shootsize,
                    }
                )
            if not DraftVecUtils.isNull(ext2):
                expected.append(
                    {
                        "anchor": (float(proxy.p3.x), float(proxy.p3.y)),
                        "angle": self.normalizeAngleDegrees(
                            math.degrees(-DraftVecUtils.angle(ext2))
                        ),
                        "length": shootsize,
                    }
                )

        return expected

    def assertOvershootLinesMatch(self, actual_lines, expected_lines):
        """Match SVG overshoot lines by anchor, angle, and length."""
        self.assertEqual(len(actual_lines), len(expected_lines))
        remaining = list(actual_lines)
        for expected in expected_lines:
            match_index = None
            for index, actual in enumerate(remaining):
                same_anchor = (
                    abs(actual["anchor"][0] - expected["anchor"][0]) < 1e-6
                    and abs(actual["anchor"][1] - expected["anchor"][1]) < 1e-6
                )
                same_angle = self.angleDeltaDegrees(actual["angle"], expected["angle"]) < 1e-6
                same_length = abs(actual["length"] - expected["length"]) < 1e-6
                if same_anchor and same_angle and same_length:
                    match_index = index
                    break

            self.assertIsNotNone(match_index, msg=f"missing overshoot line {expected}")
            remaining.pop(match_index)

        self.assertEqual(remaining, [])

    def assertAngularSvgOvershoots(self, dimension, expected_lines=None, svg_text=None):
        """Assert the Draft SVG path exposes the angular overshoot lines."""
        if svg_text is None:
            svg_text = self.getAngularSvg(dimension)
        if expected_lines is None:
            expected_lines = self.getExpectedAngularSvgOvershoots(dimension)
        self.assertOvershootLinesMatch(self.extractOvershootLines(svg_text), expected_lines)

    def createTechDrawDraftView(self, doc, source):
        """Create a TechDraw DraftView for the provided angular dimension."""
        page = doc.addObject("TechDraw::DrawPage")
        template = doc.addObject("TechDraw::DrawSVGTemplate")
        template.Template = (
            App.getResourceDir() + "Mod/TechDraw/Templates/ISO/A3_Landscape_blank.svg"
        )
        page.Template = template
        view = doc.addObject("TechDraw::DrawViewDraft")
        view.Source = source
        view.Direction = Vector(0, 0, 1)
        page.addView(view)
        return view

    def assertAngularOvershoots(self, dimension, expected_state=None):
        """Assert that the angular dimension view provider drew both overshoot marks."""
        Gui.updateGui()
        state = self.getAngularOvershootState(dimension)
        self.assertEqual(state["dim_children"], 3)
        self.assertEqual(state["ext_children"], 3)
        self.assertEqual(state["dim_scale_start"], (1.0, 1.0, 1.0))
        self.assertEqual(state["dim_scale_end"], (1.0, 1.0, 1.0))
        self.assertEqual(state["ext_scale_start"], (2.0, 2.0, 2.0))
        self.assertEqual(state["ext_scale_end"], (2.0, 2.0, 2.0))
        self.assertEqual(state["dim_translation_start"], state["coord_start"])
        self.assertEqual(state["dim_translation_end"], state["coord_end"])
        self.assertEqual(state["ext_translation_start"], state["coord_start"])
        self.assertEqual(state["ext_translation_end"], state["coord_end"])

        if expected_state is not None:
            self.assertEqual(state["dim_children"], expected_state["dim_children"])
            self.assertEqual(state["ext_children"], expected_state["ext_children"])
            self.assertEqual(state["dim_scale_start"], expected_state["dim_scale_start"])
            self.assertEqual(state["dim_scale_end"], expected_state["dim_scale_end"])
            self.assertEqual(state["ext_scale_start"], expected_state["ext_scale_start"])
            self.assertEqual(state["ext_scale_end"], expected_state["ext_scale_end"])
            self.assertEqual(
                state["dim_translation_start"], expected_state["dim_translation_start"]
            )
            self.assertEqual(state["dim_translation_end"], expected_state["dim_translation_end"])
            self.assertEqual(
                state["ext_translation_start"], expected_state["ext_translation_start"]
            )
            self.assertEqual(state["ext_translation_end"], expected_state["ext_translation_end"])
            self.assertRotationAlmostEqual(
                state["dim_rotation_start"], expected_state["dim_rotation_start"]
            )
            self.assertRotationAlmostEqual(
                state["dim_rotation_end"], expected_state["dim_rotation_end"]
            )
            self.assertRotationAlmostEqual(
                state["ext_rotation_start"], expected_state["ext_rotation_start"]
            )
            self.assertRotationAlmostEqual(
                state["ext_rotation_end"], expected_state["ext_rotation_end"]
            )

    def test_angular_dimension_draws_overshoots_after_restore(self):
        """Angular dimensions keep both overshoot marks after saving and reopening a document."""
        original_name = self.doc.Name
        dimension = self.makeAngularDimension()
        vobj = dimension.ViewObject
        vobj.DimOvershoot = 1
        vobj.ExtOvershoot = 2

        recompute_off_gui_thread(self.doc)
        self.assertAngularOvershoots(dimension)
        expected_state = self.getAngularOvershootState(dimension)

        temp_file = tempfile.NamedTemporaryFile(delete=False, suffix=".FCStd")
        temp_file.close()
        reopened = None
        try:
            save_document_as(self.doc, temp_file.name)
            self.doc = App.getDocument(original_name)
            reopened = App.openDocument(temp_file.name)
            App.setActiveDocument(reopened.Name)
            Gui.updateGui()

            restored = reopened.getObject(dimension.Name)
            self.assertIsNotNone(restored)
            self.assertAngularOvershoots(restored, expected_state)

            recompute_off_gui_thread(reopened)
            self.assertAngularOvershoots(restored, expected_state)
        finally:
            if reopened is not None:
                close_document(reopened)
            if original_name in App.listDocuments():
                self.doc = App.getDocument(original_name)
            os.unlink(temp_file.name)

    def test_angular_dimension_svg_overshoots_after_restore_and_in_techdraw(self):
        """Angular dimension overshoots survive restore in SVG and propagate into TechDraw DraftView."""
        original_name = self.doc.Name
        dimension = self.makeAngularDimension()
        vobj = dimension.ViewObject

        vobj.DimOvershoot = 0
        vobj.ExtOvershoot = 0
        recompute_off_gui_thread(self.doc)
        self.assertEqual(self.extractOvershootLines(self.getAngularSvg(dimension)), [])

        vobj.DimOvershoot = 1
        vobj.ExtOvershoot = 2
        recompute_off_gui_thread(self.doc)
        self._pump_draft_view()
        expected_lines = self.getExpectedAngularSvgOvershoots(dimension)
        self.assertAngularSvgOvershoots(dimension, expected_lines)

        temp_file = tempfile.NamedTemporaryFile(delete=False, suffix=".FCStd")
        temp_file.close()
        reopened = None
        try:
            save_document_as(self.doc, temp_file.name)
            self.doc = App.getDocument(original_name)
            reopened = App.openDocument(temp_file.name)
            App.setActiveDocument(reopened.Name)
            Gui.updateGui()

            restored = reopened.getObject(dimension.Name)
            self.assertIsNotNone(restored)
            self.assertAngularSvgOvershoots(restored, expected_lines)

            recompute_off_gui_thread(reopened)
            self.assertAngularSvgOvershoots(restored, expected_lines)

            view = self.createTechDrawDraftView(reopened, restored)
            recompute_off_gui_thread(reopened)
            try:
                refresh_document(reopened, timeout_seconds=30.0)
            except RuntimeError:
                self._pump_draft_view(timeout_seconds=5.0)
            self._wait_for_draftview_symbol_overshoots(view, len(expected_lines), timeout_seconds=60.0)
            self.assertAngularSvgOvershoots(restored, expected_lines, view.Symbol)
        finally:
            if reopened is not None:
                close_document(reopened)
            if original_name in App.listDocuments():
                self.doc = App.getDocument(original_name)
            os.unlink(temp_file.name)


    def test_angular_native_angle_and_presentation_does_not_write_model(self):
        with self._record_angular_cache_native_execute() as evidence:
            from Test.GuiRecompute import touch_on_owner_thread
            dimension = self.makeAngularDimension()
            dimension.ViewObject.DimOvershoot = 1
            dimension.ViewObject.ExtOvershoot = 2
            for first, last, expected in ((20, 70, 50), (350, 10, 20), (30, 30, 360)):
                dimension.FirstAngle = first
                dimension.LastAngle = last
                touch_on_owner_thread(self.doc, dimension)
                recompute_off_gui_thread(self.doc)
                self.assertAlmostEqual(dimension.Angle.Value, expected)
                self.assertEqual(dimension.getStatusString(), "Valid")
                before = dimension.Angle.Value
                # Direct main-thread presentation is a presentation-only check;
                # it is not credited as native execute coverage.
                dimension.ViewObject.Proxy.updateData(dimension, "Angle")
                self.assertEqual(dimension.Angle.Value, before)
                self.assertNotIn("Touched", dimension.State)
            dimension.Normal = Vector(0, 0, 0)
            dimension.FirstAngle = 20
            dimension.LastAngle = 70
            touch_on_owner_thread(self.doc, dimension)
            recompute_off_gui_thread(self.doc)
            self.assertAlmostEqual(dimension.Angle.Value, 50)
            self.assertAngularOvershoots(dimension)
            self.assertAngularSvgOvershoots(dimension, self.getExpectedAngularSvgOvershoots(dimension))
            self.assertTrue(any(e["native"] and e["name"] == dimension.Name
                                for e in evidence["entries"]))
            self.assertFalse(set(dimension.State) & {"Touched", "Invalid", "Recompute"})

    def test_angular_explicitly_touched_restore_native_angle(self):
        """Close before disk restore and observe the fresh native execute entry."""
        import functools
        import threading
        from draftobjects.dimension import AngularDimension
        from Test.GuiRecompute import touch_on_owner_thread

        original_name = self.doc.Name
        old_instance = self.doc.collaborationIdentity()["instance_id"]
        with tempfile.NamedTemporaryFile(suffix=".FCStd", delete=False) as file:
            path = file.name
        original_execute = AngularDimension.execute
        main_thread = threading.get_ident()
        phase = "initial"
        first_error = []
        observation_errors = []
        evidence = {"entries": [], "first_execute_error": None, "first_native_error": None}
        self._angular_restore_execute_evidence = evidence

        @functools.wraps(original_execute)
        def observed_execute(proxy, obj, *args, **kwargs):
            entry = {"phase": phase, "thread": threading.get_ident(),
                     "gui_main_thread": main_thread}
            try:
                entry.update(instance_id=obj.Document.collaborationIdentity()["instance_id"],
                             name=obj.Name, state=list(obj.State))
            except BaseException as exc:
                # Still call the original; an observation error must not hide
                # its exception. Observation failure is rethrown in finally.
                observation_errors.append((exc, exc.__traceback__))
            entry["native"] = ("Recompute" in entry.get("state", [])
                               and entry["thread"] != main_thread)
            evidence["entries"].append(entry)
            try:
                return original_execute(proxy, obj, *args, **kwargs)
            except BaseException as exc:
                error = dict(entry, exception_type=type(exc).__name__, exception_text=str(exc))
                if not first_error:
                    first_error.append((exc, exc.__traceback__))
                    evidence["first_execute_error"] = error
                if entry["native"] and evidence["first_native_error"] is None:
                    evidence["first_native_error"] = error
                raise

        # Install before Proxy assignment: native FeaturePython caches execute.
        # Delegate to root's recorder if present; never replace object.Proxy.
        AngularDimension.execute = observed_execute
        try:
            dimension = self.makeAngularDimension()
            name = dimension.Name
            dimension.ViewObject.DimOvershoot = 1
            dimension.ViewObject.ExtOvershoot = 2
            recompute_off_gui_thread(self.doc)
            self.assertAngularOvershoots(dimension)
            expected_state = self.getAngularOvershootState(dimension)
            expected_lines = self.getExpectedAngularSvgOvershoots(dimension)
            self.assertEqual(len(expected_lines), 4)
            self.assertAngularSvgOvershoots(dimension, expected_lines)

            save_document_as(self.doc, path)
            phase = "initial-close"
            close_document(self.doc)
            self.assertNotIn(original_name, App.listDocuments())
            # Do not access the retired dimension/document/view wrappers again.
            phase = "fresh-open"
            reopened = App.openDocument(path)
            self.doc = reopened
            self.doc_name = reopened.Name
            fresh_instance = reopened.collaborationIdentity()["instance_id"]
            evidence.update(initial_instance=old_instance, fresh_instance=fresh_instance,
                            target_name=name)
            self.assertNotEqual(fresh_instance, old_instance)
            App.setActiveDocument(self.doc_name)
            restored = reopened.getObject(name)
            self.assertIsNotNone(restored)
            self.assertAngularOvershoots(restored, expected_state)
            self.assertAngularSvgOvershoots(restored, expected_lines)

            phase = "fresh-touched"
            restored.FirstAngle = 350
            restored.LastAngle = 10
            touch_on_owner_thread(reopened, restored)
            recompute_off_gui_thread(reopened)
            self.assertTrue(any(entry.get("instance_id") == fresh_instance
                                and entry.get("name") == name and entry["native"]
                                and entry["phase"] == "fresh-touched"
                                for entry in evidence["entries"]),
                            "No fresh target native entry on the owner thread")
            self.assertAlmostEqual(restored.Angle.Value, 20)
            self.assertAngularOvershoots(restored)
            fresh_lines = self.getExpectedAngularSvgOvershoots(restored)
            self.assertEqual(len(fresh_lines), 4)
            self.assertAngularSvgOvershoots(restored, fresh_lines)

            phase = "fresh-techdraw"
            view = self.createTechDrawDraftView(reopened, restored)
            recompute_off_gui_thread(reopened)
            refresh_document(reopened, timeout_seconds=30.0)
            self._wait_for_draftview_symbol_overshoots(view, 4, timeout_seconds=60.0)
            self.assertAngularSvgOvershoots(restored, fresh_lines, view.Symbol)
            self.assertAngularOvershoots(restored)
            self.assertAlmostEqual(restored.Angle.Value, 20)
            targets = [restored] + [obj for obj in reopened.Objects if obj.TypeId in
                                   ("TechDraw::DrawPage", "TechDraw::DrawSVGTemplate",
                                    "TechDraw::DrawViewDraft")]
            self.assertEqual(len(targets), 4)
            for target in targets:
                self.assertEqual(target.getStatusString(), "Valid", target.Name)
                self.assertFalse(set(target.State) & {"Touched", "Invalid", "Recompute"},
                                 (target.Name, list(target.State)))
        finally:
            phase = "cleanup"
            try:
                try:
                    # Stable live name; teardown checks absence and will not
                    # double-close or inspect a retired Python wrapper.
                    if self.doc_name in App.listDocuments():
                        close_document(App.getDocument(self.doc_name))
                finally:
                    os.unlink(path)
            finally:
                AngularDimension.execute = original_execute
                if first_error:
                    exc, traceback = first_error[0]
                    raise exc.with_traceback(traceback)
                if observation_errors:
                    exc, traceback = observation_errors[0]
                    raise exc.with_traceback(traceback)

    def test_angular_subprecision_angle_keeps_reference_label(self):
        with self._record_angular_cache_native_execute() as evidence:
            from Test.GuiRecompute import touch_on_owner_thread
            from draftfunctions.svg import _derive_angular_dimension_svg_geometry
            from draftutils import utils
            dimension = self.makeAngularDimension()
            dimension.ViewObject.Decimals = utils.precision() + 2
            recompute_off_gui_thread(self.doc)
            before = dimension.Angle.Value
            dimension.LastAngle = 70 + 10 ** (-utils.precision() - 2)
            touch_on_owner_thread(self.doc, dimension)
            recompute_off_gui_thread(self.doc)
            self._pump_draft_view(timeout_seconds=0.2)
            self.assertEqual(dimension.Angle.Value, before)
            reference = _derive_angular_dimension_svg_geometry(dimension, dimension.ViewObject)
            self.assertEqual(dimension.ViewObject.Proxy.string, reference["string"])
            self.assertEqual(dimension.getStatusString(), "Valid")
            self.assertTrue(any(e["native"] and e["name"] == dimension.Name
                                for e in evidence["entries"]))
            self.assertFalse(set(dimension.State) & {"Touched", "Invalid", "Recompute"})

    def _record_angular_cache_native_execute(self):
        import threading
        from contextlib import contextmanager
        from draftobjects.dimension import AngularDimension
        @contextmanager
        def recording():
            original = AngularDimension.execute
            main_thread = threading.get_ident()
            first_error, observation_errors = [], []
            evidence = {"entries": [], "first_execute_error": None, "first_native_error": None}
            self._angular_cache_execute_evidence = evidence
            def execute(proxy, obj, *args, **kwargs):
                entry = dict(thread=threading.get_ident(), gui_main_thread=main_thread,
                             name=None, state=[], instance_id=None, native=False, error=None)
                try:
                    entry.update(name=obj.Name, state=list(obj.State),
                                 instance_id=obj.Document.collaborationIdentity()["instance_id"])
                    entry["native"] = ("Recompute" in entry["state"]
                                       and entry["thread"] != main_thread)
                except BaseException as exc:
                    observation_errors.append((exc, exc.__traceback__))
                evidence["entries"].append(entry)
                try:
                    return original(proxy, obj, *args, **kwargs)
                except BaseException as exc:
                    entry["error"] = repr(exc)
                    if not first_error:
                        first_error.append((exc, exc.__traceback__))
                        evidence["first_execute_error"] = repr(exc)
                    if entry["native"] and evidence["first_native_error"] is None:
                        evidence["first_native_error"] = repr(exc)
                    raise
            AngularDimension.execute = execute
            try:
                yield evidence
            finally:
                try:
                    if self.doc_name in App.listDocuments():
                        close_document(App.getDocument(self.doc_name))
                finally:
                    AngularDimension.execute = original
                    if first_error:
                        exc, traceback = first_error[0]
                        raise exc.with_traceback(traceback)
                    if observation_errors:
                        exc, traceback = observation_errors[0]
                        raise exc.with_traceback(traceback)
        return recording()

    def test_angular_svg_current_model_with_conditioned_stale_presentation_cache(self):
        """Deterministic cache conditioning after native execute, not a timer-race claim."""
        from Test.GuiRecompute import touch_on_owner_thread
        from draftfunctions.svg import _derive_angular_dimension_svg_geometry
        from draftfunctions.svgshapes import get_path
        import WorkingPlane
        keys = ("circle", "circle1", "circle2", "p1", "p2", "p3", "p4", "string", "tbase")
        absent = object()
        with self._record_angular_cache_native_execute() as evidence:
            dimension = self.makeAngularDimension()
            dimension.Dimline = Vector(300, 100, 0)  # Enough arc for a real Screen text gap.
            view = dimension.ViewObject
            view.DimOvershoot, view.ExtOvershoot = 1, 2
            view.Decimals, view.ShowUnit, view.Override = 4, True, "angle $dim"
            for mode in ("World", "Screen"):
                view.DisplayMode = mode
                dimension.FirstAngle, dimension.LastAngle = 20, 70
                touch_on_owner_thread(self.doc, dimension)
                recompute_off_gui_thread(self.doc)
                old = {key: getattr(view.Proxy, key, absent) for key in keys}
                old_svg = Draft.get_svg(dimension, direction=Vector(0, 0, 1), techdraw=True,
                                        color="#123456", linestyle="2,1")
                start = len(evidence["entries"])
                dimension.FirstAngle, dimension.LastAngle = 350, 10
                touch_on_owner_thread(self.doc, dimension)
                recompute_off_gui_thread(self.doc)
                self.assertTrue(any(e["native"] and e["name"] == dimension.Name
                                    for e in evidence["entries"][start:]))
                self.assertAlmostEqual(dimension.Angle.Value, 20)
                self.assertEqual(dimension.getStatusString(), "Valid")
                self.assertFalse(set(dimension.State) & {"Touched", "Invalid", "Recompute"})
                current = {key: getattr(view.Proxy, key, absent) for key in keys}
                reference = Draft.get_svg(dimension, direction=Vector(0, 0, 1), techdraw=True,
                                          color="#123456", linestyle="2,1")
                self.assertNotEqual(reference, old_svg)
                plane = WorkingPlane.PlaneBase()
                plane.align_to_point_and_axis_svg(Vector(0, 0, 0), Vector(0, 0, 1), 0)
                if mode == "Screen":
                    self.assertIsNotNone(current["circle1"])
                    self.assertIsNotNone(current["circle2"])
                    edges = [current["circle1"], current["circle2"]]
                else:
                    edges = [current["circle"]]
                reference_arc = get_path(dimension, plane, "none", [], "#123456", 0.35,
                                         "2.0,1.0", edges=edges)
                self.assertIn(reference_arc, reference)  # Independent native geometry/style reference.
                label = _derive_angular_dimension_svg_geometry(
                    dimension, view, angle=dimension.Angle.Value)["string"]
                root = ET.fromstring(f'<svg xmlns:freecad="{self.FREECAD_NAMESPACE}">{reference}</svg>')
                self.assertIn(label, "".join(root.itertext()))
                expected_lines = self.getExpectedAngularSvgOvershoots(dimension)
                self.assertEqual(len(expected_lines), 4)
                try:
                    for key, value in old.items():
                        if value is absent:
                            if hasattr(view.Proxy, key):
                                delattr(view.Proxy, key)
                        else:
                            setattr(view.Proxy, key, value)
                    # No event pump or GUI update between conditioning and export.
                    actual = Draft.get_svg(dimension, direction=Vector(0, 0, 1), techdraw=True,
                                           color="#123456", linestyle="2,1")
                    self.assertEqual(actual, reference, "SVG consumed the previous presentation cache")
                    lines = self.extractOvershootLines(actual)
                    self.assertEqual(len(lines), 4)
                    for line, expected in zip(lines, expected_lines):
                        self.assertAlmostEqual(line["length"], expected["length"], places=6)
                        self.assertLess(self.angleDeltaDegrees(line["angle"], expected["angle"]), 1e-6)
                        for value, reference_value in zip(line["anchor"], expected["anchor"]):
                            self.assertAlmostEqual(value, reference_value, places=6)
                    self.assertAlmostEqual(dimension.Angle.Value, 20)
                finally:
                    for key, value in current.items():
                        if value is absent:
                            if hasattr(view.Proxy, key):
                                delattr(view.Proxy, key)
                        else:
                            setattr(view.Proxy, key, value)
