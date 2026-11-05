# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run on Qt in an initialized FreeCAD GUI; no App document is created or edited."""
import sys
import unittest


class DocumentlessViewModes(unittest.TestCase):
    def test_both_literal_uv_key_orders_on_documentless_view(self):
        Gui = sys.modules.get("FreeCADGui")
        if Gui is None or not hasattr(Gui, "getMainWindow"):
            self.skipTest("requires an initialized FreeCAD GUI on the Qt thread")
        from PySide6 import QtCore, QtGui, QtTest, QtWidgets
        from pivy import coin

        app = QtWidgets.QApplication.instance()
        previous_focus = app.focusWidget()
        previous_window = app.activeWindow()
        view = Gui.createViewer(1, "Literal U,V and V,U shortcut regression")
        view.setAnimationEnabled(False)
        scene = view.getSceneGraph()
        if scene is None:
            scene = coin.SoSeparator()
            view.getViewer().setSceneGraph(scene)
        marker = coin.SoSeparator()
        marker.addChild(coin.SoCube())
        scene.addChild(marker)
        widget = view.graphicsView()
        triggered = []
        actions = []

        def container():
            parent = widget
            while parent is not None:
                if isinstance(parent, QtWidgets.QMdiSubWindow) or parent.isWindow():
                    return parent
                parent = parent.parentWidget()
            self.fail("The viewer lost its containing window")

        def focus_view():
            window = container()
            window.show()
            area = getattr(window, "mdiArea", None)
            if callable(area):
                area().setActiveSubWindow(window)
            window.window().raise_()
            window.window().activateWindow()
            app.setActiveWindow(window.window())
            widget.setFocus(QtCore.Qt.OtherFocusReason)
            QtTest.QTest.qWait(350)
            focus = app.focusWidget()
            self.assertTrue(focus is widget or focus is not None and widget.isAncestorOf(focus))
            self.assertEqual(Gui.getMainWindow().getActiveWindow().getSceneGraph(), scene)

        def press(first, second):
            focus_view()
            QtTest.QTest.keyClick(widget, first)
            QtTest.QTest.qWait(60)
            QtTest.QTest.keyClick(widget, second)
            QtTest.QTest.qWait(700)

        for action in Gui.getMainWindow().findChildren(QtGui.QAction):
            if any(shortcut.toString().startswith(("U", "V")) for shortcut in action.shortcuts()):
                callback = lambda checked=False, name=action.objectName(): triggered.append(name)
                action.triggered.connect(callback)
                actions.append((action, callback))
        try:
            self.assertIsInstance(container(), QtWidgets.QMdiSubWindow)
            press(QtCore.Qt.Key_U, QtCore.Qt.Key_V)
            self.assertIsInstance(container(), QtWidgets.QMdiSubWindow)
            self.assertEqual(triggered, [], "U,V unexpectedly activated a shortcut")
            self.assertEqual(view.getSceneGraph(), scene)
            self.assertGreaterEqual(scene.findChild(marker), 0)
            # U,V ends with a pending V prefix. Cancel it before the independent V,U case.
            QtTest.QTest.keyClick(widget, QtCore.Qt.Key_Escape)
            QtTest.QTest.qWait(350)
            press(QtCore.Qt.Key_V, QtCore.Qt.Key_U)
            self.assertNotIsInstance(container(), QtWidgets.QMdiSubWindow)
            self.assertIn("Std_ViewUndock", triggered)
            self.assertEqual(view.getSceneGraph(), scene)
            self.assertGreaterEqual(scene.findChild(marker), 0)
        finally:
            for action, callback in actions:
                action.triggered.disconnect(callback)
            container().close()
            app.processEvents()
            if previous_window is not None:
                app.setActiveWindow(previous_window)
            if previous_focus is not None:
                previous_focus.setFocus(QtCore.Qt.OtherFocusReason)

    def test_undock_redock_and_fullscreen_preserve_view_handle_scene_and_camera(self):
        Gui = sys.modules.get("FreeCADGui")
        if Gui is None or not hasattr(Gui, "getMainWindow"):
            self.skipTest("requires an initialized FreeCAD GUI on the Qt thread")
        from PySide6 import QtTest, QtWidgets
        from pivy import coin

        app = QtWidgets.QApplication.instance()
        view = Gui.createViewer(1, "Documentless view mode regression")
        view.setAnimationEnabled(False)
        marker = coin.SoSeparator()
        marker.setName("DocumentlessModeMarker")
        marker.addChild(coin.SoCube())
        scene = view.getSceneGraph()
        if scene is None:
            scene = coin.SoSeparator()
            view.getViewer().setSceneGraph(scene)
        scene.addChild(marker)
        view.viewAxonometric()
        view.fitAll()
        app.processEvents()
        def camera_pose():
            camera = view.getCameraNode()
            return (tuple(camera.position.getValue()), tuple(camera.orientation.getValue().getValue()),
                    camera.focalDistance.getValue(), camera.height.getValue())

        camera = camera_pose()

        def window_mode():
            widget = view.graphicsView()
            while widget is not None:
                if isinstance(widget, QtWidgets.QMdiSubWindow):
                    return 0
                if widget.isWindow():
                    return 2 if widget.isFullScreen() else 1
                widget = widget.parentWidget()
            self.fail("The viewer lost its containing window")

        self.assertEqual(window_mode(), 0)
        try:
            # These are the same command-group modes used by V,U, V,D and V,F.
            # Before the fix the first transition dereferences a null Gui::Document.
            for step, mode in enumerate((1, 0, 1, 0, 2, 0)):
                Gui.runCommand("Std_ViewDockUndockFullscreen", mode)
                QtTest.QTest.qWait(250)  # Allow the window manager to acknowledge the mode.
                self.assertEqual(window_mode(), mode, (step, mode))
                self.assertEqual(view.getSceneGraph(), scene)
                self.assertGreaterEqual(scene.findChild(marker), 0)
                self.assertEqual(camera_pose(), camera)
                view.redraw()
        finally:
            widget = view.graphicsView()
            while widget is not None:
                if isinstance(widget, QtWidgets.QMdiSubWindow) or widget.isWindow():
                    widget.close()
                    break
                widget = widget.parentWidget()
            app.processEvents()
