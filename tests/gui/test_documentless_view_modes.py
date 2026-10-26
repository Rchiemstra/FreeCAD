# SPDX-License-Identifier: LGPL-2.1-or-later
"""Run on Qt in an initialized FreeCAD GUI; no App document is created or edited."""
import sys
import unittest


class DocumentlessViewModes(unittest.TestCase):
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
