# SPDX-License-Identifier: LGPL-2.1-or-later
# /**************************************************************************
#                                                                           *
#    Copyright (c) 2026 The FreeCAD Project Association AISBL              *
#                                                                           *
#    This file is part of FreeCAD.                                          *
#                                                                           *
#    FreeCAD is free software: you can redistribute it and/or modify it     *
#    under the terms of the GNU Lesser General Public License as            *
#    published by the Free Software Foundation, either version 2.1 of the   *
#    License, or (at your option) any later version.                        *
#                                                                           *
#    FreeCAD is distributed in the hope that it will be useful, but         *
#    WITHOUT ANY WARRANTY; without even the implied warranty of             *
#    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU       *
#    Lesser General Public License for more details.                        *
#                                                                           *
#    You should have received a copy of the GNU Lesser General Public       *
#    License along with FreeCAD. If not, see                                 *
#    <https://www.gnu.org/licenses/>.                                       *
#                                                                           *
# **************************************************************************/

"""Happy-path and sad-path tests for the reported review-note issues.

These tests assert the *desired* (post-fix) behavior for each reported issue.
Against the current (unfixed) code the C++-dependent happy-path tests are
expected to FAIL -- that is the TDD-red demonstration of the bug. Sad-path tests
assert graceful handling of invalid/broken inputs and should pass today.

Issues covered (see the review-notes bug list):
  1+8  single click "holds"/grabs the note
  2    Label should be `review_note_N`, not the note text
  3+4+5 frame border / leader line missing and no thickness
  6    note is 1-to-many / supports multiple lines
  7+9+10 miss-click note; @link click broken / stuck / glitchy
"""

import os
import tempfile
import unittest

import FreeCAD as App
import Part

import CommandReviewNote
import JointObject
import UtilsAssembly


def _msg(text, end="\n"):
    App.Console.PrintMessage(text + end)


class _FakeSel:
    def __init__(self, obj, subs=None, picks=None):
        self.Object = obj
        self.SubElementNames = list(subs or [])
        self.PickedPoints = list(picks or [])


class TestReviewNotesIssues(unittest.TestCase):
    """Headless happy/sad path tests for the reported review-note issues."""

    def setUp(self):
        doc_name = self.__class__.__name__
        if App.ActiveDocument:
            if App.ActiveDocument.Name != doc_name:
                App.newDocument(doc_name)
        else:
            App.newDocument(doc_name)
        App.setActiveDocument(doc_name)
        self.doc = App.ActiveDocument

        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.jointgroup = self.assembly.newObject("Assembly::JointGroup", "Joints")
        self.box = self.assembly.newObject("Part::Box", "Box")
        self.box.Length = 10
        self.box.Width = 20
        self.box.Height = 30
        self.doc.recompute()
        _msg("  Temporary document '{}'".format(self.doc.Name))

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def _make_note(self, text_lines, sub="Face6", pick=None):
        if pick is None:
            pick = App.Vector(5, 10, 30)
        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, sub, picked_point=pick
        )
        return CommandReviewNote.create_review_note(
            self.assembly, data, list(text_lines), open_transaction=False
        )

    # --- issue 2: Label should be review_note_N, not the text -----------------

    def test_label_is_review_note_N_not_text(self):
        operation = "Label is review_note_N, not the note text"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["Hello world"])
        self.assertIsNotNone(note, operation)
        self.assertEqual(
            note.Label, "review_note_1", "{}: got {!r}".format(operation, note.Label)
        )
        self.assertNotEqual(note.Label, "Hello world", operation)

    def test_label_sequence_increments_per_note(self):
        operation = "Label sequence increments per note in the group"
        _msg("  Test '{}'".format(operation))
        labels = []
        for i in range(3):
            note = self._make_note(["Note {}".format(i)])
            labels.append(note.Label)
        self.assertEqual(labels, ["review_note_1", "review_note_2", "review_note_3"], operation)

    def test_label_unchanged_after_text_edit(self):
        operation = "Label unchanged after text edit"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["First"])
        before = note.Label
        self.assertEqual(before, "review_note_1", operation)
        note.LabelText = ["Completely different text"]
        self.assertEqual(note.Label, before, "{}: Label changed to {!r}".format(operation, note.Label))

    def test_whitespace_text_does_not_blank_label(self):
        operation = "Whitespace-only LabelText does not blank the review_note_N label"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["Keep"])
        self.assertEqual(note.Label, "review_note_1", operation)
        note.LabelText = ["   ", "\t", ""]
        self.assertEqual(
            note.Label, "review_note_1", "{}: got {!r}".format(operation, note.Label)
        )

    # --- issue 6: note is 1-to-many / supports multiple lines ------------------

    def test_note_supports_multiple_lines(self):
        operation = "Note supports multiple LabelText lines"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["Line one", "Line two", "Line three"])
        self.assertIsNotNone(note, operation)
        self.assertEqual(len(list(note.LabelText)), 3, operation)
        self.assertEqual(list(note.LabelText), ["Line one", "Line two", "Line three"], operation)

    def test_note_supports_multiple_at_refs(self):
        operation = "One note parses multiple @refs (1-to-many)"
        _msg("  Test '{}'".format(operation))
        text = "See @Box.Face6 and @Box and also @Box.Edge1"
        refs = CommandReviewNote.parse_review_note_references([text])
        self.assertEqual(len(refs), 3, operation)
        names = [(r["obj_name"], r["sub_name"]) for r in refs]
        self.assertIn(("Box", "Face6"), names, operation)
        self.assertIn(("Box", ""), names, operation)
        self.assertIn(("Box", "Edge1"), names, operation)

    def test_multiline_labeltext_persists_save_reload(self):
        operation = "Multi-line LabelText persists across save/reload"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["Persist A", "Persist B", "Persist C"])
        self.assertEqual(len(list(note.LabelText)), 3, operation)
        fd, path = tempfile.mkstemp(suffix=".FCStd")
        os.close(fd)
        try:
            self.doc.saveAs(path)
            App.closeDocument(self.doc.Name)
            loaded = App.openDocument(path)
            self.doc = loaded
            App.setActiveDocument(loaded.Name)
            notes = [o for o in loaded.Objects if o.TypeId == "Assembly::ReviewNote"]
            self.assertEqual(len(notes), 1, operation)
            self.assertEqual(
                list(notes[0].LabelText), ["Persist A", "Persist B", "Persist C"], operation
            )
        finally:
            if App.ActiveDocument:
                App.closeDocument(App.ActiveDocument.Name)
            self.doc = App.newDocument(self.__class__.__name__)
            App.setActiveDocument(self.doc.Name)
            try:
                os.remove(path)
            except OSError:
                pass

    # --- issues 9+10: @link resolve / select sad paths ------------------------

    def test_resolve_reference_missing_returns_none(self):
        operation = "Resolve @ref for a missing object returns (None, sub)"
        _msg("  Test '{}'".format(operation))
        obj, sub = CommandReviewNote.resolve_review_note_reference(
            self.doc, "NoSuchObject", "Face1"
        )
        self.assertIsNone(obj, operation)
        self.assertEqual(sub, "Face1", operation)

    def test_select_reference_missing_returns_false_no_throw(self):
        operation = "Select @ref for a missing object returns False without throwing"
        _msg("  Test '{}'".format(operation))
        ok = CommandReviewNote.select_review_note_reference(
            self.doc, "NoSuchObject", "Face1"
        )
        self.assertFalse(ok, operation)

    def test_parse_references_ignores_malformed_at_tokens(self):
        operation = "parse_review_note_references ignores malformed @ tokens"
        _msg("  Test '{}'".format(operation))
        refs = CommandReviewNote.parse_review_note_references(
            ["bad @123 and @ and @.dot and @ok_end"]
        )
        # @123 (digit start), @ alone, @.dot are not valid; @ok_end is valid.
        names = [r["obj_name"] for r in refs]
        self.assertNotIn("123", names, operation)
        self.assertIn("ok_end", names, operation)

    # --- issues 5+10: leader glue / stuck / inflated detectors ----------------

    def test_leader_glue_status_rejects_collapsed_end(self):
        operation = "Leader glue status rejects a collapsed (stuck) endpoint"
        _msg("  Test '{}'".format(operation))
        text = App.Vector(10, 10, 0)
        ok, reason = CommandReviewNote.review_note_leader_glue_status(
            text, App.Vector(10, 10, 0), App.Vector(2, 1, 0)
        )
        self.assertFalse(ok, "{}: {}".format(operation, reason))

    def test_leader_glue_status_rejects_detached_end(self):
        operation = "Leader glue status rejects a detached endpoint"
        _msg("  Test '{}'".format(operation))
        text = App.Vector(10, 10, 0)
        ok, reason = CommandReviewNote.review_note_leader_glue_status(
            text, App.Vector(100, 100, 0), App.Vector(2, 1, 0)
        )
        self.assertFalse(ok, "{}: {}".format(operation, reason))

    def test_leader_glue_status_rejects_inflated_half_extent(self):
        operation = "Leader glue status rejects an inflated half-extent (no thickness bug)"
        _msg("  Test '{}'".format(operation))
        text = App.Vector(10, 10, 0)
        ok, reason = CommandReviewNote.review_note_leader_glue_status(
            text, App.Vector(11, 11, 0), App.Vector(50, 50, 0)
        )
        self.assertFalse(ok, "{}: {}".format(operation, reason))

    def test_leader_stuck_after_move_detector_flags_known_bad_frame(self):
        operation = "Stuck-after-move detector flags the known-bad log frame"
        _msg("  Test '{}'".format(operation))
        self.assertTrue(
            CommandReviewNote.review_note_log_seq_is_stale_pattern(758),
            "{}: seq 758 must be detected as a stale/stuck leader".format(operation),
        )
        self.assertTrue(
            CommandReviewNote.review_note_log_seq_is_stale_pattern(793),
            "{}: seq 793 must be detected as stuck on previous text".format(operation),
        )

    def test_leader_stuck_after_move_ignores_no_move(self):
        operation = "Stuck-after-move detector ignores a no-move (click, not drag)"
        _msg("  Test '{}'".format(operation))
        end = App.Vector(8, 8, 0)
        text = App.Vector(10, 10, 0)
        # No move of text -> not stuck (a single click must not be flagged as a stuck drag).
        self.assertFalse(
            CommandReviewNote.review_note_leader_is_stuck_after_move(end, text, end),
            operation,
        )


class TestReviewNotesGuiIssues(unittest.TestCase):
    """GUI happy/sad path tests for the reported review-note issues (xvfb)."""

    @classmethod
    def setUpClass(cls):
        if not App.GuiUp:
            raise unittest.SkipTest(
                "GUI review-note issue tests require FreeCAD GUI mode (xvfb-run with FreeCADGui)"
            )

    def setUp(self):
        import FreeCADGui as Gui

        doc_name = self.__class__.__name__
        if App.ActiveDocument and App.ActiveDocument.Name != doc_name:
            App.closeDocument(App.ActiveDocument.Name)
        if not App.ActiveDocument or App.ActiveDocument.Name != doc_name:
            App.newDocument(doc_name)
        App.setActiveDocument(doc_name)
        self.doc = App.ActiveDocument
        Gui.activateWorkbench("AssemblyWorkbench")

        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.jointgroup = self.assembly.newObject("Assembly::JointGroup", "Joints")
        self.box = self.assembly.newObject("Part::Box", "Box")
        self.box.Length = 10
        self.box.Width = 20
        self.box.Height = 30
        self.doc.recompute()

        activated = False
        if self.assembly.ViewObject:
            try:
                Gui.ActiveDocument.setEdit(self.assembly)
                activated = bool(self.assembly.ViewObject.isInEditMode())
            except Exception:
                activated = False
        if not activated:
            _msg("  Warning: Assembly edit mode not active; Add Review Note may be unavailable")
        _msg("  Temporary GUI document '{}'".format(self.doc.Name))

    def tearDown(self):
        import FreeCADGui as Gui

        try:
            Gui.ActiveDocument.resetEdit()
        except Exception:
            pass
        if App.ActiveDocument:
            App.closeDocument(App.ActiveDocument.Name)

    def _flush(self):
        import FreeCADGui as Gui

        try:
            Gui.updateGui()
        except Exception:
            pass
        try:
            view = Gui.ActiveDocument.ActiveView
            if view:
                view.redraw()
        except Exception:
            pass
        try:
            Gui.updateGui()
        except Exception:
            pass

    def _make_note(self, text_lines, sub="Face6", pick=None, offset=None):
        if pick is None:
            pick = App.Vector(5, 10, 30)
        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, sub, picked_point=pick
        )
        return CommandReviewNote.create_review_note(
            self.assembly, data, list(text_lines),
            text_offset=offset, open_transaction=False,
        )

    # --- issue 2: Label should be review_note_N --------------------------------

    def test_gui_label_is_review_note_N(self):
        operation = "GUI: Label is review_note_N, not the note text"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["Hello world"])
        self.assertIsNotNone(note, operation)
        self.assertEqual(
            note.Label, "review_note_1", "{}: got {!r}".format(operation, note.Label)
        )

    # --- issues 3+4+5: frame / leader visibility and thickness -----------------

    def test_gui_frame_default_on(self):
        operation = "GUI: Frame is on by default so the text-box border renders"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["Frame me"])
        self.assertIsNotNone(note.ViewObject, operation)
        self.assertTrue(bool(note.ViewObject.Frame), operation)

    def test_gui_label_image_renders_nonzero_half_extent(self):
        operation = "GUI: label image renders with non-zero half-extent"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["Render me"], offset=App.Vector(40, 0, 0))
        self._flush()
        he = App.Vector(note.ViewObject.LeaderHalfExtent)
        self.assertGreater(float(he.x), 0.0, "{}: halfW={}".format(operation, he.x))
        self.assertGreater(float(he.y), 0.0, "{}: halfH={}".format(operation, he.y))

    def test_gui_display_mode_is_line_so_leader_shows(self):
        operation = "GUI: default display mode is Line so the leader line shows"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["Leader me"])
        modes = note.ViewObject.listDisplayModes()
        self.assertIn("Line", modes, operation)
        self.assertEqual(note.ViewObject.DisplayMode, "Line", operation)

    def test_gui_leader_has_line_width(self):
        operation = "GUI: leader SoDrawStyle sets a visible lineWidth (>= 2)"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["Thick leader"], offset=App.Vector(40, 0, 0))
        self._flush()
        widths = self._collect_leader_line_widths()
        self.assertTrue(
            len(widths) > 0,
            "{}: no leader SoDrawStyle (pointSize==3.0) found in the scene graph".format(operation),
        )
        self.assertTrue(
            any(w >= 2.0 for w in widths),
            "{}: leader SoDrawStyle has no lineWidth>=2; got {}".format(operation, widths),
        )

    def _collect_leader_line_widths(self):
        """Find the leader's SoDrawStyle (the one with pointSize==3.0) and return its
        lineWidth values. The base ViewProviderAnnotationLabel::attach sets
        pointSize=3.0 on the leader's SoDrawStyle but never sets lineWidth, so this
        exposes the 'leader has no thickness' bug."""
        import FreeCADGui as Gui
        from pivy import coin

        viewer = Gui.ActiveDocument.ActiveView
        root = viewer.getSceneGraph()
        sa = coin.SoSearchAction()
        sa.setType(coin.SoDrawStyle.getClassTypeId())
        sa.setInterest(coin.SoSearchAction.ALL)
        sa.apply(root)
        paths = sa.getPaths()
        widths = []
        n = paths.getLength() if paths is not None else 0
        for i in range(n):
            try:
                tail = paths[i].getTail()
            except Exception:
                continue
            if tail is None or not tail.isOfType(coin.SoDrawStyle.getClassTypeId()):
                continue
            if abs(float(tail.pointSize.getValue()) - 3.0) < 1e-6:
                widths.append(float(tail.lineWidth.getValue()))
        return widths

    # --- issue 6: multi-line note grows the rendered box -----------------------

    def test_gui_multiline_note_grows_half_extent(self):
        operation = "GUI: multi-line note grows the rendered half-extent"
        _msg("  Test '{}'".format(operation))
        one = self._make_note(["single"], offset=App.Vector(40, 0, 0))
        self._flush()
        he1 = App.Vector(one.ViewObject.LeaderHalfExtent)
        one.LabelText = ["single", "second line", "third line"]
        self._flush()
        he3 = App.Vector(one.ViewObject.LeaderHalfExtent)
        self.assertGreater(
            float(he3.y), float(he1.y),
            "{}: 3-line halfH={} must exceed 1-line halfH={}".format(operation, he3.y, he1.y),
        )

    # --- issues 9+10: @link click selects target / missing ref no throw ----------

    def test_gui_click_at_ref_selects_target(self):
        operation = "GUI: clicking an @Box.Face6 link selects Box/Face6"
        _msg("  Test '{}'".format(operation))
        import FreeCADGui as Gui

        note = self._make_note(["Inspect @Box.Face6"])
        self.assertIsNotNone(note, operation)
        Gui.Selection.clearSelection()
        self.assertTrue(
            CommandReviewNote.select_review_note_reference(self.doc, "Box", "Face6"),
            operation,
        )
        selected = Gui.Selection.getSelectionEx(self.doc.Name, 0)
        self.assertGreaterEqual(len(selected), 1, operation)
        face_hit = any(
            ("Face6" in list(s.SubElementNames))
            or any(n.endswith("Face6") for n in list(s.SubElementNames))
            for s in selected
        )
        self.assertTrue(face_hit, "{}: Face6 not selected; {!r}".format(
            operation, [(s.Object.Name, list(s.SubElementNames)) for s in selected]
        ))

    def test_gui_click_at_ref_missing_no_throw(self):
        operation = "GUI: clicking a missing @ref returns False without throwing"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["See @Missing.Face1"])
        self.assertIsNotNone(note, operation)
        ok = CommandReviewNote.select_review_note_reference(
            self.doc, "Missing", "Face1"
        )
        self.assertFalse(ok, operation)

    # --- resizable box via the View panel (BoxWidth / BoxHeight in mm) ----------

    def test_gui_box_width_height_default_zero(self):
        operation = "GUI: BoxWidth/BoxHeight default to 0 (auto-size)"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["Auto box"])
        self.assertIsNotNone(note.ViewObject, operation)
        self.assertTrue(hasattr(note.ViewObject, "BoxWidth"), operation)
        self.assertTrue(hasattr(note.ViewObject, "BoxHeight"), operation)
        self.assertEqual(float(note.ViewObject.BoxWidth), 0.0, operation)
        self.assertEqual(float(note.ViewObject.BoxHeight), 0.0, operation)

    def test_gui_fixed_box_width_sets_half_extent(self):
        operation = "GUI: BoxWidth (mm) grows the leader half-width"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["W"], offset=App.Vector(40, 0, 0))
        self._flush()
        auto_he = App.Vector(note.ViewObject.LeaderHalfExtent)
        note.ViewObject.BoxWidth = 60.0
        self._flush()
        fixed_he = App.Vector(note.ViewObject.LeaderHalfExtent)
        # P1: the raster is capped (MaxBoxRasterPx), so at close zoom the leader
        # half-width follows the clamped box and is < BoxWidth/2. It must still grow
        # vs auto and never exceed BoxWidth/2.
        self.assertGreater(
            float(fixed_he.x), float(auto_he.x),
            "{}: fixed halfW={} must exceed auto halfW={}".format(operation, fixed_he.x, auto_he.x),
        )
        self.assertGreater(float(fixed_he.x), 0.0, operation)
        self.assertLessEqual(
            float(fixed_he.x), 30.0 + 1e-3,
            "{}: halfW={} must not exceed BoxWidth/2=30 (raster cap)".format(operation, fixed_he.x),
        )

    def test_gui_fixed_box_height_sets_half_extent(self):
        operation = "GUI: BoxHeight (mm) grows the leader half-height"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["H"], offset=App.Vector(40, 0, 0))
        self._flush()
        auto_he = App.Vector(note.ViewObject.LeaderHalfExtent)
        note.ViewObject.BoxHeight = 24.0
        self._flush()
        fixed_he = App.Vector(note.ViewObject.LeaderHalfExtent)
        # P1: the raster is capped (MaxBoxRasterPx), so at close zoom the leader
        # half-height follows the clamped box and is < BoxHeight/2. It must still
        # grow vs auto and never exceed BoxHeight/2.
        self.assertGreater(
            float(fixed_he.y), float(auto_he.y),
            "{}: fixed halfH={} must exceed auto halfH={}".format(operation, fixed_he.y, auto_he.y),
        )
        self.assertGreater(float(fixed_he.y), 0.0, operation)
        self.assertLessEqual(
            float(fixed_he.y), 12.0 + 1e-3,
            "{}: halfH={} must not exceed BoxHeight/2=12 (raster cap)".format(operation, fixed_he.y),
        )

    def test_gui_fixed_box_raster_is_bounded(self):
        operation = "GUI: fixed box raster is bounded and leader stays glued to it"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["Bounded"], offset=App.Vector(40, 0, 0))
        self._flush()
        # Reference with a small (uncapped) box: derive worldPerPixel from the leader
        # vs the actual raster width (leader = 0.5 * RasterWidth * worldPerPixel).
        note.ViewObject.BoxWidth = 10.0
        self._flush()
        rw_small = int(note.ViewObject.RasterWidth)
        he_small = App.Vector(note.ViewObject.LeaderHalfExtent)
        self.assertGreater(rw_small, 0, operation)
        self.assertGreater(float(he_small.x), 0.0, operation)
        wp_ref = float(he_small.x) / (0.5 * rw_small)
        self.assertGreater(wp_ref, 0.0, operation)
        # A huge mm size would request an enormous raster; the P1 cap must clamp it
        # (without the cap this _flush would exhaust memory or overflow SbVec2s).
        note.ViewObject.BoxWidth = 5000.0
        note.ViewObject.BoxHeight = 5000.0
        self._flush()
        rw = int(note.ViewObject.RasterWidth)
        rh = int(note.ViewObject.RasterHeight)
        self.assertGreater(rw, 0, operation)
        self.assertGreater(rh, 0, operation)
        self.assertLessEqual(rw, 4096, "{}: RasterWidth={} must be capped at 4096".format(operation, rw))
        self.assertLessEqual(rh, 4096, "{}: RasterHeight={} must be capped at 4096".format(operation, rh))
        # Leader must stay glued to the (clamped) raster: the worldPerPixel implied by
        # leader vs raster must match the uncapped reference (same view/camera).
        he = App.Vector(note.ViewObject.LeaderHalfExtent)
        wp_capped = float(he.x) / (0.5 * rw)
        self.assertAlmostEqual(wp_capped, wp_ref, places=3,
            msg="{}: leader detached from raster (wp {} vs ref {})".format(operation, wp_capped, wp_ref))
        self.assertLessEqual(float(he.x), 2500.0, operation)  # <= BoxWidth/2
        self.assertLessEqual(float(he.y), 2500.0, operation)

    def test_gui_viewport_resize_rerasters_fixed_box(self):
        operation = "GUI: viewport resize re-rasterizes a fixed-mm box"
        _msg("  Test '{}'".format(operation))
        import FreeCADGui as Gui
        from PySide import QtWidgets
        note = self._make_note(["Resize"], offset=App.Vector(40, 0, 0))
        note.ViewObject.BoxWidth = 30.0
        self._flush()
        count_before = int(note.ViewObject.DrawImageCount)
        self.assertGreater(count_before, 0, operation)
        # P2: viewport resize changes worldPerPixel but not the Coin camera node, so
        # a fixed-mm box must re-raster via the GL-widget event filter. Resize the 3D
        # view's MDI subwindow; the GL widget gets QEvent::Resize -> filter ->
        # scheduleVisualFrame -> drawImage. DrawImageCount must increase.
        main = Gui.getMainWindow()
        mdi = main.findChild(QtWidgets.QMdiArea)
        subs = mdi.subWindowList() if mdi else []
        self.assertTrue(subs, operation + ": no MDI subwindow to resize")
        sw = subs[0]
        w = sw.width()
        h = sw.height()
        sw.resize(max(w - 140, 320), max(h - 140, 240))
        self._flush()
        count_after = int(note.ViewObject.DrawImageCount)
        sw.resize(w, h)  # restore the original geometry
        self._flush()
        self.assertGreater(count_after, count_before,
            "{}: DrawImageCount {} not > {} (no resize re-raster)".format(
                operation, count_after, count_before))

    def test_gui_auto_box_when_zero_uses_text(self):
        operation = "GUI: BoxWidth=0 keeps auto (text-sized) box"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["single"], offset=App.Vector(40, 0, 0))
        self._flush()
        he1 = App.Vector(note.ViewObject.LeaderHalfExtent)
        # Setting 0 explicitly must not blow the box up.
        note.ViewObject.BoxWidth = 0.0
        note.ViewObject.BoxHeight = 0.0
        self._flush()
        he2 = App.Vector(note.ViewObject.LeaderHalfExtent)
        self.assertLess(
            abs(float(he2.x) - float(he1.x)), 1e-3,
            "{}: auto halfW changed when BoxWidth=0 ({} vs {})".format(operation, he2.x, he1.x),
        )
        self.assertLess(
            abs(float(he2.y) - float(he1.y)), 1e-3,
            "{}: auto halfH changed when BoxHeight=0 ({} vs {})".format(operation, he2.y, he1.y),
        )

    def test_gui_fixed_box_negative_treated_as_auto(self):
        operation = "GUI: negative BoxWidth/Height treated as auto (no crash, no shrink)"
        _msg("  Test '{}'".format(operation))
        note = self._make_note(["Neg"], offset=App.Vector(40, 0, 0))
        self._flush()
        auto_he = App.Vector(note.ViewObject.LeaderHalfExtent)
        note.ViewObject.BoxWidth = -50.0
        note.ViewObject.BoxHeight = -50.0
        self._flush()
        he = App.Vector(note.ViewObject.LeaderHalfExtent)
        self.assertGreater(float(he.x), 0.0, operation)
        self.assertGreater(float(he.y), 0.0, operation)
        # Negative must not shrink below the auto size.
        self.assertGreaterEqual(float(he.x), float(auto_he.x) - 1e-3, operation)
        self.assertGreaterEqual(float(he.y), float(auto_he.y) - 1e-3, operation)
