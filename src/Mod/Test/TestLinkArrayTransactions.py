# SPDX-License-Identifier: LGPL-2.1-or-later

"""Undo and rollback of App::Link arrays must restore exactly what they changed.

Changing ElementCount and ShowElement in one transaction and then rolling it
back left orphan "<link>_i<N>" element objects: restoring ShowElement
re-created the elements while the transaction was also restoring them. A
collaboration rollback reported "checked transaction restore changed the
collaboration object boundary" and every later commit was refused.

Run with FreeCADCmd -t TestLinkArrayTransactions.
"""

import unittest

import FreeCAD as App


class TestLinkArrayTransactions(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TestLinkArrayTransactions")
        self.doc.UndoMode = 1
        self.box = self.doc.addObject("App::FeaturePython", "Box")
        self.link = self.doc.addObject("App::Link", "Link")
        self.link.LinkedObject = self.box
        self.doc.recompute()

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def _names(self):
        return sorted(obj.Name for obj in self.doc.Objects)

    def _edit(self, count, show):
        self.link.ElementCount = count
        self.link.ShowElement = show

    def test_undo_of_count_and_show_leaves_no_orphan_elements(self):
        self.doc.openTransaction("array")
        self._edit(5, False)
        self.doc.commitTransaction()

        self.doc.undo()

        self.assertEqual(self._names(), ["Box", "Link"])
        self.assertEqual(self.link.ElementCount, 0)
        self.assertTrue(self.link.ShowElement)

        self.doc.redo()

        self.assertEqual(self.link.ElementCount, 5)
        self.assertFalse(self.link.ShowElement)
        self.assertEqual(self._names(), ["Box", "Link"])

    def test_undo_of_a_grown_array_removes_the_new_elements(self):
        self.doc.openTransaction("grow")
        self.link.ElementCount = 3
        self.doc.commitTransaction()
        self.assertEqual(len(self.link.ElementList), 3)

        self.doc.undo()

        self.assertEqual(self._names(), ["Box", "Link"])
        self.assertEqual(len(self.link.ElementList), 0)

        self.doc.redo()

        self.assertEqual(len(self.link.ElementList), 3)
        self.assertEqual(len(self._names()), 5)

    def test_collaboration_rollback_restores_the_object_boundary(self):
        if not hasattr(self.doc, "commitCompatibilityMutation"):
            self.skipTest("no collaboration commit in this build")

        def failing_edit():
            self._edit(5, False)
            raise RuntimeError("abort after the side effects")

        with self.assertRaisesRegex(RuntimeError, "abort after the side effects"):
            self.doc.commitCompatibilityMutation(failing_edit, structural=True)

        self.assertEqual(self._names(), ["Box", "Link"])
        self.assertEqual(self.link.ElementCount, 0)
        result = self.doc.commitCompatibilityMutation(
            lambda: setattr(self.link, "ElementCount", 2), structural=True
        )
        self.assertEqual(result.get("status"), "Committed", result)
        self.assertEqual(len(self.link.ElementList), 2)
