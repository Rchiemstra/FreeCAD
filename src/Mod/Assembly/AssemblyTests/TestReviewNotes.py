# SPDX-License-Identifier: LGPL-2.1-or-later
# /****************************************************************************
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
#    License along with FreeCAD. If not, see                                *
#    <https://www.gnu.org/licenses/>.                                       *
#                                                                           *
# ***************************************************************************/

"""Tests for Assembly review notes: eligibility, normalization, tracking, persistence."""

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


class TestReviewNotes(unittest.TestCase):
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

    def _make_joint(self):
        box2 = self.assembly.newObject("Part::Box", "Box2")
        self.doc.recompute()
        joint = self.jointgroup.newObject("App::FeaturePython", "Joint")
        JointObject.Joint(joint, 0)  # Fixed
        joint.Reference1 = [self.box, ["Face6", "Vertex7"]]
        joint.Reference2 = [box2, ["Face6", "Vertex7"]]
        joint.Placement1 = App.Placement(App.Vector(5, 10, 30), App.Rotation())
        joint.Placement2 = App.Placement(App.Vector(5, 10, 0), App.Rotation())
        self.doc.recompute()
        return joint, box2

    # --- eligibility / normalization ---------------------------------

    def test_normalize_component(self):
        operation = "Normalize whole component"
        _msg("  Test '{}'".format(operation))
        data = CommandReviewNote.normalize_review_note_target(self.assembly, self.box, "")
        self.assertIsNotNone(data, operation)
        self.assertEqual(data["target_obj"], self.box)
        self.assertEqual(data["joint_side"], "None")
        self.assertEqual(data["local_anchor"], App.Vector(5, 10, 15))

    def test_normalize_face_edge_vertex(self):
        operation = "Normalize face/edge/vertex"
        _msg("  Test '{}'".format(operation))

        for sub in ("Face6", "Edge1", "Vertex1"):
            data = CommandReviewNote.normalize_review_note_target(
                self.assembly, self.box, sub, picked_point=App.Vector(1, 2, 3)
            )
            self.assertIsNotNone(data, "{} ({})".format(operation, sub))
            self.assertEqual(data["target_obj"], self.box)
            self.assertEqual(data["sub_list"], [sub])
            self.assertEqual(data["joint_side"], "None")
            # Local anchor from pick relative to component placement (identity).
            self.assertTrue(
                data["local_anchor"].isEqual(App.Vector(1, 2, 3), 1e-6),
                "{} ({}) anchor".format(operation, sub),
            )

    def test_normalize_joint_sides(self):
        operation = "Normalize joint sides"
        _msg("  Test '{}'".format(operation))
        joint, _box2 = self._make_joint()

        # Tree selection defaults to Reference1.
        data = CommandReviewNote.normalize_review_note_target(self.assembly, joint, "")
        self.assertIsNotNone(data, operation)
        self.assertEqual(data["target_obj"], joint)
        self.assertEqual(data["joint_side"], "Reference1")
        self.assertEqual(data["local_anchor"], App.Vector())

        # 3D Main pick near Placement2 chooses Reference2.
        near_ref2 = App.Vector(5, 10, 0)
        data2 = CommandReviewNote.normalize_review_note_target(
            self.assembly, joint, "Main", picked_point=near_ref2
        )
        self.assertIsNotNone(data2, operation)
        self.assertEqual(data2["joint_side"], "Reference2")

        near_ref1 = App.Vector(5, 10, 30)
        data1 = CommandReviewNote.normalize_review_note_target(
            self.assembly, joint, "Main", picked_point=near_ref1
        )
        self.assertEqual(data1["joint_side"], "Reference1")

    def test_unsupported_and_multiple_selection(self):
        operation = "Unsupported / multiple selection eligibility"
        _msg("  Test '{}'".format(operation))

        # Unsupported: bare document object outside assembly.
        other = self.doc.addObject("Part::Box", "Outside")
        data = CommandReviewNote.normalize_review_note_target(self.assembly, other, "Face1")
        self.assertIsNone(data, operation)

        # Exactly one supported target is eligible.
        sel_one = [_FakeSel(self.box, ["Face6"], [App.Vector(5, 10, 30)])]
        self.assertTrue(CommandReviewNote.is_add_review_note_eligible(self.assembly, sel_one))

        # Multiple supported targets are not eligible.
        box2 = self.assembly.newObject("Part::Box", "BoxB")
        sel_multi = [
            _FakeSel(self.box, ["Face6"], [App.Vector(5, 10, 30)]),
            _FakeSel(box2, ["Face6"], [App.Vector(5, 10, 30)]),
        ]
        self.assertFalse(CommandReviewNote.is_add_review_note_eligible(self.assembly, sel_multi))

        # No assembly.
        self.assertFalse(CommandReviewNote.is_add_review_note_eligible(None, sel_one))

    # --- creation / group / tracking ---------------------------------

    def test_lazy_group_and_create_note(self):
        operation = "Lazy group creation and note properties"
        _msg("  Test '{}'".format(operation))

        groups = [o for o in self.assembly.OutList if o.TypeId == "Assembly::ReviewNoteGroup"]
        self.assertEqual(len(groups), 0)

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["First line", "Second line"], open_transaction=False
        )
        self.assertIsNotNone(note)
        self.assertEqual(note.TypeId, "Assembly::ReviewNote")
        self.assertEqual(list(note.LabelText), ["First line", "Second line"])
        self.assertEqual(note.Label, "First line")
        self.assertEqual(note.Target[0], self.box)
        self.assertEqual(list(note.Target[1]), ["Face6"])
        self.assertTrue(note.LocalAnchor.isEqual(App.Vector(5, 10, 30), 1e-6))
        self.assertFalse(note.Resolved)

        groups = [o for o in self.assembly.OutList if o.TypeId == "Assembly::ReviewNoteGroup"]
        self.assertEqual(len(groups), 1)
        self.assertEqual(groups[0].Label, "Review Notes")
        self.assertIn(note, groups[0].Group)

        # BasePosition follows local anchor at identity placement.
        self.assertTrue(note.BasePosition.isEqual(App.Vector(5, 10, 30), 1e-6), operation)

    def test_attachment_under_translation_and_rotation(self):
        operation = "Attachment under translation and rotation"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "", picked_point=App.Vector(5, 10, 15)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Move me"], open_transaction=False
        )
        self.assertTrue(note.BasePosition.isEqual(App.Vector(5, 10, 15), 1e-6))

        self.box.Placement = App.Placement(App.Vector(100, 0, 0), App.Rotation(0, 0, 90))
        # Observer refreshes synchronously on Placement change.
        expected = self.box.Placement.multVec(App.Vector(5, 10, 15))
        self.assertTrue(
            note.BasePosition.isEqual(expected, 1e-6),
            "{}: got {} expected {}".format(operation, note.BasePosition, expected),
        )

    def test_attachment_with_assembly_placement(self):
        operation = "Attachment with non-identity Assembly placement"
        _msg("  Test '{}'".format(operation))

        self.assembly.Placement = App.Placement(App.Vector(50, 60, 70), App.Rotation(10, 20, 30))
        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "", picked_point=None
        )
        # Without a pick, local anchor is bbox center in component-local space.
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Asm plc"], open_transaction=False
        )
        # BasePosition is assembly-local: component Placement * LocalAnchor.
        expected = self.box.Placement.multVec(note.LocalAnchor)
        self.assertTrue(note.BasePosition.isEqual(expected, 1e-6), operation)

        # Moving the Assembly must not rewrite assembly-local BasePosition.
        before = App.Vector(note.BasePosition)
        self.assembly.Placement = App.Placement(App.Vector(1, 2, 3), App.Rotation(0, 0, 90))
        self.assertTrue(
            note.BasePosition.isEqual(before, 1e-6),
            "{}: Assembly move changed local BasePosition".format(operation),
        )

    def test_parent_note_follows_nested_container_placement(self):
        operation = "Parent-owned note follows nested Assembly placement"
        _msg("  Test '{}'".format(operation))

        nested = self.assembly.newObject("Assembly::AssemblyObject", "Nested")
        inner = nested.newObject("Part::Box", "Inner")
        inner.Length = 10
        inner.Width = 20
        inner.Height = 30
        self.doc.recompute()

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, inner, "", picked_point=App.Vector(5, 10, 15)
        )
        self.assertIsNotNone(data, operation)
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Nested container"], open_transaction=False
        )

        nested.Placement = App.Placement(App.Vector(100, 0, 0), App.Rotation(0, 0, 45))
        expected = nested.Placement.multVec(inner.Placement.multVec(note.LocalAnchor))
        self.assertTrue(
            note.BasePosition.isEqual(expected, 1e-5),
            "{}: got {} expected {}".format(operation, note.BasePosition, expected),
        )

        # Rotation-only intermediate move must also refresh.
        nested.Placement = App.Placement(App.Vector(0, 50, 0), App.Rotation(0, 90, 0))
        expected = nested.Placement.multVec(inner.Placement.multVec(note.LocalAnchor))
        self.assertTrue(
            note.BasePosition.isEqual(expected, 1e-5),
            "{} after rotate: got {} expected {}".format(
                operation, note.BasePosition, expected
            ),
        )

    def test_joint_note_follows_nested_container_placement(self):
        operation = "Joint note follows nested container move of its reference"
        _msg("  Test '{}'".format(operation))

        nested = self.assembly.newObject("Assembly::AssemblyObject", "NestedJ")
        inner = nested.newObject("Part::Box", "InnerJ")
        inner.Length = 10
        inner.Width = 20
        inner.Height = 30
        box2 = self.assembly.newObject("Part::Box", "Box2J")
        self.doc.recompute()

        joint = self.jointgroup.newObject("App::FeaturePython", "JointNested")
        JointObject.Joint(joint, 0)
        joint.Reference1 = [inner, ["Face6", "Vertex7"]]
        joint.Reference2 = [box2, ["Face6", "Vertex7"]]
        joint.Placement1 = App.Placement(App.Vector(5, 10, 30), App.Rotation())
        joint.Placement2 = App.Placement(App.Vector(5, 10, 0), App.Rotation())
        self.doc.recompute()

        data = CommandReviewNote.normalize_review_note_target(self.assembly, joint, "")
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Joint nested"], open_transaction=False
        )

        nested.Placement = App.Placement(App.Vector(80, 0, 0), App.Rotation())
        # Reference1 / moving part lives under nested; BasePosition is assembly-local.
        expected = nested.Placement.multVec(inner.Placement.multVec(joint.Placement1.Base))
        self.assertTrue(
            note.BasePosition.isEqual(expected, 1e-5),
            "{}: got {} expected {}".format(operation, note.BasePosition, expected),
        )

    def test_purge_touched_placement_path(self):
        operation = "Placement-then-purgeTouched refresh"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "", picked_point=App.Vector(1, 2, 3)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Solver path"], open_transaction=False
        )

        self.box.Placement = App.Placement(App.Vector(0, 0, 40), App.Rotation())
        self.box.purgeTouched()
        expected = App.Vector(1, 2, 43)
        self.assertTrue(
            note.BasePosition.isEqual(expected, 1e-6),
            "{}: got {}".format(operation, note.BasePosition),
        )

    def test_joint_note_follows_jcs(self):
        operation = "Joint note follows JCS"
        _msg("  Test '{}'".format(operation))
        joint, _box2 = self._make_joint()

        data = CommandReviewNote.normalize_review_note_target(self.assembly, joint, "")
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Joint note"], open_transaction=False
        )
        # JointObject recomputes Placement1 from references; anchor at that JCS origin.
        expected = App.Vector(joint.Placement1.Base)
        self.assertTrue(
            note.BasePosition.isEqual(expected, 1e-6),
            "{}: got {} expected {}".format(operation, note.BasePosition, expected),
        )

        joint.Placement1 = App.Placement(App.Vector(1, 1, 1), App.Rotation())
        self.assertTrue(note.BasePosition.isEqual(App.Vector(1, 1, 1), 1e-6), operation)

        self.box.Placement = App.Placement(App.Vector(10, 0, 0), App.Rotation())
        self.assertTrue(note.BasePosition.isEqual(App.Vector(11, 1, 1), 1e-6), operation)

    def test_broken_target_and_undo(self):
        operation = "Broken target retains text; undo reattaches"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Keep me"], open_transaction=True
        )
        last_base = App.Vector(note.BasePosition)

        self.doc.openTransaction("Delete target")
        self.doc.removeObject(self.box.Name)
        self.doc.commitTransaction()

        self.assertTrue(note.LabelText[0] == "Keep me")
        self.assertTrue(note.BasePosition.isEqual(last_base, 1e-6))
        self.assertTrue(note.isAttachmentBroken())

        self.doc.undo()
        self.assertFalse(note.isAttachmentBroken())
        self.assertEqual(note.Target[0].Name, "Box")

    def test_first_creation_undo_removes_group(self):
        operation = "First-creation undo removes note and group"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(self.assembly, self.box, "")
        CommandReviewNote.create_review_note(
            self.assembly, data, ["Undo me"], open_transaction=True
        )
        groups = [o for o in self.assembly.OutList if o.TypeId == "Assembly::ReviewNoteGroup"]
        self.assertEqual(len(groups), 1)

        self.doc.undo()
        groups = [o for o in self.doc.Objects if o.TypeId == "Assembly::ReviewNoteGroup"]
        notes = [o for o in self.doc.Objects if o.TypeId == "Assembly::ReviewNote"]
        self.assertEqual(len(groups), 0, operation)
        self.assertEqual(len(notes), 0, operation)

    def test_resolve_edit_visibility_undo(self):
        operation = "Resolve/edit participate in undo"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(self.assembly, self.box, "")
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Open"], open_transaction=False
        )

        CommandReviewNote.edit_review_note(note, ["Edited"])
        self.assertEqual(list(note.LabelText), ["Edited"])
        self.doc.undo()
        self.assertEqual(list(note.LabelText), ["Open"])

        CommandReviewNote.toggle_resolve_review_note(note)
        self.assertTrue(note.Resolved)
        self.doc.undo()
        self.assertFalse(note.Resolved)

    def test_save_reload_persistence(self):
        operation = "Save/close/reload persistence"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly,
            data,
            ["Persist", "Line2"],
            text_offset=App.Vector(12, 13, 14),
            open_transaction=False,
        )
        note.Resolved = True
        note.LeaderPort = 0.25
        if note.ViewObject:
            note.ViewObject.Visibility = False

        fd, path = tempfile.mkstemp(suffix=".FCStd")
        os.close(fd)
        try:
            self.doc.saveAs(path)
            doc_name = self.doc.Name
            App.closeDocument(doc_name)

            loaded = App.openDocument(path)
            self.doc = loaded
            App.setActiveDocument(loaded.Name)

            notes = [o for o in loaded.Objects if o.TypeId == "Assembly::ReviewNote"]
            self.assertEqual(len(notes), 1, operation)
            note = notes[0]
            self.assertEqual(list(note.LabelText), ["Persist", "Line2"])
            self.assertEqual(note.Target[0].Name, "Box")
            self.assertEqual(list(note.Target[1]), ["Face6"])
            self.assertTrue(note.LocalAnchor.isEqual(App.Vector(5, 10, 30), 1e-6))
            self.assertTrue(note.TextPosition.isEqual(App.Vector(12, 13, 14), 1e-6))
            self.assertTrue(note.Resolved)
            self.assertAlmostEqual(note.LeaderPort, 0.25, places=5)
            if note.ViewObject:
                self.assertFalse(
                    note.ViewObject.Visibility,
                    "{} visibility must persist".format(operation),
                )
            groups = [o for o in loaded.Objects if o.TypeId == "Assembly::ReviewNoteGroup"]
            self.assertEqual(len(groups), 1)
            self.assertIn(note, groups[0].Group)

            box = loaded.getObject("Box")
            box.Placement = App.Placement(App.Vector(0, 0, 5), App.Rotation())
            expected = box.Placement.multVec(note.LocalAnchor)
            self.assertTrue(
                note.BasePosition.isEqual(expected, 1e-6),
                "{} tracking after reload".format(operation),
            )
        finally:
            if App.ActiveDocument:
                App.closeDocument(App.ActiveDocument.Name)
            # Recreate a blank doc so tearDown can close it cleanly.
            self.doc = App.newDocument(self.__class__.__name__)
            App.setActiveDocument(self.doc.Name)
            try:
                os.remove(path)
            except OSError:
                pass

    def test_text_references_parse_resolve_and_persist(self):
        operation = "Parse/resolve @refs; persist in LabelText"
        _msg("  Test '{}'".format(operation))

        refs = CommandReviewNote.parse_review_note_references(
            [
                "See @Box.Face6 and @Box",
                "Also @Missing.Face1",
            ]
        )
        self.assertEqual(len(refs), 3, operation)
        self.assertEqual(refs[0]["obj_name"], "Box", operation)
        self.assertEqual(refs[0]["sub_name"], "Face6", operation)
        self.assertEqual(refs[1]["obj_name"], "Box", operation)
        self.assertEqual(refs[1]["sub_name"], "", operation)

        obj, sub = CommandReviewNote.resolve_review_note_reference(
            self.doc, "Box", "Face6"
        )
        self.assertEqual(obj, self.box, operation)
        self.assertEqual(sub, "Face6", operation)
        missing, _sub = CommandReviewNote.resolve_review_note_reference(
            self.doc, "NoSuch", "Face1"
        )
        self.assertIsNone(missing, operation)

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly,
            data,
            ["Check @Box.Face6 before release"],
            open_transaction=False,
        )
        self.assertIn("@Box.Face6", note.LabelText[0], operation)

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
                list(notes[0].LabelText),
                ["Check @Box.Face6 before release"],
                operation,
            )
            loaded_refs = CommandReviewNote.parse_review_note_references(notes[0].LabelText)
            self.assertEqual(len(loaded_refs), 1, operation)
            self.assertEqual(loaded_refs[0]["full"], "Box.Face6", operation)
        finally:
            if App.ActiveDocument:
                App.closeDocument(App.ActiveDocument.Name)
            self.doc = App.newDocument(self.__class__.__name__)
            App.setActiveDocument(self.doc.Name)
            try:
                os.remove(path)
            except OSError:
                pass

    def test_at_suggestions_and_cursor_token(self):
        operation = "@ suggestions for objects/subelements and cursor token"
        _msg("  Test '{}'".format(operation))

        at_idx, prefix = CommandReviewNote.at_token_at_cursor("See @Bo", 7)
        self.assertEqual(at_idx, 4, operation)
        self.assertEqual(prefix, "Bo", operation)
        at_idx, prefix = CommandReviewNote.at_token_at_cursor("See @", 5)
        self.assertEqual(at_idx, 4, operation)
        self.assertEqual(prefix, "", operation)
        at_idx, prefix = CommandReviewNote.at_token_at_cursor("See @Box.Face", 13)
        self.assertEqual(at_idx, 4, operation)
        self.assertEqual(prefix, "Box.Face", operation)
        at_idx, prefix = CommandReviewNote.at_token_at_cursor("plain text", 5)
        self.assertIsNone(at_idx, operation)

        names = CommandReviewNote.collect_review_note_at_suggestions(self.doc, "")
        self.assertIn("Box", names, operation)
        self.assertNotIn("Joints", names, operation)

        filtered = CommandReviewNote.collect_review_note_at_suggestions(self.doc, "Bo")
        self.assertIn("Box", filtered, operation)

        faces = CommandReviewNote.collect_review_note_at_suggestions(self.doc, "Box.Fa")
        self.assertTrue(any(s.startswith("Box.Face") for s in faces), operation)
        self.assertTrue(all(s.startswith("Box.") for s in faces), operation)

        after_dot = CommandReviewNote.collect_review_note_at_suggestions(self.doc, "Box.")
        self.assertTrue(any(s == "Box.Face1" or s.startswith("Box.Face") for s in after_dot), operation)

        long_path = "AssemblyCase.MidA.MidB.RightPocket.Face12"
        shortened = CommandReviewNote.ellipsis_review_note_at_path(long_path, 32)
        self.assertEqual(
            shortened,
            "AssemblyCase.…RightPocket.Face12",
            operation,
        )
        self.assertNotIn("MidA", shortened, operation)
        self.assertEqual(
            CommandReviewNote.ellipsis_review_note_at_path("Box.Face1", 80),
            "Box.Face1",
            operation,
        )
        inserted = CommandReviewNote.apply_review_note_at_completion(
            "See @As", 7, long_path
        )
        self.assertEqual(inserted, "See @" + long_path, operation)
        self.assertNotIn("…", inserted, operation)

        # Selection → @ref path helper (used by modeless face-click insert).
        self.assertEqual(
            CommandReviewNote.review_note_at_path_from_selection(self.box, "Face6"),
            "Box.Face6",
            operation,
        )
        self.assertEqual(
            CommandReviewNote.review_note_at_path_from_selection(
                self.assembly, "Box.Face6"
            ),
            "Assembly.Box.Face6",
            operation,
        )
        self.assertIsNone(
            CommandReviewNote.review_note_at_path_from_selection(self.box, ""),
            operation,
        )
        self.assertIsNone(
            CommandReviewNote.review_note_at_path_from_selection(self.assembly, "Box"),
            operation,
        )

    def test_at_suggestions_exact_subelement_beyond_preview_limit(self):
        operation = "@ suggestions offer Face126/Edge/Vertex beyond preview limit"
        _msg("  Test '{}'".format(operation))

        # 25 boxes → 150 faces / 300 edges / 200 vertices — past the Face60 preview cap.
        solids = [Part.makeBox(1, 1, 1, App.Vector(i * 2, 0, 0)) for i in range(25)]
        many = self.doc.addObject("Part::Feature", "CaseSnapWindowRightPocket")
        many.Shape = Part.makeCompound(solids)
        # Nested path matching the reported case layout.
        assembly_case = self.doc.addObject("App::Part", "AssemblyCase")
        assembly_case.addObject(many)
        self.doc.recompute()

        self.assertGreaterEqual(len(many.Shape.Faces), 126, operation)
        self.assertGreaterEqual(len(many.Shape.Edges), 70, operation)
        self.assertGreaterEqual(len(many.Shape.Vertexes), 70, operation)

        preview = CommandReviewNote.collect_review_note_at_suggestions(
            self.doc, "AssemblyCase.CaseSnapWindowRightPocket.Face"
        )
        self.assertTrue(
            any(s.endswith(".Face1") for s in preview),
            "{} preview still lists early faces".format(operation),
        )
        self.assertNotIn(
            "AssemblyCase.CaseSnapWindowRightPocket.Face126",
            preview,
            "{} Face126 must stay outside the uncapped-number preview".format(operation),
        )
        face_preview = [s for s in preview if ".Face" in s and s.rsplit(".", 1)[-1][4:].isdigit()]
        self.assertLessEqual(
            len(face_preview),
            60,
            "{} Face preview stays capped for performance".format(operation),
        )

        face126 = CommandReviewNote.collect_review_note_at_suggestions(
            self.doc, "AssemblyCase.CaseSnapWindowRightPocket.Face126"
        )
        self.assertIn(
            "AssemblyCase.CaseSnapWindowRightPocket.Face126",
            face126,
            "{} typed Face126 must be offered".format(operation),
        )
        self.assertEqual(
            CommandReviewNote._exact_shape_element_if_valid(many, "Face126"),
            "Face126",
            operation,
        )
        self.assertIsNone(
            CommandReviewNote._exact_shape_element_if_valid(many, "Face9999"),
            "{} missing Face9999".format(operation),
        )

        edge70 = CommandReviewNote.collect_review_note_at_suggestions(
            self.doc, "AssemblyCase.CaseSnapWindowRightPocket.Edge70"
        )
        self.assertIn(
            "AssemblyCase.CaseSnapWindowRightPocket.Edge70",
            edge70,
            "{} typed Edge70 must be offered".format(operation),
        )
        vertex70 = CommandReviewNote.collect_review_note_at_suggestions(
            self.doc, "AssemblyCase.CaseSnapWindowRightPocket.Vertex70"
        )
        self.assertIn(
            "AssemblyCase.CaseSnapWindowRightPocket.Vertex70",
            vertex70,
            "{} typed Vertex70 must be offered".format(operation),
        )

        # Direct object path (no nesting) still works for high-index faces.
        direct = CommandReviewNote.collect_review_note_at_suggestions(
            self.doc, "CaseSnapWindowRightPocket.Face126"
        )
        self.assertIn("CaseSnapWindowRightPocket.Face126", direct, operation)

        # Invalid high index must not be invented.
        missing = CommandReviewNote.collect_review_note_at_suggestions(
            self.doc, "CaseSnapWindowRightPocket.Face9999"
        )
        self.assertNotIn("CaseSnapWindowRightPocket.Face9999", missing, operation)

    def test_leader_port_undo_redo_and_boundary(self):
        operation = "LeaderPort property undo/redo and nearest-border endpoint"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Port"], text_offset=App.Vector(40, 0, 0), open_transaction=False
        )
        self.assertLess(note.LeaderPort, 0.0, "{} default auto".format(operation))

        self.doc.openTransaction("Set leader port")
        note.LeaderPort = 0.0
        self.doc.commitTransaction()
        self.assertAlmostEqual(note.LeaderPort, 0.0, places=5, msg=operation)

        self.doc.undo()
        self.assertLess(note.LeaderPort, 0.0, "{} undo to auto".format(operation))
        self.doc.redo()
        self.assertAlmostEqual(note.LeaderPort, 0.0, places=5, msg="{} redo".format(operation))

        # Boundary endpoint must leave the text/image center for a non-trivial offset.
        half_w, half_h = CommandReviewNote.review_note_label_half_extents(120, 40, font_size=10)
        # Nearest border along the ray toward the base (axis-aligned clip).
        auto = CommandReviewNote.review_note_auto_boundary_endpoint(
            note.TextPosition, half_w, half_h
        )
        self.assertFalse(
            auto.isEqual(note.TextPosition, 1e-6),
            "{} auto endpoint must leave text center".format(operation),
        )
        # TextOffset is +X → attach on the left face (border intersection).
        self.assertAlmostEqual(auto.y, note.TextPosition.y, places=5, msg=operation)
        self.assertLess(auto.x, note.TextPosition.x, operation)
        self.assertAlmostEqual(auto.x, note.TextPosition.x - half_w, places=5, msg=operation)

        # Diagonal offset: endpoint sits on whichever edge is hit first — not forced to a side midpoint.
        diag_text = App.Vector(40, 20, 0)
        diag = CommandReviewNote.review_note_auto_boundary_endpoint(diag_text, half_w, half_h)
        off = App.Vector(diag.x - diag_text.x, diag.y - diag_text.y, 0)
        on_vertical = abs(abs(off.x) - half_w) < 1e-5
        on_horizontal = abs(abs(off.y) - half_h) < 1e-5
        self.assertTrue(
            on_vertical or on_horizontal,
            "{} diagonal must hit a box edge (got {})".format(operation, diag),
        )
        # Stay on the ray from text center toward the base (2D cross ≈ 0).
        toward = diag_text * (-1.0)
        self.assertAlmostEqual(
            off.x * toward.y - off.y * toward.x,
            0.0,
            places=4,
            msg="{} diagonal must stay on the ray toward base".format(operation),
        )
        # Pure vertical / opposite / near-zero offsets.
        up = CommandReviewNote.review_note_auto_boundary_endpoint(App.Vector(0, 40, 0), half_w, half_h)
        self.assertAlmostEqual(up.x, 0.0, places=5, msg=operation)
        self.assertAlmostEqual(up.y, 40.0 - half_h, places=5, msg=operation)
        left = CommandReviewNote.review_note_auto_boundary_endpoint(
            App.Vector(-30, 0, 0), half_w, half_h
        )
        self.assertAlmostEqual(left.x, -30.0 + half_w, places=5, msg=operation)
        origin = CommandReviewNote.review_note_auto_boundary_endpoint(App.Vector(), half_w, half_h)
        self.assertAlmostEqual(origin.x, -half_w, places=5, msg=operation)
    # --- Attachment correctness regressions -----------------------------

    def test_reject_assembly_lcs_origin_group_targets(self):
        operation = "Reject owning Assembly, LCS/origin/datum/group targets"
        _msg("  Test '{}'".format(operation))

        self.assertIsNone(
            CommandReviewNote.normalize_review_note_target(self.assembly, self.assembly, ""),
            "{} owning Assembly".format(operation),
        )
        self.assertIsNone(
            CommandReviewNote.normalize_review_note_target(
                self.assembly, self.assembly, "Face1"
            ),
            "{} Assembly face".format(operation),
        )
        # Assembly-rooted subpath into a child component must still resolve.
        resolved = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.assembly, "Box.Face6", picked_point=App.Vector(5, 10, 30)
        )
        self.assertIsNotNone(resolved, "{} Assembly.Box.Face6".format(operation))
        self.assertEqual(resolved["target_obj"], self.box, operation)
        self.assertEqual(resolved["sub_list"], ["Face6"], operation)

        origin = self.assembly.Origin
        self.assertIsNotNone(origin, operation)
        self.assertIsNone(
            CommandReviewNote.normalize_review_note_target(self.assembly, origin, ""),
            "{} Origin".format(operation),
        )
        for feat in origin.OriginFeatures:
            self.assertIsNone(
                CommandReviewNote.normalize_review_note_target(self.assembly, feat, ""),
                "{} Origin feature {}".format(operation, feat.Name),
            )

        self.assertIsNone(
            CommandReviewNote.normalize_review_note_target(
                self.assembly, self.jointgroup, ""
            ),
            "{} JointGroup".format(operation),
        )

        # Reject LCS when the type is available in this FreeCAD build.
        try:
            lcs = self.assembly.newObject("App::LocalCoordinateSystem", "ReviewNoteLCS")
            self.doc.recompute()
            self.assertIsNone(
                CommandReviewNote.normalize_review_note_target(self.assembly, lcs, ""),
                "{} LCS".format(operation),
            )
        except Exception:
            pass

        # Malformed / unsupported subpaths.
        self.assertIsNone(
            CommandReviewNote.normalize_review_note_target(
                self.assembly, self.box, "Solid1"
            ),
            "{} Solid1".format(operation),
        )
        self.assertIsNone(
            CommandReviewNote.normalize_review_note_target(
                self.assembly, self.box, "Face"
            ),
            "{} Face without index".format(operation),
        )
        self.assertIsNone(
            CommandReviewNote.normalize_review_note_target(
                self.assembly, self.box, "Face6.Extra"
            ),
            "{} malformed Face6.Extra".format(operation),
        )

    def test_linked_and_nested_invalid_face_is_broken(self):
        operation = "Linked and nested Face99 are broken attachments"
        _msg("  Test '{}'".format(operation))

        source = self.doc.addObject("Part::Box", "LinkSource")
        source.Length = 10
        source.Width = 20
        source.Height = 30
        link = self.assembly.newObject("App::Link", "LinkedBox")
        link.LinkedObject = source
        self.doc.recompute()

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, link, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        self.assertIsNotNone(data, operation)
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Link face"], open_transaction=False
        )
        last_base = App.Vector(note.BasePosition)
        note.Target = (link, ["Face99"])
        note.refreshBasePosition()
        self.assertTrue(note.isAttachmentBroken(), "{} linked Face99".format(operation))
        self.assertTrue(note.AttachmentBroken, "{} linked AttachmentBroken".format(operation))
        self.assertTrue(note.BasePosition.isEqual(last_base, 1e-6), operation)

        nested = self.assembly.newObject("Assembly::AssemblyObject", "NestedAsm")
        inner = nested.newObject("Part::Box", "NestedBox")
        inner.Length = 10
        inner.Width = 20
        inner.Height = 30
        self.doc.recompute()
        ndata = CommandReviewNote.normalize_review_note_target(
            nested, inner, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        nnote = CommandReviewNote.create_review_note(
            nested, ndata, ["Nested face"], open_transaction=False
        )
        n_base = App.Vector(nnote.BasePosition)
        nnote.Target = (inner, ["Face99"])
        nnote.refreshBasePosition()
        self.assertTrue(nnote.isAttachmentBroken(), "{} nested Face99".format(operation))
        self.assertTrue(nnote.BasePosition.isEqual(n_base, 1e-6), operation)

    def test_foreign_joint_reference_is_broken(self):
        operation = "Foreign/removed joint references break attachment"
        _msg("  Test '{}'".format(operation))
        joint, box2 = self._make_joint()

        data = CommandReviewNote.normalize_review_note_target(self.assembly, joint, "")
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Joint foreign"], open_transaction=False
        )
        last_base = App.Vector(note.BasePosition)
        self.assertFalse(note.isAttachmentBroken(), operation)

        other = self.doc.addObject("Assembly::AssemblyObject", "OtherAssembly")
        foreign = other.newObject("Part::Box", "ForeignBox")
        self.doc.recompute()
        joint.Reference1 = [foreign, ["Face6"]]
        note.refreshBasePosition()
        self.assertTrue(note.isAttachmentBroken(), "{} foreign ref".format(operation))
        self.assertTrue(note.AttachmentBroken, operation)
        self.assertTrue(note.BasePosition.isEqual(last_base, 1e-6), operation)

        # Restore then remove the referenced component from the owning Assembly.
        joint.Reference1 = [self.box, ["Face6", "Vertex7"]]
        note.refreshBasePosition()
        self.assertFalse(note.isAttachmentBroken(), operation)
        last_base = App.Vector(note.BasePosition)
        self.assembly.removeObject(box2)
        joint.Reference2 = [box2, ["Face6"]]
        note.JointSide = "Reference2"
        note.refreshBasePosition()
        self.assertTrue(note.isAttachmentBroken(), "{} removed component".format(operation))
        self.assertTrue(note.BasePosition.isEqual(last_base, 1e-6), operation)

    def test_attachment_broken_property_notifies_icon_refresh(self):
        operation = "AttachmentBroken flips and drives icon refresh"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Icon notify"], open_transaction=False
        )
        note.refreshBasePosition()
        self.assertFalse(note.AttachmentBroken, operation)
        self.assertFalse(note.isAttachmentBroken(), operation)

        note.Target = (self.box, ["Face99"])
        note.refreshBasePosition()
        self.assertTrue(note.AttachmentBroken, operation)
        self.assertTrue(note.isAttachmentBroken(), operation)

        note.Target = (self.box, ["Face6"])
        note.refreshBasePosition()
        self.assertFalse(note.AttachmentBroken, "{} recovered".format(operation))
        self.assertFalse(note.isAttachmentBroken(), "{} recovered".format(operation))

        if App.GuiUp and note.ViewObject:
            # Broken then recovered icons must be obtainable without throwing.
            note.Target = (self.box, ["Face99"])
            note.refreshBasePosition()
            broken_icon = note.ViewObject.Icon
            note.Target = (self.box, ["Face6"])
            note.refreshBasePosition()
            open_icon = note.ViewObject.Icon
            self.assertIsNotNone(broken_icon, operation)
            self.assertIsNotNone(open_icon, operation)

    # --- P0 regressions (continued) ------------------------------------

    def test_world_pick_on_placed_assembly(self):
        operation = "World pick on translated top-level Assembly"
        _msg("  Test '{}'".format(operation))

        self.assembly.Placement = App.Placement(App.Vector(100, 0, 0), App.Rotation())
        local_pt = App.Vector(5, 10, 30)
        world_pt = self.assembly.Placement.multVec(self.box.Placement.multVec(local_pt))

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=world_pt
        )
        self.assertIsNotNone(data, operation)
        self.assertTrue(
            data["local_anchor"].isEqual(local_pt, 1e-5),
            "{}: local_anchor {} != {}".format(operation, data["local_anchor"], local_pt),
        )

        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Placed pick"], open_transaction=False
        )
        expected_base = self.box.Placement.multVec(local_pt)
        self.assertTrue(
            note.BasePosition.isEqual(expected_base, 1e-5),
            "{}: BasePosition {} != {}".format(operation, note.BasePosition, expected_base),
        )

    def test_world_pick_on_placed_nested_assembly(self):
        operation = "World pick on translated/rotated nested Assembly"
        _msg("  Test '{}'".format(operation))

        self.assembly.Placement = App.Placement(
            App.Vector(100, 0, 0), App.Rotation(0, 90, 0)
        )
        nested = self.assembly.newObject("Assembly::AssemblyObject", "Nested")
        nested.Placement = App.Placement(App.Vector(10, 20, 30), App.Rotation(0, 0, 45))
        inner = nested.newObject("Part::Box", "InnerBox")
        inner.Length = 10
        inner.Width = 20
        inner.Height = 30
        inner.Placement = App.Placement(App.Vector(1, 2, 3), App.Rotation())
        self.doc.recompute()

        local_pt = App.Vector(5, 10, 30)
        world_pt = self.assembly.Placement.multVec(
            nested.Placement.multVec(inner.Placement.multVec(local_pt))
        )

        data = CommandReviewNote.normalize_review_note_target(
            nested, inner, "Face6", picked_point=world_pt
        )
        self.assertIsNotNone(data, operation)
        self.assertTrue(
            data["local_anchor"].isEqual(local_pt, 1e-5),
            "{}: local_anchor {} != {}".format(operation, data["local_anchor"], local_pt),
        )

        note = CommandReviewNote.create_review_note(
            nested, data, ["Nested pick"], open_transaction=False
        )
        expected_base = inner.Placement.multVec(local_pt)
        self.assertTrue(
            note.BasePosition.isEqual(expected_base, 1e-5),
            "{}: BasePosition {} != {}".format(operation, note.BasePosition, expected_base),
        )

    def test_collect_joint_main_pick_chooses_reference2(self):
        operation = "3D joint Main selection chooses Reference2"
        _msg("  Test '{}'".format(operation))
        joint, _box2 = self._make_joint()

        # Selection shape produced by SoFCSelection with synthetic Main.
        near_ref2 = App.Vector(joint.Placement2.Base)
        sel = [_FakeSel(joint, ["Main"], [near_ref2])]
        targets = CommandReviewNote.collect_review_note_targets(self.assembly, sel)
        self.assertEqual(len(targets), 1, operation)
        self.assertEqual(targets[0]["joint_side"], "Reference2", operation)
        self.assertEqual(targets[0]["sub_list"], ["Main"], operation)

        if App.GuiUp and joint.ViewObject and getattr(joint.ViewObject, "Proxy", None):
            proxy = joint.ViewObject.Proxy
            if hasattr(proxy, "display_mode"):
                mode = proxy.display_mode
                self.assertEqual(mode.subElementName.getValue(), "Main", operation)

    def test_invalidated_subelements_and_joint_refs(self):
        operation = "Invalidated Face/Edge/Vertex and joint refs stay broken"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Sub check"], open_transaction=False
        )
        last_base = App.Vector(note.BasePosition)

        note.Target = (self.box, ["Face99"])
        self.assertTrue(note.isAttachmentBroken(), "{} face".format(operation))
        self.assertTrue(note.BasePosition.isEqual(last_base, 1e-6), operation)

        note.Target = (self.box, ["Edge99"])
        self.assertTrue(note.isAttachmentBroken(), "{} edge".format(operation))

        note.Target = (self.box, ["Vertex99"])
        self.assertTrue(note.isAttachmentBroken(), "{} vertex".format(operation))

        # Restore valid geometry target, then break joint connector side.
        note.Target = (self.box, ["Face6"])
        note.JointSide = "None"
        self.assertFalse(note.isAttachmentBroken())

        joint, box2 = self._make_joint()
        jdata = CommandReviewNote.normalize_review_note_target(self.assembly, joint, "")
        jnote = CommandReviewNote.create_review_note(
            self.assembly, jdata, ["Joint sub"], open_transaction=False
        )
        j_base = App.Vector(jnote.BasePosition)
        joint.Reference1 = [box2, ["Face99"]]
        self.assertTrue(jnote.isAttachmentBroken(), "{} joint ref".format(operation))
        self.assertTrue(jnote.BasePosition.isEqual(j_base, 1e-6), operation)

    def test_foreign_and_removed_component_targets(self):
        operation = "Foreign targets and removed components rejected/broken"
        _msg("  Test '{}'".format(operation))

        other_asm = self.doc.addObject("Assembly::AssemblyObject", "OtherAsm")
        foreign = other_asm.newObject("Part::Box", "ForeignBox")
        self.doc.recompute()
        self.assertIsNone(
            CommandReviewNote.normalize_review_note_target(self.assembly, foreign, "Face1"),
            operation,
        )
        self.assertIsNone(
            CommandReviewNote.normalize_review_note_target(
                self.assembly, foreign, "Face1", picked_point=App.Vector(1, 1, 1)
            ),
            operation,
        )

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Membership"], open_transaction=False
        )
        last_base = App.Vector(note.BasePosition)

        self.assembly.removeObject(self.box)
        self.assertTrue(note.isAttachmentBroken(), operation)
        self.assertTrue(note.BasePosition.isEqual(last_base, 1e-6), operation)
        self.assertIsNone(
            CommandReviewNote.normalize_review_note_target(self.assembly, self.box, "Face6"),
            operation,
        )

    def test_app_level_assembly_deletion_cleans_notes(self):
        operation = "App-level Assembly deletion removes notes and group"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(self.assembly, self.box, "")
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Delete asm"], open_transaction=False
        )
        groups = [o for o in self.assembly.OutList if o.TypeId == "Assembly::ReviewNoteGroup"]
        self.assertEqual(len(groups), 1)
        note_name = note.Name
        group_name = groups[0].Name
        asm_name = self.assembly.Name

        self.doc.removeObject(asm_name)

        self.assertIsNone(self.doc.getObject(note_name), operation)
        self.assertIsNone(self.doc.getObject(group_name), operation)
        self.assertIsNone(self.doc.getObject(asm_name), operation)

        # Recreate assembly so tearDown can close a normal document.
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.jointgroup = self.assembly.newObject("Assembly::JointGroup", "Joints")
        self.box = self.assembly.newObject("Part::Box", "Box")
        self.doc.recompute()

    def test_double_click_edit_dispatch(self):
        operation = "Double-click dispatches to edit_review_note"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(self.assembly, self.box, "")
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Before"], open_transaction=False
        )

        # ViewProviderReviewNote.doubleClicked must call this Python entry point.
        self.assertTrue(hasattr(CommandReviewNote, "edit_review_note"), operation)
        self.assertFalse(
            hasattr(CommandReviewNote, "editReviewNote"),
            "{}: legacy camelCase alias must not be required".format(operation),
        )

        ok = CommandReviewNote.edit_review_note(note, ["After double-click"])
        self.assertTrue(ok, operation)
        self.assertEqual(list(note.LabelText), ["After double-click"], operation)

    def test_camera_offset_uses_assembly_local_axes(self):
        operation = "Camera text offset transformed into Assembly-local space"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(self.assembly, self.box, "")
        self.assembly.Placement = App.Placement()
        identity_offset = CommandReviewNote._initial_text_offset(
            self.assembly,
            data,
            camera_direction=App.Vector(0, 0, -1),
            camera_up=App.Vector(0, 1, 0),
        )
        self.assembly.Placement = App.Placement(
            App.Vector(0, 0, 0), App.Rotation(90, 0, 0)
        )
        rotated_offset = CommandReviewNote._initial_text_offset(
            self.assembly,
            data,
            camera_direction=App.Vector(0, 0, -1),
            camera_up=App.Vector(0, 1, 0),
        )
        self.assertFalse(
            rotated_offset.isEqual(identity_offset, 1e-6),
            "{}: expected rotated assembly to change local offset ({} vs {})".format(
                operation, rotated_offset, identity_offset
            ),
        )

    # --- P1 coverage ----------------------------------------------------

    def test_linked_component_attachment(self):
        operation = "Linked component attachment and tracking"
        _msg("  Test '{}'".format(operation))

        source = self.doc.addObject("Part::Box", "SourceBox")
        source.Length = 10
        source.Width = 20
        source.Height = 30
        link = self.assembly.newObject("App::Link", "LinkedBox")
        link.LinkedObject = source
        self.doc.recompute()

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, link, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        self.assertIsNotNone(data, operation)
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Linked"], open_transaction=False
        )
        self.assertTrue(note.BasePosition.isEqual(App.Vector(5, 10, 30), 1e-5), operation)

        link.Placement = App.Placement(App.Vector(0, 0, 40), App.Rotation())
        self.assertTrue(
            note.BasePosition.isEqual(App.Vector(5, 10, 70), 1e-5),
            "{}: got {}".format(operation, note.BasePosition),
        )

    def test_link_element_target_follows_parent_link(self):
        operation = "LinkElement note follows parent Link Placement (FreeCAD#16113)"
        _msg("  Test '{}'".format(operation))

        source = self.doc.addObject("Part::Box", "SourceArray")
        source.Length = 10
        source.Width = 20
        source.Height = 30
        link = self.assembly.newObject("App::Link", "ArrayLink")
        link.LinkedObject = source
        link.ElementCount = 2
        self.doc.recompute()

        elements = list(getattr(link, "ElementList", []) or [])
        self.assertGreaterEqual(len(elements), 1, operation)
        elem = elements[0]
        self.assertEqual(elem.TypeId, "App::LinkElement", operation)
        self.assertFalse(
            self.assembly.hasObject(elem, True),
            "{}: precondition — hasObject must miss LinkElements".format(operation),
        )

        # Selection-style path: Assembly + LinkElement name.
        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.assembly, elem.Name, picked_point=App.Vector(5, 10, 15)
        )
        self.assertIsNotNone(data, "{}: normalize Assembly+LinkElement".format(operation))
        self.assertEqual(data["target_obj"], elem, operation)

        # Direct LinkElement root must also be accepted.
        data_direct = CommandReviewNote.normalize_review_note_target(
            self.assembly, elem, "", picked_point=App.Vector(5, 10, 15)
        )
        self.assertIsNotNone(data_direct, "{}: normalize LinkElement root".format(operation))

        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["LinkElement"], open_transaction=False
        )
        self.assertFalse(note.isAttachmentBroken(), operation)
        self.assertEqual(note.Target[0], elem, operation)

        before = App.Vector(note.BasePosition)
        link.Placement = App.Placement(App.Vector(100, 0, 0), App.Rotation())
        expected = link.Placement.multVec(elem.Placement.multVec(note.LocalAnchor))
        self.assertTrue(
            note.BasePosition.isEqual(expected, 1e-5),
            "{}: got {} expected {} (before {})".format(
                operation, note.BasePosition, expected, before
            ),
        )

    def test_part_owner_occurrence_path_not_remapped(self):
        operation = "App::Part owner keeps Link occurrence (not linked twin)"
        _msg("  Test '{}'".format(operation))

        owner = self.doc.addObject("App::Part", "PartAssembly")
        twin = self.doc.addObject("Part::Box", "SimSpool")
        twin.Length = 40
        twin.Width = 40
        twin.Height = 20
        occ_a = owner.newObject("App::Link", "AssemblySpool")
        occ_a.LinkedObject = twin
        occ_a.Placement = App.Placement(App.Vector(100, 0, 0), App.Rotation())
        occ_b = owner.newObject("App::Link", "OtherSpool")
        occ_b.LinkedObject = twin
        occ_b.Placement = App.Placement(App.Vector(-100, 0, 0), App.Rotation())

        gear = self.doc.addObject("App::Part", "SimGearPart")
        tip = gear.newObject("Part::Box", "Spool_GearTipRelief")
        tip.Length = 10
        tip.Width = 20
        tip.Height = 30
        occ_gear = owner.newObject("App::Link", "AssemblySpoolGear")
        occ_gear.LinkedObject = gear
        occ_gear.Placement = App.Placement(App.Vector(0, 50, 0), App.Rotation())
        self.doc.recompute()

        sub = "AssemblySpoolGear.Spool_GearTipRelief.Face6"
        data = CommandReviewNote.normalize_review_note_target(
            owner, owner, sub, picked_point=App.Vector(5, 60, 30)
        )
        self.assertIsNotNone(data, operation)
        self.assertEqual(data["target_obj"], occ_gear, "{} target occurrence".format(operation))
        self.assertEqual(
            list(data["sub_list"]),
            ["Spool_GearTipRelief.Face6"],
            "{} relative sub".format(operation),
        )
        self.assertNotEqual(data["target_obj"].Name, "SimSpool", operation)
        self.assertNotEqual(data["target_obj"].Name, "Spool_GearTipRelief", operation)

        note = CommandReviewNote.create_review_note(
            owner, data, ["Part occurrence"], open_transaction=False
        )
        self.assertIsNotNone(note, operation)
        self.assertEqual(note.getOwnerPart(), owner, operation)
        self.assertIsNone(note.getAssembly(), "{} plain Part has no AssemblyObject".format(operation))
        groups = [o for o in owner.Group if o.TypeId == "Assembly::ReviewNoteGroup"]
        self.assertEqual(len(groups), 1, operation)
        self.assertIn(note, groups[0].Group, operation)
        self.assertFalse(note.isAttachmentBroken(), operation)

        before = App.Vector(note.BasePosition)
        occ_gear.Placement = App.Placement(App.Vector(0, 150, 0), App.Rotation())
        expected = occ_gear.Placement.multVec(note.LocalAnchor)
        self.assertTrue(
            note.BasePosition.isEqual(expected, 1e-5),
            "{} follow: got {} expected {} (before {})".format(
                operation, note.BasePosition, expected, before
            ),
        )

        note.Target = (occ_gear, ["Spool_GearTipRelief.Face999"])
        note.refreshBasePosition()
        self.assertTrue(note.isAttachmentBroken(), "{} missing face".format(operation))

        self.assertEqual(
            CommandReviewNote.find_review_note_owner(owner, "AssemblySpool.Face6"),
            owner,
            operation,
        )

    def test_observer_survives_assembly_create_undo(self):
        operation = "Observer revoked on undo of Assembly create (no UAF)"
        _msg("  Test '{}'".format(operation))

        self.doc.openTransaction("Create assembly with note")
        asm = self.doc.addObject("Assembly::AssemblyObject", "AsmUndoCreate")
        asm.newObject("Assembly::JointGroup", "Joints")
        box = asm.newObject("Part::Box", "BoxUndo")
        box.Length = 10
        box.Width = 20
        box.Height = 30
        self.doc.recompute()
        data = CommandReviewNote.normalize_review_note_target(
            asm, box, "", picked_point=App.Vector(1, 2, 3)
        )
        CommandReviewNote.create_review_note(
            asm, data, ["Undo create"], open_transaction=False
        )
        self.doc.commitTransaction()

        self.doc.undo()
        self.assertIsNone(self.doc.getObject("AsmUndoCreate"), operation)

        # Recreate and move — must not SIGSEGV from a dangling tracker.
        self.doc.openTransaction("Recreate")
        asm2 = self.doc.addObject("Assembly::AssemblyObject", "AsmUndoCreate")
        asm2.newObject("Assembly::JointGroup", "Joints")
        box2 = asm2.newObject("Part::Box", "BoxUndo")
        box2.Length = 10
        box2.Width = 20
        box2.Height = 30
        self.doc.recompute()
        data2 = CommandReviewNote.normalize_review_note_target(
            asm2, box2, "", picked_point=App.Vector(1, 2, 3)
        )
        note2 = CommandReviewNote.create_review_note(
            asm2, data2, ["After undo"], open_transaction=False
        )
        self.doc.commitTransaction()
        box2.Placement = App.Placement(App.Vector(10, 0, 0), App.Rotation())
        self.assertTrue(
            note2.BasePosition.isEqual(App.Vector(11, 2, 3), 1e-6),
            "{}: tracking after recreate got {}".format(operation, note2.BasePosition),
        )

    def test_observer_reinstalled_after_assembly_delete_undo(self):
        operation = "Observer reinstalled after undo of Assembly deletion"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "", picked_point=App.Vector(2, 3, 4)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Delete undo"], open_transaction=False
        )
        asm_name = self.assembly.Name
        note_name = note.Name

        self.doc.openTransaction("Delete assembly")
        self.doc.removeObject(asm_name)
        self.doc.commitTransaction()
        self.assertIsNone(self.doc.getObject(asm_name), operation)

        self.doc.undo()
        asm = self.doc.getObject(asm_name)
        note = self.doc.getObject(note_name)
        self.assertIsNotNone(asm, operation)
        self.assertIsNotNone(note, operation)
        box = self.doc.getObject("Box")
        self.assertIsNotNone(box, operation)

        box.Placement = App.Placement(App.Vector(0, 0, 25), App.Rotation())
        expected = box.Placement.multVec(App.Vector(2, 3, 4))
        self.assertTrue(
            note.BasePosition.isEqual(expected, 1e-6),
            "{}: got {} expected {}".format(operation, note.BasePosition, expected),
        )

    def test_reference2_joint_tracking(self):
        operation = "Reference2 joint note tracks Placement2 / part move"
        _msg("  Test '{}'".format(operation))
        joint, box2 = self._make_joint()

        near_ref2 = App.Vector(joint.Placement2.Base)
        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, joint, "Main", picked_point=near_ref2
        )
        self.assertEqual(data["joint_side"], "Reference2", operation)
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Ref2"], open_transaction=False
        )
        expected = App.Vector(joint.Placement2.Base)
        self.assertTrue(note.BasePosition.isEqual(expected, 1e-6), operation)

        joint.Placement2 = App.Placement(App.Vector(2, 3, 4), App.Rotation())
        self.assertTrue(note.BasePosition.isEqual(App.Vector(2, 3, 4), 1e-6), operation)

        box2.Placement = App.Placement(App.Vector(10, 0, 0), App.Rotation())
        self.assertTrue(note.BasePosition.isEqual(App.Vector(12, 3, 4), 1e-6), operation)

    def test_solver_style_move_keeps_tracking(self):
        operation = "Solver-style Placement update keeps note tracking"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "", picked_point=App.Vector(5, 10, 15)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Solver"], open_transaction=False
        )

        # Mimic solver: set Placement then purgeTouched, optionally solve.
        self.box.Placement = App.Placement(App.Vector(0, 50, 0), App.Rotation(0, 0, 45))
        self.box.purgeTouched()
        if hasattr(self.assembly, "solve"):
            self.assembly.solve(False)

        expected = self.box.Placement.multVec(App.Vector(5, 10, 15))
        self.assertTrue(
            note.BasePosition.isEqual(expected, 1e-5),
            "{}: got {} expected {}".format(operation, note.BasePosition, expected),
        )

    def test_ordinary_note_and_assembly_deletion(self):
        operation = "Ordinary note deletion and Assembly deletion cleanup"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(self.assembly, self.box, "")
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Delete me"], open_transaction=True
        )
        groups = [o for o in self.assembly.OutList if o.TypeId == "Assembly::ReviewNoteGroup"]
        self.assertEqual(len(groups), 1)
        group = groups[0]
        note_name = note.Name

        self.doc.openTransaction("Delete note")
        self.doc.removeObject(note_name)
        self.doc.commitTransaction()
        self.assertIsNone(self.doc.getObject(note_name), operation)
        # Group remains when other notes could still be added.
        self.assertIsNotNone(self.doc.getObject(group.Name), operation)

        self.doc.undo()
        self.assertIsNotNone(self.doc.getObject(note_name), operation)

        # Full Assembly deletion removes group + notes (App path).
        note2 = CommandReviewNote.create_review_note(
            self.assembly,
            CommandReviewNote.normalize_review_note_target(self.assembly, self.box, ""),
            ["Also"],
            open_transaction=False,
        )
        group_name = group.Name
        note2_name = note2.Name
        asm_name = self.assembly.Name
        self.doc.removeObject(asm_name)
        self.assertIsNone(self.doc.getObject(group_name), operation)
        self.assertIsNone(self.doc.getObject(note_name), operation)
        self.assertIsNone(self.doc.getObject(note2_name), operation)

        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.jointgroup = self.assembly.newObject("Assembly::JointGroup", "Joints")
        self.box = self.assembly.newObject("Part::Box", "Box")
        self.doc.recompute()

    def test_undo_redo_mutations(self):
        operation = "Undo/redo for create, edit, resolve, drag"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(self.assembly, self.box, "")
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Original"], open_transaction=True
        )
        note_name = note.Name

        CommandReviewNote.edit_review_note(note, ["Edited"])
        self.assertEqual(list(note.LabelText), ["Edited"], operation)
        self.doc.undo()
        self.assertEqual(list(note.LabelText), ["Original"], operation)
        self.doc.redo()
        self.assertEqual(list(note.LabelText), ["Edited"], operation)

        CommandReviewNote.toggle_resolve_review_note(note)
        self.assertTrue(note.Resolved, operation)
        self.doc.undo()
        self.assertFalse(note.Resolved, operation)
        self.doc.redo()
        self.assertTrue(note.Resolved, operation)

        self.doc.openTransaction("Drag label")
        note.TextPosition = App.Vector(40, 50, 60)
        self.doc.commitTransaction()
        self.doc.undo()
        self.assertFalse(note.TextPosition.isEqual(App.Vector(40, 50, 60), 1e-6), operation)
        self.doc.redo()
        self.assertTrue(note.TextPosition.isEqual(App.Vector(40, 50, 60), 1e-6), operation)

        # Undo back through creation removes note + group.
        while self.doc.getObject(note_name):
            self.doc.undo()
        notes = [o for o in self.doc.Objects if o.TypeId == "Assembly::ReviewNote"]
        groups = [o for o in self.doc.Objects if o.TypeId == "Assembly::ReviewNoteGroup"]
        self.assertEqual(len(notes), 0, operation)
        self.assertEqual(len(groups), 0, operation)

        self.doc.redo()
        self.assertIsNotNone(self.doc.getObject(note_name), operation)

    def test_recompute_refreshes_dependent_notes(self):
        operation = "Recompute of target refreshes note BasePosition"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "", picked_point=App.Vector(5, 10, 15)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Recompute"], open_transaction=False
        )
        # Bypass the Placement signal path: mutate Placement quietly is hard, so
        # set Placement then force a document recompute and ensure note stays aligned.
        self.box.Placement = App.Placement(App.Vector(0, 0, 25), App.Rotation())
        self.doc.recompute()
        expected = self.box.Placement.multVec(App.Vector(5, 10, 15))
        self.assertTrue(
            note.BasePosition.isEqual(expected, 1e-6),
            "{}: got {} expected {}".format(operation, note.BasePosition, expected),
        )

    def test_visibility_undo_redo(self):
        operation = "Visibility undo/redo"
        _msg("  Test '{}'".format(operation))
        data = CommandReviewNote.normalize_review_note_target(self.assembly, self.box, "")
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Vis"], open_transaction=False
        )
        if not note.ViewObject:
            self.skipTest("ViewObject unavailable in this FreeCADCmd config")

        self.assertTrue(note.ViewObject.Visibility, operation)
        self.doc.openTransaction("Hide note")
        note.ViewObject.Visibility = False
        self.doc.commitTransaction()
        self.assertFalse(note.ViewObject.Visibility, operation)
        self.doc.undo()
        self.assertTrue(note.ViewObject.Visibility, operation)
        self.doc.redo()
        self.assertFalse(note.ViewObject.Visibility, operation)

    def test_note_target_deletion_redo(self):
        operation = "Note and target deletion redo"
        _msg("  Test '{}'".format(operation))
        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Del redo"], open_transaction=True
        )
        note_name = note.Name
        last_base = App.Vector(note.BasePosition)

        self.doc.openTransaction("Delete note")
        self.doc.removeObject(note_name)
        self.doc.commitTransaction()
        self.assertIsNone(self.doc.getObject(note_name), operation)
        self.doc.undo()
        self.assertIsNotNone(self.doc.getObject(note_name), operation)
        self.doc.redo()
        self.assertIsNone(self.doc.getObject(note_name), operation)
        self.doc.undo()  # restore note for target deletion path
        note = self.doc.getObject(note_name)
        self.assertIsNotNone(note, operation)

        box_name = self.box.Name
        self.doc.openTransaction("Delete target")
        self.doc.removeObject(box_name)
        self.doc.commitTransaction()
        note.refreshBasePosition()
        self.assertTrue(note.isAttachmentBroken(), operation)
        self.assertTrue(note.BasePosition.isEqual(last_base, 1e-6), operation)
        self.doc.undo()
        note.refreshBasePosition()
        self.assertFalse(note.isAttachmentBroken(), "{} target undo".format(operation))
        self.doc.redo()
        note.refreshBasePosition()
        self.assertTrue(note.isAttachmentBroken(), "{} target redo".format(operation))

        # Recreate box for tearDown consumers of self.box.
        self.box = self.assembly.newObject("Part::Box", "Box")
        self.box.Length = 10
        self.box.Width = 20
        self.box.Height = 30
        self.doc.recompute()

    def test_whitespace_label_and_fallback_offset(self):
        operation = "Whitespace LabelText clears Label; fallback offset is 20 mm"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(self.assembly, self.box, "")
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Keep"], open_transaction=False
        )
        self.assertEqual(note.Label, "Keep", operation)
        note.LabelText = ["   ", "\t", ""]
        # first non-whitespace line is empty → Label cleared.
        self.assertEqual(note.Label, "", "{} whitespace LabelText".format(operation))

        offset = CommandReviewNote._fallback_text_offset()
        self.assertAlmostEqual(offset.Length, 20.0, places=5, msg=operation)
        expected_dir = App.Vector(1, 1, 1)
        expected_dir.normalize()
        self.assertTrue(offset.isEqual(expected_dir * 20.0, 1e-6), operation)

        no_cam = CommandReviewNote._initial_text_offset(
            self.assembly, data, camera_direction=None, camera_up=None
        )
        # Headless path (no camera) must use the exact 20 mm fallback.
        self.assertTrue(no_cam.isEqual(offset, 1e-6), "{} headless fallback".format(operation))

    def test_populated_review_note_group_assembly_cleanup(self):
        operation = "Populated Review Notes group cleaned with Assembly"
        _msg("  Test '{}'".format(operation))

        for i in range(3):
            data = CommandReviewNote.normalize_review_note_target(self.assembly, self.box, "")
            CommandReviewNote.create_review_note(
                self.assembly, data, ["Note {}".format(i)], open_transaction=False
            )
        groups = [o for o in self.assembly.OutList if o.TypeId == "Assembly::ReviewNoteGroup"]
        self.assertEqual(len(groups), 1, operation)
        self.assertEqual(len(groups[0].Group), 3, operation)
        group_name = groups[0].Name
        note_names = [n.Name for n in groups[0].Group]

        # Legacy plain group must not be treated as the Review Notes container.
        legacy = self.assembly.newObject("App::DocumentObjectGroup", "LegacyReviewNotes")
        legacy.Label = "Review Notes"
        self.assertIs(
            UtilsAssembly.getReviewNoteGroup(self.assembly),
            groups[0],
            "{} ignores legacy DocumentObjectGroup".format(operation),
        )

        asm_name = self.assembly.Name
        self.doc.removeObject(asm_name)
        self.assertIsNone(self.doc.getObject(group_name), operation)
        for name in note_names:
            self.assertIsNone(self.doc.getObject(name), operation)

        # Recreate assembly for tearDown.
        self.assembly = self.doc.addObject("Assembly::AssemblyObject", "Assembly")
        self.jointgroup = self.assembly.newObject("Assembly::JointGroup", "Joints")
        self.box = self.assembly.newObject("Part::Box", "Box")
        self.box.Length = 10
        self.box.Width = 20
        self.box.Height = 30
        self.doc.recompute()

    def test_persistence_visibility_and_joint_side(self):
        operation = "Persistence of visibility and JointSide"
        _msg("  Test '{}'".format(operation))
        joint, _box2 = self._make_joint()

        near_ref2 = App.Vector(joint.Placement2.Base)
        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, joint, "Main", picked_point=near_ref2
        )
        note = CommandReviewNote.create_review_note(
            self.assembly,
            data,
            ["Persist joint"],
            text_offset=App.Vector(7, 8, 9),
            open_transaction=False,
        )
        self.assertEqual(note.JointSide, "Reference2")
        if note.ViewObject:
            note.ViewObject.Visibility = False

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
            note = notes[0]
            self.assertEqual(note.JointSide, "Reference2", operation)
            self.assertTrue(note.TextPosition.isEqual(App.Vector(7, 8, 9), 1e-6), operation)
            if note.ViewObject:
                self.assertFalse(note.ViewObject.Visibility, operation)
        finally:
            if App.ActiveDocument:
                App.closeDocument(App.ActiveDocument.Name)
            self.doc = App.newDocument(self.__class__.__name__)
            App.setActiveDocument(self.doc.Name)
            try:
                os.remove(path)
            except OSError:
                pass


class TestReviewNotesGui(unittest.TestCase):
    """Xvfb/GUI smoke for review notes: menus, edit, icons, visibility, leader."""

    @classmethod
    def setUpClass(cls):
        if not App.GuiUp:
            raise unittest.SkipTest(
                "GUI smoke requires FreeCAD GUI mode (e.g. xvfb-run with FreeCADGui)"
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

        # Activate assembly edit so UtilsAssembly.activeAssembly() works.
        activated = False
        if self.assembly.ViewObject:
            try:
                Gui.ActiveDocument.setEdit(self.assembly)
                activated = bool(self.assembly.ViewObject.isInEditMode())
            except Exception:
                activated = False
            if not activated:
                try:
                    proxy = getattr(self.assembly.ViewObject, "Proxy", None)
                    if proxy and hasattr(proxy, "setEdit"):
                        proxy.setEdit(self.assembly.ViewObject, 0)
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

    def test_gui_commands_and_view_context_eligibility(self):
        operation = "GUI commands registered; Add Note View-eligible"
        _msg("  Test '{}'".format(operation))
        import FreeCADGui as Gui

        for cmd in (
            "Assembly_AddReviewNote",
            "Assembly_EditReviewNote",
            "Assembly_ToggleResolveReviewNote",
        ):
            self.assertIn(cmd, Gui.listCommands(), "{} missing {}".format(operation, cmd))

        Gui.Selection.clearSelection()
        # Prefer Assembly-rooted path — matches typical 3D view picks.
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box.Face6")
        sel = Gui.Selection.getSelectionEx("*", 0)
        self.assertTrue(
            CommandReviewNote.is_add_review_note_eligible(self.assembly, sel),
            "{} eligible sel={!r}".format(
                operation,
                [(s.Object.Name, list(s.SubElementNames)) for s in sel],
            ),
        )

    def test_gui_double_click_edit_visibility_icons_leader(self):
        operation = "Double-click edit, visibility, icons, leader"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Open note"], open_transaction=False
        )
        self.assertIsNotNone(note.ViewObject, operation)

        modes = note.ViewObject.listDisplayModes()
        self.assertTrue(any(m in modes for m in ("Line", "Object")), operation)

        note.Resolved = False
        note.ViewObject.signalChangeIcon()
        note.Resolved = True
        note.ViewObject.signalChangeIcon()
        note.Resolved = False

        note.ViewObject.Visibility = False
        self.assertFalse(note.ViewObject.Visibility, operation)
        note.ViewObject.Visibility = True
        self.assertTrue(note.ViewObject.Visibility, operation)

        # Visibility must participate in undo/redo (plan requirement; headless skips this).
        self.doc.openTransaction("Hide review note")
        note.ViewObject.Visibility = False
        self.doc.commitTransaction()
        self.assertFalse(note.ViewObject.Visibility, operation)
        self.doc.undo()
        self.assertTrue(note.ViewObject.Visibility, "{} visibility undo".format(operation))
        self.doc.redo()
        self.assertFalse(note.ViewObject.Visibility, "{} visibility redo".format(operation))
        note.ViewObject.Visibility = True

        self.assertTrue(note.ViewObject.doubleClicked(), operation)
        import FreeCADGui as Gui

        task = CommandReviewNote.get_active_review_note_task()
        self.assertIsNotNone(task, "{} expected modeless editor".format(operation))
        self.assertTrue(Gui.Control.activeDialog(), operation)
        task.edit.setPlainText("Edited via double-click")
        self.assertTrue(task.accept(), operation)
        self.assertFalse(Gui.Control.activeDialog(), operation)
        self.assertEqual(list(note.LabelText), ["Edited via double-click"], operation)

        self.doc.openTransaction("Drag")
        note.TextPosition = App.Vector(25, 25, 25)
        self.doc.commitTransaction()
        self.assertTrue(note.TextPosition.isEqual(App.Vector(25, 25, 25), 1e-6), operation)
        base_before = App.Vector(note.BasePosition)
        self.box.Placement = App.Placement(App.Vector(0, 0, 10), App.Rotation())
        self.assertFalse(note.BasePosition.isEqual(base_before, 1e-6), operation)
        self.assertTrue(note.TextPosition.isEqual(App.Vector(25, 25, 25), 1e-6), operation)

    def test_gui_rotated_assembly_drag_space(self):
        operation = "Rotated Assembly keeps TextPosition assembly-local after drag set"
        _msg("  Test '{}'".format(operation))

        self.assembly.Placement = App.Placement(App.Vector(10, 20, 30), App.Rotation(0, 0, 90))
        data = CommandReviewNote.normalize_review_note_target(self.assembly, self.box, "")
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Rotated"], open_transaction=False
        )
        note.TextPosition = App.Vector(15, 0, 0)
        expected = self.box.Placement.multVec(note.LocalAnchor)
        self.assertTrue(
            note.BasePosition.isEqual(expected, 1e-5),
            "{}: BasePosition {}".format(operation, note.BasePosition),
        )
        self.assertTrue(note.TextPosition.isEqual(App.Vector(15, 0, 0), 1e-6), operation)

    def test_gui_review_note_group_claims_notes_3d(self):
        operation = "Assembly GeoFeatureGroup 3D-claims review notes"
        _msg("  Test '{}'".format(operation))

        self.assembly.Placement = App.Placement(App.Vector(40, 50, 60), App.Rotation(0, 0, 30))
        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["CS claim"], open_transaction=False
        )
        groups = [o for o in self.assembly.Group if o.TypeId == "Assembly::ReviewNoteGroup"]
        self.assertEqual(len(groups), 1, operation)
        group = groups[0]
        # GeoFeatureGroup auto-adds subgroup children into Assembly.Group, so notes
        # inherit the Assembly transform via Assembly.claimChildren3D (assembly-local
        # BasePosition/TextPosition stay correct under non-identity Assembly placement).
        self.assertIn(note, self.assembly.Group, "{}: note missing from Assembly.Group".format(operation))
        self.assertIn(note, group.Group, "{}: note missing from Review Notes group".format(operation))
        self.assertIsNotNone(self.assembly.ViewObject, operation)
        self.assertTrue(
            hasattr(self.assembly.ViewObject, "claimChildren3D"),
            "{}: claimChildren3D binding missing".format(operation),
        )
        asm_claimed = self.assembly.ViewObject.claimChildren3D()
        self.assertIn(group, asm_claimed, "{}: group not under Assembly 3D".format(operation))
        self.assertIn(note, asm_claimed, "{}: note not under Assembly 3D CS".format(operation))

    def test_gui_view_and_tree_context_menus(self):
        operation = "View and Tree context menus expose review-note commands"
        _msg("  Test '{}'".format(operation))
        import FreeCADGui as Gui

        wb = Gui.activeWorkbench()
        self.assertTrue(hasattr(wb, "ContextMenu"), operation)

        recorded = []

        def _capture(submenu, cmds):
            recorded.append((submenu, list(cmds)))

        original = wb.appendContextMenu
        wb.appendContextMenu = _capture
        try:
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box.Face6")
            recorded.clear()
            wb.ContextMenu("View")
            flat = [c for _s, cmds in recorded for c in cmds]
            self.assertIn("Assembly_AddReviewNote", flat, "{} View Add flat={!r}".format(operation, flat))

            data = CommandReviewNote.normalize_review_note_target(
                self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
            )
            note = CommandReviewNote.create_review_note(
                self.assembly, data, ["Ctx"], open_transaction=False
            )

            try:
                Gui.ActiveDocument.resetEdit()
            except Exception:
                pass
            Gui.Selection.clearSelection()
            Gui.Selection.addSelection(note)
            recorded.clear()
            wb.ContextMenu("Tree")
            flat = [c for _s, cmds in recorded for c in cmds]
            self.assertIn("Assembly_EditReviewNote", flat, "{} Tree Edit".format(operation))
            self.assertIn(
                "Assembly_ToggleResolveReviewNote", flat, "{} Tree Resolve".format(operation)
            )
            self.assertNotIn(
                "Assembly_AddReviewNote", flat, "{} Tree must not Add".format(operation)
            )
        finally:
            wb.appendContextMenu = original

    def test_gui_status_icons_leader_and_annotation_drag_spaces(self):
        operation = "Status icons, leader modes, nested/identity drag spaces, AnnotationLabel"
        _msg("  Test '{}'".format(operation))

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Icons"], open_transaction=False
        )
        self.assertIsNotNone(note.ViewObject, operation)

        open_icon = note.ViewObject.Icon
        note.Resolved = True
        note.ViewObject.signalChangeIcon()
        resolved_icon = note.ViewObject.Icon
        note.Resolved = False
        note.Target = (self.box, ["Face99"])
        note.refreshBasePosition()
        note.ViewObject.signalChangeIcon()
        broken_icon = note.ViewObject.Icon
        note.Target = (self.box, ["Face6"])
        note.refreshBasePosition()
        note.ViewObject.signalChangeIcon()
        recovered_icon = note.ViewObject.Icon

        self.assertIsNotNone(open_icon, operation)
        self.assertIsNotNone(resolved_icon, operation)
        self.assertIsNotNone(broken_icon, operation)
        self.assertIsNotNone(recovered_icon, operation)
        self.assertNotEqual(
            open_icon.cacheKey(), broken_icon.cacheKey(), "{} open vs broken".format(operation)
        )

        modes = note.ViewObject.listDisplayModes()
        self.assertTrue(any(m in modes for m in ("Line", "Object")), "{} leader".format(operation))

        note.TextPosition = App.Vector(11, 12, 13)
        before = App.Vector(note.BasePosition)
        self.box.Placement = App.Placement(App.Vector(0, 0, 5), App.Rotation())
        self.assertFalse(note.BasePosition.isEqual(before, 1e-6), operation)
        self.assertTrue(note.TextPosition.isEqual(App.Vector(11, 12, 13), 1e-6), operation)

        nested = self.assembly.newObject("Assembly::AssemblyObject", "NestedAsm")
        inner = nested.newObject("Part::Box", "NestedBox")
        inner.Length = 10
        inner.Width = 20
        inner.Height = 30
        nested.Placement = App.Placement(App.Vector(0, 100, 0), App.Rotation(0, 0, 45))
        self.doc.recompute()
        ndata = CommandReviewNote.normalize_review_note_target(
            nested, inner, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        nnote = CommandReviewNote.create_review_note(
            nested, ndata, ["Nested"], open_transaction=False
        )
        nnote.TextPosition = App.Vector(3, 4, 5)
        expected = inner.Placement.multVec(nnote.LocalAnchor)
        self.assertTrue(nnote.BasePosition.isEqual(expected, 1e-5), operation)
        self.assertTrue(nnote.TextPosition.isEqual(App.Vector(3, 4, 5), 1e-6), operation)

        plain = self.doc.addObject("App::AnnotationLabel", "PlainLabel")
        plain.BasePosition = App.Vector(1, 2, 3)
        plain.TextPosition = App.Vector(4, 5, 6)
        plain.LabelText = ["Plain"]
        self.assertTrue(plain.TextPosition.isEqual(App.Vector(4, 5, 6), 1e-6), operation)
        if plain.ViewObject:
            self.assertIsNotNone(plain.ViewObject.Icon, operation)

    def test_delete_empty_review_notes_group_from_tree(self):
        operation = "Empty Review Notes group deletable via Std_Delete with undo/redo"
        _msg("  Test '{}'".format(operation))
        import FreeCADGui as Gui

        group = UtilsAssembly.getReviewNoteGroup(self.assembly)
        group_name = group.Name
        self.assertEqual(group.TypeId, "Assembly::ReviewNoteGroup", operation)
        self.assertEqual(list(group.Group), [], operation)

        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(group)
        Gui.runCommand("Std_Delete")

        self.assertIsNone(self.doc.getObject(group_name), operation)
        self.assertIs(self.doc.getObject(self.assembly.Name), self.assembly, operation)

        self.doc.undo()
        restored = self.doc.getObject(group_name)
        self.assertIsNotNone(restored, operation)
        self.assertEqual(restored.TypeId, "Assembly::ReviewNoteGroup", operation)

        self.doc.redo()
        self.assertIsNone(self.doc.getObject(group_name), operation)

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["After empty delete"], open_transaction=False
        )
        self.assertIsNotNone(note, operation)
        recreated = UtilsAssembly.getReviewNoteGroup(self.assembly)
        self.assertIsNotNone(recreated, operation)
        self.assertEqual(len(recreated.Group), 1, operation)
        self.assertIn(note, recreated.Group, operation)

    def test_populated_review_notes_group_remains_protected(self):
        operation = "Populated Review Notes group protected from Std_Delete"
        _msg("  Test '{}'".format(operation))
        import FreeCADGui as Gui

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly, data, ["Keep me"], open_transaction=False
        )
        group = note.getGroup()
        self.assertIsNotNone(group, operation)
        group_name = group.Name
        note_name = note.Name

        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(group)
        Gui.runCommand("Std_Delete")

        self.assertIsNotNone(self.doc.getObject(group_name), operation)
        self.assertIsNotNone(self.doc.getObject(note_name), operation)
        self.assertIsNotNone(group.Document, operation)
        self.assertIsNotNone(note.Document, operation)
        self.assertIn(note, group.Group, operation)

    def test_gui_text_reference_selection(self):
        operation = "GUI @ref selection highlights target geometry"
        _msg("  Test '{}'".format(operation))
        import FreeCADGui as Gui

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly,
            data,
            ["Inspect @Box.Face6"],
            open_transaction=False,
        )
        self.assertIsNotNone(note, operation)
        Gui.Selection.clearSelection()
        self.assertTrue(
            CommandReviewNote.select_review_note_reference(self.doc, "Box", "Face6"),
            operation,
        )
        selected = Gui.Selection.getSelectionEx(self.doc.Name, 0)
        self.assertGreaterEqual(len(selected), 1, operation)
        # Inside an Assembly GeoFeatureGroup, selection may surface as Assembly + "Box.Face6".
        face_hit = any(
            (
                s.Object.Name == "Box"
                and (
                    "Face6" in list(s.SubElementNames)
                    or any(n.endswith("Face6") for n in list(s.SubElementNames))
                )
            )
            or any(
                n == "Box.Face6" or n.endswith(".Box.Face6") or n.endswith("Box.Face6")
                for n in list(s.SubElementNames)
            )
            for s in selected
        )
        plain = [(s.Object.Name, list(s.SubElementNames)) for s in selected]
        self.assertTrue(face_hit, "{} Face6 not in {!r}".format(operation, plain))
        objs = Gui.Selection.getSelection()
        self.assertTrue(
            any(o.Name in ("Box", "Assembly") for o in objs),
            "{} getSelection={!r}".format(operation, [o.Name for o in objs]),
        )

    def test_gui_leader_terminates_on_box_border(self):
        operation = "Leader attaches to nearest box border after drag/edit/zoom/camera"
        _msg("  Test '{}'".format(operation))
        import math
        import FreeCADGui as Gui

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note = CommandReviewNote.create_review_note(
            self.assembly,
            data,
            ["Border continuity"],
            text_offset=App.Vector(40, 0, 0),
            open_transaction=False,
        )
        self.assertIsNotNone(note.ViewObject, operation)
        self.assertTrue(hasattr(note.ViewObject, "LeaderEnd"), operation)
        self.assertTrue(hasattr(note.ViewObject, "LeaderHalfExtent"), operation)

        def _flush():
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

        def _assert_end_on_border(ctx, require_face_base=True):
            _flush()
            text = App.Vector(note.TextPosition)
            end = App.Vector(note.ViewObject.LeaderEnd)
            he = App.Vector(note.ViewObject.LeaderHalfExtent)
            hw, hh = float(he.x), float(he.y)
            off = end - text
            self.assertGreater(hw, 1e-6, "{} {}: halfW".format(operation, ctx))
            self.assertGreater(hh, 1e-6, "{} {}: halfH".format(operation, ctx))
            self.assertFalse(
                end.isEqual(text, 1e-4),
                "{} {}: LeaderEnd must leave text center (end={} text={})".format(
                    operation, ctx, end, text
                ),
            )
            # Endpoint must land on the rectangle border in billboard UV:
            # max(|u|/halfW, |v|/halfH) == 1 with the other component ≤ 1.
            # With a default camera, billboard ≈ XY for assembly-local offsets.
            nu = abs(off.x) / hw
            nv = abs(off.y) / hh
            self.assertAlmostEqual(
                max(nu, nv),
                1.0,
                places=2,
                msg="{} {}: end not on box border (off={} half=({}, {}) nu={} nv={})".format(
                    operation, ctx, off, hw, hh, nu, nv
                ),
            )
            self.assertLessEqual(
                min(nu, nv),
                1.0 + 0.05,
                "{} {}: end outside box (nu={} nv={})".format(operation, ctx, nu, nv),
            )
            # Offset length is between the nearer side and the corner.
            self.assertGreaterEqual(
                off.Length + 1e-3,
                min(hw, hh),
                "{} {}: leader stops inside the box (gap through text)".format(operation, ctx),
            )
            self.assertLessEqual(
                off.Length,
                math.hypot(hw, hh) + 1e-3,
                "{} {}: leader overshoots past the box corner".format(operation, ctx),
            )
            if require_face_base and text.Length > 5.0:
                self.assertLess(
                    end.Length,
                    text.Length,
                    "{} {}: border point must face the base (end={} text={})".format(
                        operation, ctx, end, text
                    ),
                )
                self.assertGreater(
                    end.x,
                    0.0,
                    "{} {}: end still on the text side of the base".format(operation, ctx),
                )
                self.assertLess(
                    end.x,
                    text.x,
                    "{} {}: leader must not stop short of the box (gap)".format(operation, ctx),
                )

        _assert_end_on_border("initial")
        # +X default camera: attach on the left face at mid-height.
        he0 = App.Vector(note.ViewObject.LeaderHalfExtent)
        end0 = App.Vector(note.ViewObject.LeaderEnd)
        text0 = App.Vector(note.TextPosition)
        self.assertAlmostEqual(end0.x, text0.x - he0.x, places=2, msg=operation)
        self.assertAlmostEqual(end0.y, text0.y, places=2, msg=operation)
        # Half-extent must track the real image, not the old 0.5 mm floor.
        self.assertGreater(he0.x, 0.5 + 1e-3, "{} halfW must exceed legacy 0.5 floor".format(operation))

        note.TextPosition = App.Vector(60, 10, 0)
        _assert_end_on_border("after TextPosition")

        note.LabelText = ["Border continuity", "with extra lines", "to grow the box"]
        _assert_end_on_border("after LabelText")
        he1 = App.Vector(note.ViewObject.LeaderHalfExtent)
        self.assertGreater(he1.y, he0.y, "{} taller text grows halfH".format(operation))

        view = Gui.ActiveDocument.ActiveView
        self.assertIsNotNone(view, operation)
        cam = view.getCameraNode()
        self.assertIsNotNone(cam, operation)
        before = App.Vector(note.ViewObject.LeaderEnd)
        before_he = App.Vector(note.ViewObject.LeaderHalfExtent)
        try:
            if hasattr(cam, "height"):
                old_h = float(cam.height.getValue())
                cam.height.setValue(old_h * 2.5)
            else:
                pos = cam.position.getValue()
                cam.position.setValue(
                    pos[0],
                    pos[1],
                    pos[2] * 2.0 if abs(pos[2]) > 1e-3 else pos[2] + 200.0,
                )
        except Exception as exc:
            self.fail("{} camera adjust failed: {}".format(operation, exc))
        _flush()
        after = App.Vector(note.ViewObject.LeaderEnd)
        after_he = App.Vector(note.ViewObject.LeaderHalfExtent)
        self.assertFalse(
            after.isEqual(before, 1e-3),
            "{} zoom must recompute LeaderEnd (before={} after={})".format(
                operation, before, after
            ),
        )
        self.assertGreater(
            after_he.x,
            before_he.x * 1.5,
            "{} zoom-out must enlarge world half-extents".format(operation),
        )
        _assert_end_on_border("after zoom")

        try:
            from pivy import coin

            cam.orientation.setValue(coin.SbRotation(coin.SbVec3f(1, 0, 0), 0.4))
        except Exception:
            try:
                view.viewAxonometric()
            except Exception:
                pass
        _flush()
        # After camera rotate, billboard axes leave XY — only require a non-center end
        # whose offset length matches a border point for the current half-extents.
        text = App.Vector(note.TextPosition)
        end = App.Vector(note.ViewObject.LeaderEnd)
        he = App.Vector(note.ViewObject.LeaderHalfExtent)
        off = end - text
        self.assertFalse(end.isEqual(text, 1e-4), "{} after rotate".format(operation))
        self.assertGreaterEqual(off.Length + 1e-3, min(he.x, he.y), operation)
        self.assertLessEqual(off.Length, math.hypot(he.x, he.y) + 1e-2, operation)

    def test_gui_leader_border_attachment_varied_offsets(self):
        operation = "Leader border attachment for diagonal/short/vertical offsets"
        _msg("  Test '{}'".format(operation))
        import math
        import FreeCADGui as Gui

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )

        def _flush():
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

        cases = [
            ("diag", App.Vector(40, 30, 0)),
            ("+Y", App.Vector(0, 45, 0)),
            ("-X", App.Vector(-35, 0, 0)),
            ("short", App.Vector(8, 0, 0)),
            ("+Z", App.Vector(0, 0, 40)),
        ]
        for name, offset in cases:
            note = CommandReviewNote.create_review_note(
                self.assembly,
                data,
                ["Offset " + name],
                text_offset=offset,
                open_transaction=False,
            )
            _flush()
            text = App.Vector(note.TextPosition)
            end = App.Vector(note.ViewObject.LeaderEnd)
            he = App.Vector(note.ViewObject.LeaderHalfExtent)
            off = end - text
            self.assertFalse(
                end.isEqual(text, 1e-4),
                "{} {}: buried at center".format(operation, name),
            )
            self.assertGreater(he.x, 1e-6, "{} {}".format(operation, name))
            self.assertGreater(he.y, 1e-6, "{} {}".format(operation, name))
            # Must reach the border — not stop inside the text (through-text gap).
            self.assertGreaterEqual(
                off.Length + 1e-3,
                min(he.x, he.y),
                "{} {}: stops inside box (off={} half=({},{}))".format(
                    operation, name, off, he.x, he.y
                ),
            )
            self.assertLessEqual(
                off.Length,
                math.hypot(he.x, he.y) + 1e-2,
                "{} {}: overshoots box".format(operation, name),
            )
            if name in ("diag", "+Y", "-X", "short"):
                nu = abs(off.x) / he.x
                nv = abs(off.y) / he.y
                self.assertAlmostEqual(
                    max(nu, nv),
                    1.0,
                    places=2,
                    msg="{} {}: not on border nu={} nv={}".format(operation, name, nu, nv),
                )

    def test_gui_at_suggestion_ellipsis_narrow_dropdown(self):
        operation = "Narrow @ suggestion dropdown ellipsizes long paths"
        _msg("  Test '{}'".format(operation))
        import FreeCADGui as Gui
        from PySide import QtCore, QtGui, QtWidgets

        long_path = "AssemblyCase.MidA.MidB.RightPocket.Face12"
        short_path = "Box.Face1"
        # Search still returns complete paths.
        # Build a narrow popup like the note dialog completer.
        narrow_w = 180
        model = QtGui.QStandardItemModel()
        CommandReviewNote.fill_review_note_at_suggestion_model(
            model, [long_path, short_path]
        )
        self.assertEqual(model.rowCount(), 2, operation)

        idx0 = model.index(0, 0)
        self.assertEqual(model.data(idx0, QtCore.Qt.EditRole), long_path, operation)
        self.assertEqual(model.data(idx0, QtCore.Qt.UserRole), long_path, operation)
        self.assertEqual(model.data(idx0, QtCore.Qt.ToolTipRole), long_path, operation)
        self.assertEqual(model.data(idx0, QtCore.Qt.DisplayRole), long_path, operation)

        view = QtWidgets.QListView()
        view.setModel(model)
        view.setItemDelegate(CommandReviewNote.ReviewNoteAtSuggestionDelegate(view))
        view.setFixedWidth(narrow_w)
        view.setHorizontalScrollBarPolicy(QtCore.Qt.ScrollBarAlwaysOff)
        view.show()
        try:
            Gui.updateGui()
        except Exception:
            pass

        fm = view.fontMetrics()
        painted = CommandReviewNote.ellipsis_review_note_at_path_for_width(
            long_path, fm, narrow_w
        )
        self.assertIn("…", painted, operation)
        self.assertTrue(painted.startswith("AssemblyCase"), operation)
        self.assertTrue(
            painted.endswith("Face12") or painted.endswith("RightPocket.Face12"),
            "{} painted={!r}".format(operation, painted),
        )
        self.assertLess(len(painted), len(long_path), operation)
        if hasattr(fm, "horizontalAdvance"):
            self.assertLessEqual(fm.horizontalAdvance(painted), narrow_w, operation)
        else:
            self.assertLessEqual(fm.width(painted), narrow_w, operation)

        # Short paths stay intact.
        short_painted = CommandReviewNote.ellipsis_review_note_at_path_for_width(
            short_path, fm, narrow_w
        )
        self.assertEqual(short_painted, short_path, operation)

        # Selecting inserts the full unmodified reference.
        draft = "Check @Assem"
        cursor = len(draft)
        inserted = CommandReviewNote.apply_review_note_at_completion(
            draft, cursor, model.data(idx0, QtCore.Qt.EditRole)
        )
        self.assertEqual(inserted, "Check @" + long_path, operation)
        self.assertNotIn("…", inserted, operation)

        view.hide()
        view.deleteLater()
        try:
            Gui.updateGui()
        except Exception:
            pass

    def test_gui_modeless_editor_allows_interaction(self):
        operation = "Modeless Review Note editor keeps FreeCAD interactive"
        _msg("  Test '{}'".format(operation))
        import FreeCADGui as Gui
        from PySide import QtWidgets

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        self.assertIsNotNone(data, operation)
        original_sub = list(data["sub_list"])
        original_anchor = App.Vector(data["local_anchor"])

        # Extra component for later selection (must exist outside the editor transaction).
        box2 = self.assembly.newObject("Part::Box", "Box2")
        self.doc.recompute()

        # Open create editor without committing yet.
        task = CommandReviewNote.open_review_note_text_task(
            assembly=self.assembly, target_data=data
        )
        self.assertIsNotNone(task, operation)
        self.assertTrue(Gui.Control.activeDialog(), operation)
        self.assertIs(CommandReviewNote.get_active_review_note_task(), task, operation)
        self.assertTrue(task.isAllowedAlterSelection(), operation)
        self.assertTrue(task.isAllowedAlterView(), operation)
        self.assertTrue(task.isAllowedAlterDocument(), operation)

        # Rotate / zoom while the editor remains open.
        view = Gui.ActiveDocument.ActiveView
        self.assertIsNotNone(view, operation)
        cam = view.getCameraNode()
        self.assertIsNotNone(cam, operation)
        try:
            if hasattr(cam, "height"):
                cam.height.setValue(float(cam.height.getValue()) * 1.4)
            view.viewAxonometric()
        except Exception as exc:
            self.fail("{} camera interaction failed: {}".format(operation, exc))
        try:
            Gui.updateGui()
        except Exception:
            pass
        self.assertTrue(Gui.Control.activeDialog(), "{} editor must stay open".format(operation))

        # Select different geometry / tree objects — must not retarget the frozen create anchor.
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box.Face1")
        self.assertEqual(list(task.target_data["sub_list"]), original_sub, operation)
        self.assertTrue(
            task.target_data["local_anchor"].isEqual(original_anchor, 1e-9),
            operation,
        )
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.assembly)
        self.assertEqual(list(task.target_data["sub_list"]), original_sub, operation)

        # Apply creates the note on the original Face6 anchor.
        task.edit.setPlainText("Modeless @Box.Face1 mention")
        task.clicked(QtWidgets.QDialogButtonBox.Apply)
        note = task.note
        self.assertIsNotNone(note, operation)
        note_name = note.Name
        self.assertEqual(list(note.Target[1]), original_sub, operation)
        self.assertTrue(note.LocalAnchor.isEqual(original_anchor, 1e-9), operation)
        self.assertEqual(list(note.LabelText), ["Modeless @Box.Face1 mention"], operation)

        # Further selection must still leave the committed note's anchor alone.
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box2.Face6")
        self.assertTrue(task._anchor_still_frozen(), operation)
        self.assertEqual(list(note.Target[1]), original_sub, operation)

        # Cancel discards the Apply (abort command).
        self.assertTrue(task.reject(), operation)
        self.assertFalse(Gui.Control.activeDialog(), operation)
        self.assertIsNone(CommandReviewNote.get_active_review_note_task(), operation)
        self.assertIsNone(self.doc.getObject(note_name), "{} Cancel must discard".format(operation))

        # Edit path: open on existing note, interact, OK commits.
        data2 = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        note2 = CommandReviewNote.create_review_note(
            self.assembly, data2, ["Before edit"], open_transaction=False
        )
        before_target = note2.Target
        before_anchor = App.Vector(note2.LocalAnchor)

        task2 = CommandReviewNote.open_review_note_text_task(note=note2)
        self.assertTrue(Gui.Control.activeDialog(), operation)
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(box2)
        try:
            view.fitAll()
        except Exception:
            pass
        self.assertTrue(task2._anchor_still_frozen(), operation)
        task2.edit.setPlainText("After modeless OK")
        self.assertTrue(task2.accept(), operation)
        self.assertEqual(list(note2.LabelText), ["After modeless OK"], operation)
        self.assertEqual(note2.Target[0], before_target[0], operation)
        self.assertEqual(list(note2.Target[1]), list(before_target[1]), operation)
        self.assertTrue(note2.LocalAnchor.isEqual(before_anchor, 1e-9), operation)
        self.assertFalse(Gui.Control.activeDialog(), operation)

    def test_gui_modeless_face_click_inserts_at_ref(self):
        operation = "Modeless editor inserts @ObjectPath.FaceN from 3D face click"
        _msg("  Test '{}'".format(operation))
        import FreeCADGui as Gui
        from PySide import QtGui

        data = CommandReviewNote.normalize_review_note_target(
            self.assembly, self.box, "Face6", picked_point=App.Vector(5, 10, 30)
        )
        self.assertIsNotNone(data, operation)
        original_sub = list(data["sub_list"])
        original_anchor = App.Vector(data["local_anchor"])

        box2 = self.assembly.newObject("Part::Box", "Box2")
        self.doc.recompute()

        task = CommandReviewNote.open_review_note_text_task(
            assembly=self.assembly, target_data=data
        )
        self.assertIsNotNone(task, operation)
        self.assertTrue(task._selection_observer, operation)

        # Seed text and leave the caret between the words (last cursor before 3D pick).
        task.edit.setPlainText("Check  here")
        cursor = task.edit.textCursor()
        cursor.setPosition(6)  # after "Check "
        task.edit.setTextCursor(cursor)
        task.edit._remember_cursor_pos()
        # Simulate focus moving to the 3D view.
        try:
            task.edit.clearFocus()
        except Exception:
            pass

        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box.Face1")
        try:
            Gui.updateGui()
        except Exception:
            pass

        text = task.edit.toPlainText()
        expected_path = CommandReviewNote.review_note_at_path_from_selection(
            self.assembly, "Box.Face1"
        )
        self.assertEqual(expected_path, "Assembly.Box.Face1", operation)
        self.assertIn("@" + expected_path, text, "{} text={!r}".format(operation, text))
        self.assertTrue(text.startswith("Check @"), "{} text={!r}".format(operation, text))
        self.assertTrue(text.endswith(" here") or " here" in text, operation)

        # Original create anchor must stay frozen.
        self.assertEqual(list(task.target_data["sub_list"]), original_sub, operation)
        self.assertTrue(
            task.target_data["local_anchor"].isEqual(original_anchor, 1e-9),
            operation,
        )

        # Second pick appends another ref at the updated caret.
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box2.Face6")
        try:
            Gui.updateGui()
        except Exception:
            pass
        text2 = task.edit.toPlainText()
        path2 = CommandReviewNote.review_note_at_path_from_selection(
            self.assembly, "Box2.Face6"
        )
        self.assertIn("@" + path2, text2, "{} text={!r}".format(operation, text2))
        self.assertEqual(list(task.target_data["sub_list"]), original_sub, operation)

        # OK commits text with inserted refs; Cancel path covered by sibling test.
        self.assertTrue(task.accept(), operation)
        note = task.note
        self.assertIsNotNone(note, operation)
        self.assertEqual(list(note.Target[1]), original_sub, operation)
        self.assertTrue(note.LocalAnchor.isEqual(original_anchor, 1e-9), operation)
        joined = "\n".join(list(note.LabelText))
        self.assertIn("@" + expected_path, joined, operation)
        self.assertIn("@" + path2, joined, operation)
        self.assertFalse(Gui.Control.activeDialog(), operation)

        # Edit mode: face click inserts without moving the existing note anchor.
        before_target = note.Target
        before_anchor = App.Vector(note.LocalAnchor)
        task2 = CommandReviewNote.open_review_note_text_task(note=note)
        task2.edit.setPlainText("Edit ")
        cursor = task2.edit.textCursor()
        cursor.movePosition(QtGui.QTextCursor.End)
        task2.edit.setTextCursor(cursor)
        task2.edit._remember_cursor_pos()
        Gui.Selection.clearSelection()
        Gui.Selection.addSelection(self.doc.Name, self.assembly.Name, "Box2.Face2")
        try:
            Gui.updateGui()
        except Exception:
            pass
        edit_path = CommandReviewNote.review_note_at_path_from_selection(
            self.assembly, "Box2.Face2"
        )
        self.assertEqual(edit_path, "Assembly.Box2.Face2", operation)
        self.assertIn("@" + edit_path, task2.edit.toPlainText(), operation)
        self.assertTrue(task2._anchor_still_frozen(), operation)
        self.assertEqual(note.Target[0], before_target[0], operation)
        self.assertEqual(list(note.Target[1]), list(before_target[1]), operation)
        self.assertTrue(note.LocalAnchor.isEqual(before_anchor, 1e-9), operation)
        self.assertTrue(task2.reject(), operation)
        self.assertEqual(list(note.LabelText), joined.split("\n"), operation)
