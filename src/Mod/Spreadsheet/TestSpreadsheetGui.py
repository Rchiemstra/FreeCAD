# SPDX-License-Identifier: LGPL-2.1-or-later

# ***************************************************************************
# *   Copyright (c) 2021 Chris Hennes <chennes@pioneerlibrarysystem.org>    *
# *                                                                         *
# *   This program is free software; you can redistribute it and/or modify  *
# *   it under the terms of the GNU General Public License (GPL)            *
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
# ***************************************************************************/

import hashlib
import os
import tempfile
import time
import unittest
import FreeCAD

from PySide import QtCore, QtGui, QtTest, QtWidgets
import FreeCADGui

# ----------------------------------------------------------------------------------
# define the functions to test the FreeCAD Spreadsheet GUI
# ----------------------------------------------------------------------------------


class SpreadsheetGuiCases(unittest.TestCase):
    def setUp(self):
        self.doc = FreeCAD.newDocument()
        self.sheet = self.doc.addObject("Spreadsheet::Sheet", "Spreadsheet")
        self.view_provider = self.sheet.ViewObject

    def getTableView(self):
        return self.view_provider.getView()

    def tearDown(self):
        FreeCAD.closeDocument(self.doc.Name)

    def injectSimpleData(self):
        """A utility function to initialize a blank sheet with some known data"""
        self.sheet.clearAll()
        self.sheet.set("A1", "1")
        self.sheet.set("A2", "2")
        self.sheet.set("A3", "3")
        self.sheet.set("A4", "4")
        self.sheet.set("B1", "5")
        self.sheet.set("B2", "6")
        self.sheet.set("B3", "7")
        self.sheet.set("B4", "8")
        self.sheet.set("C1", "9")
        self.sheet.set("C2", "10")
        self.sheet.set("C3", "11")
        self.sheet.set("C4", "12")
        self.sheet.set("D1", "13")
        self.sheet.set("D2", "14")
        self.sheet.set("D3", "15")
        self.sheet.set("D4", "16")
        self.doc.recompute()

    def testCopySingleCell(self):
        self.injectSimpleData()
        self.view_provider.doubleClicked()
        view = self.getTableView()
        view.select("A1", QtCore.QItemSelectionModel.SelectCurrent)
        view.setCurrentIndex("A1")
        FreeCAD.Gui.runCommand("Std_Copy", 0)
        view.select("E5", QtCore.QItemSelectionModel.SelectCurrent)
        view.setCurrentIndex("E5")
        FreeCAD.Gui.runCommand("Std_Paste", 0)
        self.doc.recompute()
        self.assertEqual(self.sheet.get("A1"), self.sheet.get("E5"))


class SpreadsheetWindowTestBase(unittest.TestCase):
    def setUp(self):
        self._temporary_directory = tempfile.TemporaryDirectory()
        self.file_name = os.path.join(self._temporary_directory.name, "window-layout.FCStd")
        self.doc = FreeCAD.newDocument("SpreadsheetWindowTest")
        self.sheet = self.doc.addObject("Spreadsheet::Sheet", "Spreadsheet")
        self.doc.recompute()
        self.doc.saveAs(self.file_name)

        view_preferences = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
        self._old_save_layout = view_preferences.GetBool("SaveWindowLayoutPerDocument", True)
        view_preferences.SetBool("SaveWindowLayoutPerDocument", True)

    def tearDown(self):
        if self.doc.Name in FreeCAD.listDocuments():
            FreeCAD.closeDocument(self.doc.Name)
        FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View").SetBool(
            "SaveWindowLayoutPerDocument", self._old_save_layout
        )
        self._temporary_directory.cleanup()
        self._process_events()

    @staticmethod
    def _process_events():
        QtWidgets.QApplication.processEvents(QtCore.QEventLoop.ProcessEventsFlag.AllEvents, 100)

    def _wait_until(self, predicate, timeout=5.0):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            self._process_events()
            if predicate():
                return True
            time.sleep(0.01)
        return predicate()

    @staticmethod
    def _spreadsheet_windows():
        return [
            widget
            for widget in QtWidgets.QApplication.topLevelWidgets()
            if widget.metaObject().className() == "SpreadsheetGui::SheetView"
        ]

    def _open_in_new_window(self):
        self.sheet.ViewObject.showSheetMdi()
        FreeCADGui.runCommand("Std_ViewUndock", 0)
        self.assertTrue(self._wait_until(lambda: len(self._spreadsheet_windows()) == 1))
        return self._spreadsheet_windows()[0]

    def _layout_group(self):
        canonical = QtCore.QDir.fromNativeSeparators(QtCore.QFileInfo(self.file_name).absoluteFilePath())
        if os.name == "nt":
            canonical = canonical.casefold()
        key = hashlib.md5(canonical.encode("utf-8"), usedforsecurity=False).hexdigest()
        return FreeCAD.ParamGet(f"User parameter:BaseApp/Preferences/DocumentWindows/{key}")


class SpreadsheetWindowIntegration(SpreadsheetWindowTestBase):
    def test_save_writes_owner_mode_and_geometry_record(self):
        window = self._open_in_new_window()
        window.showNormal()
        window.setGeometry(80, 90, 640, 420)
        self._process_events()

        self.doc.save()

        record = self._layout_group().GetString("View0", "")
        fields = record.split()
        self.assertEqual(fields[0], "SpreadsheetGui::SheetView")
        self.assertEqual(fields[1], self.sheet.Name)
        self.assertEqual(int(fields[3]), 1)
        self.assertGreaterEqual(int(fields[6]), 400)
        self.assertGreaterEqual(int(fields[7]), 300)


class SpreadsheetWindowEndToEnd(SpreadsheetWindowTestBase):
    def test_editor_uses_the_undocked_sheet_scene_and_delete_closes_window(self):
        window = self._open_in_new_window()
        table = next(
            child
            for child in window.findChildren(QtWidgets.QTableView)
            if child.metaObject().className() == "SpreadsheetGui::SheetTableView"
        )
        index = table.model().index(0, 0)
        table.setCurrentIndex(index)
        self.assertTrue(table.edit(index))
        self._process_events()

        editor = next(
            widget
            for widget in QtWidgets.QApplication.allWidgets()
            if widget.metaObject().className() == "SpreadsheetGui::LineEdit"
        )
        self.assertIsNotNone(editor.graphicsProxyWidget())
        self.assertIs(editor.graphicsProxyWidget().scene(), table.graphicsProxyWidget().scene())
        QtTest.QTest.keyClick(editor, QtCore.Qt.Key.Key_Escape)

        self.doc.removeObject(self.sheet.Name)
        self.assertTrue(self._wait_until(lambda: not self._spreadsheet_windows()))

    def test_close_and_reopen_restores_floating_geometry(self):
        window = self._open_in_new_window()
        expected = QtCore.QRect(70, 80, 620, 410)
        window.showNormal()
        window.setGeometry(expected)
        self._process_events()
        self.doc.save()

        document_name = self.doc.Name
        FreeCAD.closeDocument(document_name)
        self.assertTrue(self._wait_until(lambda: not self._spreadsheet_windows()))

        self.doc = FreeCAD.openDocument(self.file_name)
        self.assertTrue(self._wait_until(lambda: len(self._spreadsheet_windows()) == 1))
        restored = self._spreadsheet_windows()[0].geometry()
        self.assertLessEqual(abs(restored.x() - expected.x()), 20)
        self.assertLessEqual(abs(restored.y() - expected.y()), 20)
        self.assertEqual(restored.size(), expected.size())
