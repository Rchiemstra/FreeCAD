# SPDX-License-Identifier: LGPL-2.1-or-later
# SPDX-FileCopyrightText: 2026 FreeCAD contributors
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

import Arch
import Draft
import FreeCAD
from Test.GuiRecompute import recompute_document, close_document

from bimtests.TestArchBaseGui import TestArchBaseGui


class TestArchStairsGui(TestArchBaseGui):

    def _assert_visibility(self, stairs, expected):
        self.assertEqual(stairs.ViewObject.Visibility, expected)
        for railing in (stairs.RailingLeft, stairs.RailingRight):
            self.assertIsNotNone(railing)
            self.assertEqual(railing.ViewObject.Visibility, expected)

    def test_stairs_railings_follow_parent_visibility(self):
        stairs = Arch.makeStairs(length=3500, width=800, height=2500, steps=14)
        recompute_document(self.document)

        self.assertIsNotNone(stairs.RailingLeft)
        self.assertIsNotNone(stairs.RailingRight)
        self._assert_visibility(stairs, True)

        stairs.ViewObject.Visibility = False
        self.pump_gui_events()
        recompute_document(self.document)
        self._assert_visibility(stairs, False)

        stairs.ViewObject.Visibility = True
        self.pump_gui_events()
        recompute_document(self.document)
        self._assert_visibility(stairs, True)

        level = Arch.makeBuildingPart()
        level.Group = [stairs]
        recompute_document(self.document)

        level.ViewObject.Visibility = False
        self.pump_gui_events()
        recompute_document(self.document)
        self._assert_visibility(stairs, False)

        level.ViewObject.Visibility = True
        self.pump_gui_events()
        recompute_document(self.document)
        self._assert_visibility(stairs, True)

    def test_stairs_multi_segment_railing_follow_parent_visibility(self):
        wire1 = Draft.make_wire([FreeCAD.Vector(0, 0, 0), FreeCAD.Vector(1000, 0, 0)])
        wire2 = Draft.make_wire([FreeCAD.Vector(1000, 0, 0), FreeCAD.Vector(1000, 1000, 0)])
        stairs = Arch.makeStairs(baseobj=[wire1, wire2], width=800, height=2500, steps=14)
        recompute_document(self.document)

        Arch.makeRailing([stairs] + stairs.Additions)
        recompute_document(self.document)

        segment = stairs.Additions[-1]
        segment.RailingLeft.ViewObject.Visibility = True
        segment.RailingRight.ViewObject.Visibility = True

        stairs.ViewObject.Visibility = False
        self.pump_gui_events()
        recompute_document(self.document)

        self.assertFalse(segment.RailingLeft.ViewObject.Visibility)
        self.assertFalse(segment.RailingRight.ViewObject.Visibility)

        stairs.ViewObject.Visibility = True
        self.pump_gui_events()
        recompute_document(self.document)

        self.assertTrue(segment.RailingLeft.ViewObject.Visibility)
        self.assertTrue(segment.RailingRight.ViewObject.Visibility)


    def test_native_railing_wire_completion_and_one_time_hide(self):
        from Test.GuiRecompute import touch_on_owner_thread, refresh_document

        stairs = Arch.makeStairs(length=3500, width=800, height=2500, steps=14)
        recompute_document(self.document)
        refresh_document(self.document)
        for rail in (stairs.RailingLeft, stairs.RailingRight):
            self.assertIsNotNone(rail.Base)
            self.assertEqual(rail.Base.TypeId, "Part::Feature")
            self.assertFalse(rail.Base.Shape.isNull())
            self.assertTrue(rail.Base.Shape.isValid())
            self.assertFalse(rail.Base.ViewObject.Visibility)
            self.assertEqual(rail.getStatusString(), "Valid")
        self.assertEqual(stairs.getStatusString(), "Valid")
        self.assertAlmostEqual(stairs.BlondelRatio.Value,
                               2 * stairs.RiserHeight.Value + stairs.TreadDepth.Value)

        # Both replacement branches must finish. A missing railing is valid on
        # other fixtures; this factory explicitly requires both railings.
        for side in ("RailingRight", "RailingLeft"):
            rail = getattr(stairs, side)
            old = rail.Base
            old_id = old.ID
            old_name = old.Name
            rail.Base = None
            self.document.removeObject(old_name)
            touch_on_owner_thread(self.document, stairs)
            recompute_document(self.document)
            refresh_document(self.document)
            self.assertIsNotNone(rail.Base)
            self.assertNotEqual(rail.Base.ID, old_id)
            self.assertFalse(rail.Base.Shape.isNull())
            self.assertFalse(rail.Base.ViewObject.Visibility)

        # An unrelated catch-up must not erase a later explicit user choice.
        wire = stairs.RailingRight.Base
        wire.ViewObject.show()
        ids = tuple(getattr(stairs, side).Base.ID for side in ("RailingLeft", "RailingRight"))
        count = len(self.document.Objects)
        touch_on_owner_thread(self.document, stairs)
        recompute_document(self.document)
        refresh_document(self.document)
        self.assertTrue(wire.ViewObject.Visibility)
        self.assertEqual(ids, tuple(getattr(stairs, side).Base.ID
                                   for side in ("RailingLeft", "RailingRight")))
        self.assertEqual(len(self.document.Objects), count)


    def test_generated_wire_user_visibility_survives_restore(self):
        import os
        import tempfile
        import threading
        import ArchStairs
        from Test.GuiRecompute import save_document_as, refresh_document, touch_on_owner_thread

        cls = ArchStairs._Stairs
        original_execute = cls.execute
        main_thread = threading.get_ident()
        first_error, observation_errors = [], []
        evidence = {"entries": [], "first_execute_error": None, "first_native_error": None}
        self._stairs_restore_execute_evidence = evidence
        phase = "initial"
        def execute(proxy, obj, *args, **kwargs):
            entry = dict(phase=phase, thread=threading.get_ident(), gui_main_thread=main_thread,
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
                return original_execute(proxy, obj, *args, **kwargs)
            except BaseException as exc:
                entry["error"] = repr(exc)
                if not first_error:
                    first_error.append((exc, exc.__traceback__))
                    evidence["first_execute_error"] = repr(exc)
                if entry["native"] and evidence["first_native_error"] is None:
                    evidence["first_native_error"] = repr(exc)
                raise
        cls.execute = execute  # FeaturePython caches this binding at construction.
        path = None
        try:
            stairs = Arch.makeStairs(length=3500, width=800, height=2500, steps=14)
            recompute_document(self.document)
            refresh_document(self.document)
            self.assertTrue(any(e["native"] and e["name"] == stairs.Name
                                and e["phase"] == "initial" for e in evidence["entries"]))
            self.assertEqual(stairs.getStatusString(), "Valid")
            self.assertFalse(set(stairs.State) & {"Touched", "Invalid", "Recompute"})
            for rail in (stairs.RailingLeft, stairs.RailingRight):
                self.assertIsNotNone(rail)
                self.assertIsNotNone(rail.Base)
                self.assertFalse(rail.Shape.isNull())
                self.assertTrue(rail.Shape.isValid())
                self.assertFalse(rail.Base.Shape.isNull())
                self.assertTrue(rail.Base.Shape.isValid())
                self.assertEqual(rail.getStatusString(), "Valid")
                self.assertEqual(rail.Base.getStatusString(), "Valid")
                self.assertFalse(rail.Base.ViewObject.Visibility)
            stairs.RailingRight.Base.ViewObject.show()
            self.assertTrue(stairs.RailingRight.Base.ViewObject.Visibility)
            name, original_name = stairs.Name, self.document.Name
            old_instance = self.document.collaborationIdentity()["instance_id"]
            links = tuple((rail.Name, rail.Base.Name, rail.Base.ID)
                          for rail in (stairs.RailingLeft, stairs.RailingRight))
            with tempfile.NamedTemporaryFile(suffix=".FCStd", delete=False) as file:
                path = file.name
            save_document_as(self.document, path)
            close_document(self.document)
            self.document = None  # Immediately retire the base teardown reference.
            self.assertNotIn(original_name, FreeCAD.listDocuments())
            phase = "fresh-open"
            reopened = FreeCAD.openDocument(path)
            self.document, self.doc_name = reopened, reopened.Name
            fresh_instance = reopened.collaborationIdentity()["instance_id"]
            self.assertNotEqual(fresh_instance, old_instance)
            restored = reopened.getObject(name)
            self.assertIsNotNone(restored)
            for side, (rail_name, wire_name, wire_id) in zip(
                    ("RailingLeft", "RailingRight"), links):
                rail, wire = reopened.getObject(rail_name), reopened.getObject(wire_name)
                self.assertIsNotNone(rail)
                self.assertIsNotNone(wire)
                self.assertEqual(getattr(restored, side), rail)
                self.assertEqual(rail.Base, wire)
                self.assertEqual(wire.ID, wire_id)
            phase = "fresh-touched"
            touch_on_owner_thread(reopened, restored)
            recompute_document(reopened)
            refresh_document(reopened)
            self.assertTrue(any(e["native"] and e["name"] == name
                                and e["instance_id"] == fresh_instance
                                and e["phase"] == "fresh-touched" for e in evidence["entries"]))
            self.assertFalse(restored.RailingLeft.Base.ViewObject.Visibility)
            self.assertTrue(restored.RailingRight.Base.ViewObject.Visibility)
            for target in (restored, restored.RailingLeft, restored.RailingRight,
                           restored.RailingLeft.Base, restored.RailingRight.Base):
                self.assertFalse(target.Shape.isNull())
                self.assertTrue(target.Shape.isValid())
                self.assertEqual(target.getStatusString(), "Valid")
                self.assertFalse(set(target.State) & {"Touched", "Invalid", "Recompute"})
        finally:
            phase = "cleanup"
            try:
                if self.doc_name in FreeCAD.listDocuments():
                    close_document(FreeCAD.getDocument(self.doc_name))
            finally:
                self.document = None
                try:
                    if path is not None:
                        os.unlink(path)
                finally:
                    cls.execute = original_execute
                    if first_error:
                        exc, traceback = first_error[0]
                        raise exc.with_traceback(traceback)
                    if observation_errors:
                        exc, traceback = observation_errors[0]
                        raise exc.with_traceback(traceback)
