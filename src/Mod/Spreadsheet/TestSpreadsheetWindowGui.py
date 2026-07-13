# SPDX-License-Identifier: LGPL-2.1-or-later

import hashlib
import os
import tempfile
import time
import unittest

import FreeCAD
import FreeCADGui
from PySide import QtCore, QtGui, QtWidgets


class SpreadsheetWindowTestBase(unittest.TestCase):
    def setUp(self):
        self._temporary_directory = tempfile.TemporaryDirectory()
        self.file_name = os.path.join(self._temporary_directory.name, "window-layout.FCStd")
        self.doc = FreeCAD.newDocument("SpreadsheetWindowTest")
        self.sheet = self.doc.addObject("Spreadsheet::Sheet", "Spreadsheet")
        self.doc.recompute()

        view_preferences = FreeCAD.ParamGet("User parameter:BaseApp/Preferences/View")
        self._old_save_layout = view_preferences.GetBool("SaveWindowLayoutPerDocument", True)
        view_preferences.SetBool("SaveWindowLayoutPerDocument", True)
        self.doc.saveAs(self.file_name)

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

    @staticmethod
    def _view_3d_windows():
        return [
            widget
            for widget in QtWidgets.QApplication.topLevelWidgets()
            if widget.metaObject().className() == "Gui::View3DInventor"
        ]

    def _open_in_new_window(self):
        self.sheet.ViewObject.showSheetMdi()
        FreeCADGui.runCommand("Std_ViewDockUndockFullscreen", 1)
        self.assertTrue(self._wait_until(lambda: len(self._spreadsheet_windows()) == 1))
        return self._spreadsheet_windows()[0]

    @staticmethod
    def _send_mouse_event(tab_bar, event_type, position, button, buttons):
        global_position = tab_bar.mapToGlobal(position)
        event = QtGui.QMouseEvent(
            event_type,
            QtCore.QPointF(position),
            QtCore.QPointF(global_position),
            button,
            buttons,
            QtCore.Qt.KeyboardModifier.NoModifier,
        )
        QtWidgets.QApplication.sendEvent(tab_bar, event)

    def _drag_current_tab_outside(self):
        tab_bar = FreeCADGui.getMainWindow().findChild(
            QtWidgets.QTabBar, "mdiAreaTabBar"
        )
        self.assertIsNotNone(tab_bar)
        press_position = tab_bar.tabRect(tab_bar.currentIndex()).center()
        release_position = QtCore.QPoint(press_position.x(), -50)

        self._send_mouse_event(
            tab_bar,
            QtCore.QEvent.Type.MouseButtonPress,
            press_position,
            QtCore.Qt.MouseButton.LeftButton,
            QtCore.Qt.MouseButton.LeftButton,
        )
        self._send_mouse_event(
            tab_bar,
            QtCore.QEvent.Type.MouseMove,
            release_position,
            QtCore.Qt.MouseButton.NoButton,
            QtCore.Qt.MouseButton.LeftButton,
        )
        self._send_mouse_event(
            tab_bar,
            QtCore.QEvent.Type.MouseButtonRelease,
            release_position,
            QtCore.Qt.MouseButton.LeftButton,
            QtCore.Qt.MouseButton.NoButton,
        )

    def _layout_group(self):
        canonical = QtCore.QDir.fromNativeSeparators(
            QtCore.QFileInfo(self.file_name).absoluteFilePath()
        )
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
    def test_dragging_tab_outside_tab_bar_detaches_view(self):
        self.sheet.ViewObject.showSheetMdi()
        self._drag_current_tab_outside()

        self.assertTrue(self._wait_until(lambda: len(self._spreadsheet_windows()) == 1))

    def test_dragging_3d_tab_outside_tab_bar_detaches_view(self):
        self.assertFalse(self._view_3d_windows())
        FreeCADGui.activeDocument().activeView().viewAxonometric()
        self._process_events()
        self._drag_current_tab_outside()

        self.assertTrue(self._wait_until(lambda: len(self._view_3d_windows()) == 1))

    def test_delete_undocked_spreadsheet_closes_window(self):
        self._open_in_new_window()
        sheet_name = self.sheet.Name
        self.sheet = None
        self.doc.removeObject(sheet_name)
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
