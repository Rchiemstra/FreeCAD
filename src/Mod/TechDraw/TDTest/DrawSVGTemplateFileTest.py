# SPDX-License-Identifier: LGPL-2.1-or-later

"""An embedded TechDraw template must stay readable after a collaborative commit.

Same-document copy/paste used to alias one read-only transient file. Destroying
the copy, including when a collaborative commit later dropped that undo record,
deleted the bytes while the original PageResult still pointed at them.
"""

import os
import unittest

import FreeCAD as App


class DrawSVGTemplateFileTest(unittest.TestCase):
    def setUp(self):
        self.doc = App.newDocument("TDTemplateFile")
        self.template_path = os.path.join(os.path.dirname(__file__), "TestTemplate.svg")
        self.page = self.doc.addObject("TechDraw::DrawPage", "Page")

    def tearDown(self):
        App.closeDocument(self.doc.Name)

    def _length(self, quantity):
        return quantity.Value if hasattr(quantity, "Value") else float(quantity)

    def _assert_template_readable(self, template):
        path = template.PageResult
        self.assertTrue(path)
        self.assertTrue(os.path.isfile(path), path)
        with open(path, "rb") as handle:
            payload = handle.read()
        self.assertTrue(payload.startswith(b"<?xml") or b"<svg" in payload[:200])
        self.assertIn(b"FC-Title", payload)
        self.assertIn(b"Title", payload)
        self.assertEqual(template.EditableTexts["FC-Title"], "Title")
        self.assertAlmostEqual(self._length(template.Width), 297.0, places=2)
        self.assertAlmostEqual(self._length(template.Height), 210.0, places=2)
        self.assertEqual(os.path.dirname(path), self.doc.TransientDir)

    def _install_template(self):
        page = self.page
        template_path = self.template_path

        def edit():
            template = self.doc.addObject("TechDraw::DrawSVGTemplate", "Template")
            template.Template = template_path
            page.Template = template

        result = self.doc.commitCompatibilityMutation(edit, structural=True, recompute=True)
        self.assertEqual(result["status"], "Committed")
        template = self.doc.getObject("Template")
        self.assertIs(self.page.Template, template)
        self._assert_template_readable(template)
        return template

    def test_collaborative_commit_keeps_the_embedded_template(self):
        template = self._install_template()

        self.doc.recompute()
        self._assert_template_readable(template)

        self.doc.undo()
        self.assertIsNone(self.doc.getObject("Template"))
        self.doc.redo()
        template = self.doc.getObject("Template")
        self._assert_template_readable(template)
        self.doc.recompute()
        self._assert_template_readable(template)

    def test_rollback_after_setting_the_template_restores_a_readable_file(self):
        template = self._install_template()
        live = template.PageResult
        replacement = os.path.join(self.doc.TransientDir, "replacement.svg")
        with open(self.template_path, "rb") as source, open(replacement, "wb") as dest:
            dest.write(source.read())

        def edit():
            template.Template = replacement
            raise RuntimeError("abort after setting the template")

        with self.assertRaisesRegex(RuntimeError, "abort after setting the template"):
            self.doc.commitCompatibilityMutation(edit, structural=False, recompute=False)
        self.assertEqual(template.PageResult, live)
        self._assert_template_readable(template)

    def test_destroying_a_copied_template_keeps_the_embedded_file(self):
        template = self._install_template()
        live = template.PageResult
        copies = self.doc.copyObject([template], False)
        self.assertEqual(len(copies), 1)
        copy_name = copies[0].Name

        def drop():
            self.doc.removeObject(copy_name)

        result = self.doc.commitCompatibilityMutation(drop, structural=True, recompute=False)
        self.assertEqual(result["status"], "Committed")
        self.doc.clearUndos()
        self.assertEqual(template.PageResult, live)
        self._assert_template_readable(template)
