# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: 2026 FreeCAD Project Association
# SPDX-FileCopyrightText: 2025 Furgo
# SPDX-FileNotice: Part of the FreeCAD project.

################################################################################
#                                                                              #
#   FreeCAD is free software: you can redistribute it and/or modify            #
#   it under the terms of the GNU Lesser General Public License as             #
#   published by the Free Software Foundation, either version 2.1              #
#   of the License, or (at your option) any later version.                     #
#                                                                              #
#   FreeCAD is distributed in the hope that it will be useful,                 #
#   but WITHOUT ANY WARRANTY; without even the implied warranty                #
#   of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.                    #
#   See the GNU Lesser General Public License for more details.                #
#                                                                              #
#   You should have received a copy of the GNU Lesser General Public           #
#   License along with FreeCAD. If not, see https://www.gnu.org/licenses       #
#                                                                              #
################################################################################

import FreeCAD as App
from Test.GuiRecompute import recompute_document, close_document
import Arch
import ArchSectionPlane
import Draft
from bimtests import TestArchBaseGui


class TestArchWindowGui(TestArchBaseGui.TestArchBaseGui):

    def test_change_window_opening(self):
        """Tests if changes to a window opening touches the window's chain of hosts"""

        # Create a wall, a window, a level and a section.
        points = [App.Vector(0.0, 0.0, 0.0), App.Vector(2000.0, 0.0, 0.0)]
        line = Draft.make_wire(points)
        wall = Arch.makeWall(line, height=2000)
        wpl = App.Placement(App.Vector(500, 0, 1500), App.Vector(1, 0, 0), -90)
        win = Arch.makeWindowPreset(
            "Open 1-pane",
            width=1000.0,
            height=1000.0,
            h1=50.0,
            h2=50.0,
            h3=50.0,
            w1=100.0,
            w2=50.0,
            o1=0.0,
            o2=50.0,
            placement=wpl,
        )
        win.Hosts = [wall]
        level = Arch.makeFloor()
        level.addObject(wall)
        section = Arch.makeSectionPlane(level)
        recompute_document(App.ActiveDocument)

        # Change opening from 0 to 50 (= 45 degrees):
        svg = ArchSectionPlane.getSVG(section)
        win.Opening = 50
        recompute_document(App.ActiveDocument)
        svg_new = ArchSectionPlane.getSVG(section)
        self.assertNotEqual(svg, svg_new)

        # Invert opening:
        svg = svg_new
        win.ViewObject.Proxy.invertOpening()
        recompute_document(App.ActiveDocument)
        svg_new = ArchSectionPlane.getSVG(section)
        self.assertNotEqual(svg, svg_new)

        # Invert hinge:
        svg = svg_new
        win.ViewObject.Proxy.invertHinge()
        recompute_document(App.ActiveDocument)
        svg_new = ArchSectionPlane.getSVG(section)
        self.assertNotEqual(svg, svg_new)


    def test_section_native_dimensions_zero_normal_and_cache(self):
        with self._record_section_native_execute() as evidence:
            import time
            from Test.GuiRecompute import touch_on_owner_thread, refresh_document

            section = Arch.makeSectionPlane()
            view = section.ViewObject
            view.DisplayLength = 1200
            view.DisplayHeight = 700
            deadline = time.monotonic() + 10
            while not view.Proxy.dimensionCommandsDone():
                self.assertLess(time.monotonic(), deadline, "dimension owner command timed out")
                self.pump_gui_events(20)
            view.Proxy.raiseDimensionCommandErrors()
            section.Placement = App.Placement(App.Vector(4, 5, 6),
                                              App.Rotation(App.Vector(0, 1, 0), 90))
            for length, height in ((1200, 700), (0, 700), (1200, 0), (0, 0)):
                view.DisplayLength = length
                view.DisplayHeight = height
                deadline = time.monotonic() + 10
                while not view.Proxy.dimensionCommandsDone():
                    self.assertLess(time.monotonic(), deadline)
                    self.pump_gui_events(20)
                view.Proxy.raiseDimensionCommandErrors()
                section.Proxy.svgcache = {"sentinel": True}
                section.Proxy.shapecache = {"sentinel": True}
                touch_on_owner_thread(self.document, section)
                recompute_document(self.document)
                self.assertEqual(section.getStatusString(), "Valid")
                self.assertFalse(set(section.State) & {"Touched", "Invalid", "Recompute"})
                self.assertEqual(len(section.Shape.Faces), 1)
                self.assertAlmostEqual(section.Shape.Area, (length or 1) * (height or 1))
                self.assertLess(section.Proxy.getNormal(section).getAngle(
                    section.Placement.Rotation.multVec(App.Vector(0, 0, 1))), 1e-7)
                self.assertIsNone(section.Proxy.svgcache)
                self.assertIsNone(section.Proxy.shapecache)
                refresh_document(self.document)
                self.assertEqual((section.PlaneLength.Value, section.PlaneHeight.Value),
                                 (length, height))
            self.assertTrue(any(e["native"] for e in evidence["entries"]))


    def _wait_section_dimensions(self, section):
        import time
        deadline = time.monotonic() + 10
        while not section.ViewObject.Proxy.dimensionCommandsDone():
            self.assertLess(time.monotonic(), deadline, "section dimension command timed out")
            self.pump_gui_events(20)
        section.ViewObject.Proxy.raiseDimensionCommandErrors()

    def test_section_legacy_display_size_migrates_to_owner_inputs(self):
        with self._record_section_native_execute() as evidence:
            from Test.GuiRecompute import touch_on_owner_thread
            section = Arch.makeSectionPlane()
            self._wait_section_dimensions(section)
            view = section.ViewObject
            for size, expected in ((700, 700), (0, 1000)):
                # Exercise the existing legacy restore adapter directly, then
                # execute the result through native owner recompute.
                for prop in ("DisplayLength", "DisplayHeight"):
                    view.setPropertyStatus(prop, "-LockDynamic")
                    view.removeProperty(prop)
                view.addProperty("App::PropertyLength", "DisplaySize")
                view.DisplaySize = size
                view.Proxy.onDocumentRestored(view)
                self._wait_section_dimensions(section)
                touch_on_owner_thread(self.document, section)
                recompute_document(self.document)
                self.assertNotIn("DisplaySize", view.PropertiesList)
                self.assertAlmostEqual(section.Shape.Area, expected * expected)
                self.assertEqual(section.getStatusString(), "Valid")
                self.assertFalse(set(section.State) & {"Touched", "Invalid", "Recompute"})
            self.assertTrue(any(e["native"] for e in evidence["entries"]))

    def test_section_nondefault_dimensions_survive_restore(self):
        with self._record_section_native_execute() as evidence:
            import os
            import tempfile
            from Test.GuiRecompute import save_document_as, touch_on_owner_thread
            section = Arch.makeSectionPlane()
            section.ViewObject.DisplayLength = 1200
            section.ViewObject.DisplayHeight = 700
            self._wait_section_dimensions(section)
            recompute_document(self.document)
            name, original = section.Name, self.document.Name
            old_instance = self.document.collaborationIdentity()["instance_id"]
            with tempfile.NamedTemporaryFile(suffix=".FCStd", delete=False) as file:
                path = file.name
            try:
                save_document_as(self.document, path)
                close_document(self.document)
                # The base teardown must not use the retired wrapper, even if
                # opening the saved file fails.
                self.document = None
                self.assertNotIn(original, App.listDocuments())
                reopened = App.openDocument(path)
                self.document = reopened
                self.doc_name = reopened.Name
                self.assertNotEqual(reopened.collaborationIdentity()["instance_id"], old_instance)
                App.setActiveDocument(self.doc_name)
                restored = reopened.getObject(name)
                self.assertIsNotNone(restored)
                self._wait_section_dimensions(restored)
                touch_on_owner_thread(reopened, restored)
                recompute_document(reopened)
                self.assertAlmostEqual(restored.Shape.Area, 840000)
                self.assertEqual(restored.getStatusString(), "Valid")
                self.assertFalse(set(restored.State) & {"Touched", "Invalid", "Recompute"})
                self.assertAlmostEqual(restored.ViewObject.DisplayLength.Value, 1200)
                self.assertAlmostEqual(restored.ViewObject.DisplayHeight.Value, 700)
                self.assertTrue(any(e["native"] and e["name"] == name
                                    and e["instance_id"] == reopened.collaborationIdentity()["instance_id"]
                                    for e in evidence["entries"]))
            finally:
                try:
                    if self.doc_name in App.listDocuments():
                        close_document(App.getDocument(self.doc_name))
                finally:
                    self.document = None
                    os.unlink(path)
            self.assertTrue(any(e["native"] for e in evidence["entries"]))

    def _record_section_native_execute(self):
        """Install before construction; preserve C++-swallowed first errors."""
        import threading
        from contextlib import contextmanager

        @contextmanager
        def recording():
            cls = ArchSectionPlane._SectionPlane
            original = cls.execute  # May already be root's recorder.
            main_thread = threading.get_ident()
            evidence = {"entries": [], "first_execute_error": None, "first_native_error": None}
            first_error, observation_errors = [], []
            self._section_followup_execute_evidence = evidence
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
            cls.execute = execute
            try:
                yield evidence
            finally:
                try:
                    if self.doc_name in App.listDocuments():
                        close_document(App.getDocument(self.doc_name))
                finally:
                    self.document = None
                    cls.execute = original
                    if first_error:
                        exc, traceback = first_error[0]
                        raise exc.with_traceback(traceback)
                    if observation_errors:
                        exc, traceback = observation_errors[0]
                        raise exc.with_traceback(traceback)
        return recording()

    def test_section_completed_dimension_transaction_undo_redo(self):
        """Native owner completion followed by true GUI undo/redo; no input replay."""
        from Test.GuiRecompute import (undo_document, redo_document,
                                       touch_on_owner_thread, refresh_document)
        cls = ArchSectionPlane._ViewProviderSectionPlane
        original_submit = cls._submitDimensionInputs
        submissions, attempts = [], []
        self._section_followup_submission_evidence = dict(submissions=submissions, attempts=attempts)
        def submit(provider, view):
            attempts.append(view.Object.Name)
            prior = getattr(view.Object.Proxy, "_dimension_request", None)
            result = original_submit(provider, view)
            current = getattr(view.Object.Proxy, "_dimension_request", None)
            # In the reviewed provider a new bound request is assigned exactly
            # once before each native compatibility submission. Undo guards and
            # completed-value dedup may enter this method without submitting.
            if current != prior:
                submissions.append(current)
            return result
        cls._submitDimensionInputs = submit
        try:
            with self._record_section_native_execute() as evidence:
                section = Arch.makeSectionPlane()
                self._wait_section_dimensions(section)
                recompute_document(self.document)
                view = section.ViewObject
                def controlled_input(length, height):
                    before = len(submissions)
                    # The supported Python provider guard batches the aliases.
                    # Its explicit submit uses native compatibility's labeled
                    # transaction, rather than opening a transaction on GUI.
                    view.Proxy._presenting_dimensions = True
                    try:
                        view.DisplayLength, view.DisplayHeight = length, height
                    finally:
                        view.Proxy._presenting_dimensions = False
                    view.Proxy._submitDimensionInputs(view)
                    self._wait_section_dimensions(section)  # Real terminal publication.
                    self.assertEqual(len(submissions), before + 1)
                    touch_on_owner_thread(self.document, section)
                    recompute_document(self.document)
                    self.assertAlmostEqual(section.Shape.Area, length * height)
                    self.assertEqual(section.getStatusString(), "Valid")
                controlled_input(1200, 700)
                initial_revision = section.PlaneDimensionsRevision
                controlled_input(900, 400)
                edited_revision = section.PlaneDimensionsRevision
                self.assertNotEqual(initial_revision, edited_revision)
                count = len(submissions)
                for action, length, height, revision in (
                        (undo_document, 1200, 700, initial_revision),
                        (redo_document, 900, 400, edited_revision)):
                    start = len(evidence["entries"])
                    action(self.document)
                    self.assertEqual((section.PlaneLength.Value, section.PlaneHeight.Value,
                                      section.PlaneDimensionsRevision), (length, height, revision))
                    section.Proxy.svgcache, section.Proxy.shapecache = "old-svg", "old-shape"
                    touch_on_owner_thread(self.document, section)
                    recompute_document(self.document)
                    self.assertTrue(any(e["native"] and e["name"] == section.Name
                                        for e in evidence["entries"][start:]))
                    self.assertAlmostEqual(section.Shape.Area, length * height)
                    self.assertIsNone(section.Proxy.svgcache)
                    self.assertIsNone(section.Proxy.shapecache)
                    refresh_document(self.document)
                    self.assertEqual((view.DisplayLength.Value, view.DisplayHeight.Value), (length, height))
                    self.assertEqual(section.getStatusString(), "Valid")
                    self.assertFalse(set(section.State) & {"Touched", "Invalid", "Recompute"})
                    self.assertEqual(len(submissions), count, "undo/presentation submitted a new input")
        finally:
            cls._submitDimensionInputs = original_submit
