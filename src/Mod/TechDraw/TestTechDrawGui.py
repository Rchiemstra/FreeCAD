# SPDX-License-Identifier: LGPL-2.1-or-later

# **************************************************************************
#   Copyright (c) 2022 WandererFan <wandererfan@gmail.com>                *
#                                                                         *
#   This file is part of the FreeCAD CAx development system.              *
#                                                                         *
#   This program is free software; you can redistribute it and/or modify  *
#   it under the terms of the GNU Lesser General Public License (LGPL)    *
#   as published by the Free Software Foundation; either version 2 of     *
#   the License, or (at your option) any later version.                   *
#   for detail see the LICENCE text file.                                 *
#                                                                         *
#   FreeCAD is distributed in the hope that it will be useful,            *
#   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
#   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
#   GNU Library General Public License for more details.                  *
#                                                                         *
#   You should have received a copy of the GNU Library General Public     *
#   License along with FreeCAD; if not, write to the Free Software        *
#   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  *
#   USA                                                                   *
# **************************************************************************

#these tests require QApplication in order to process events while waiting for 
#threads to complete
from TDTest.DrawViewSectionTest import DrawViewSectionTest  # noqa: F401
from TDTest.DrawViewPartTest import DrawViewPartTest  # noqa: F401
from TDTest.DrawViewDetailTest import DrawViewDetailTest  # noqa: F401
from TDTest.DrawViewDimensionTest import DrawViewDimensionTest  # noqa: F401
from TDTest.TechDrawTaskTest import TechDrawTaskTest  # noqa: F401



import os
import unittest

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtWidgets
from Test.GuiRecompute import close_document, recompute_document


class SVGTemplateRedrawTest(unittest.TestCase):
    """Empty templates are a normal intermediate GUI state, not invalid SVG."""

    def setUp(self):
        self.doc = App.newDocument("TDRedrawRegression")
        self.report = Gui.getMainWindow().findChild(QtWidgets.QTextEdit, "Report view")
        self.assertIsNotNone(self.report)
        QtCore.QCoreApplication.processEvents()
        self.report_start = len(self.report.toPlainText())
        self.template_path = os.path.join(os.path.dirname(__file__), "TDTest", "TestTemplate.svg")

    def tearDown(self):
        close_document(self.doc)

    def _install(self, configured):
        self.page = self.doc.addObject("TechDraw::DrawPage", "Page")
        self.template = self.doc.addObject("TechDraw::DrawSVGTemplate", "Template")
        if configured:
            self.template.Template = self.template_path
        self.page.Template = self.template
        recompute_document(self.doc)
        self._show_page()

    def _show_page(self):
        self.page.ViewObject.show()
        QtCore.QCoreApplication.processEvents()
        views = Gui.getDocument(self.doc.Name).mdiViewsOfType("TechDrawGui::MDIViewPage")
        self.assertEqual(len(views), 1, "The drawing page must actually be open")

    def _assert_quiet_redraws(self):
        for scale in (1.1, 1.2, 1.0):
            self.page.Scale = scale
            recompute_document(self.doc)
            self.page.ViewObject.hide()
            self._show_page()  # Recreate the scene and force SVG template draw().
        text = self.report.toPlainText()[self.report_start:]
        self.assertNotIn("QGISVGTemplate::createClickHandles - xml loading error", text)

    def test_configured_template_redraws_quietly(self):
        self._install(True)
        self.assertTrue(self.template.PageResult)
        self.assertIn("FC-Title", self.template.EditableTexts)
        self._assert_quiet_redraws()

    def test_empty_template_redraws_quietly(self):
        self._install(False)
        self.assertFalse(self.template.PageResult)
        self._assert_quiet_redraws()

    def test_replacing_template_with_empty_redraws_quietly_and_can_be_reloaded(self):
        self._install(True)
        self.template = self.doc.addObject("TechDraw::DrawSVGTemplate", "EmptyTemplate")
        self.page.Template = self.template
        self.assertFalse(self.template.PageResult)
        recompute_document(self.doc)
        self._assert_quiet_redraws()
        self.template.Template = self.template_path
        recompute_document(self.doc)
        self.assertTrue(self.template.PageResult)
        self.assertIn("FC-Title", self.template.EditableTexts)
        self._assert_quiet_redraws()
