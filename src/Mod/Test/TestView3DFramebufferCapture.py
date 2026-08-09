# SPDX-License-Identifier: LGPL-2.1-or-later

"""GUI contract tests for 3D viewer image capture."""

from contextlib import suppress
import time
import unittest

import FreeCAD
import FreeCADGui

try:
    from PySide6 import QtOpenGLWidgets, QtWidgets

    QOpenGLWidget = QtOpenGLWidgets.QOpenGLWidget
except ImportError:
    from PySide import QtGui as QtWidgets  # type: ignore

    QOpenGLWidget = QtWidgets.QOpenGLWidget

try:
    NO_PARTIAL_UPDATE = QOpenGLWidget.UpdateBehavior.NoPartialUpdate
except AttributeError:
    NO_PARTIAL_UPDATE = QOpenGLWidget.NoPartialUpdate


class TestView3DFramebufferCapture(unittest.TestCase):
    def setUp(self):
        self.doc = FreeCAD.newDocument("TestView3DFramebufferCapture")
        FreeCADGui.ActiveDocument = FreeCADGui.getDocument(self.doc.Name)
        self.view = FreeCADGui.ActiveDocument.ActiveView
        self.viewer = self.view.getViewer()
        self.viewport = self.view.graphicsView().viewport()

        self._update_behavior = self.viewport.updateBehavior()
        self.viewport.setUpdateBehavior(NO_PARTIAL_UPDATE)

        self._had_axis_cross = self.view.hasAxisCross()
        self.view.setAxisCross(False)

        self._had_navi_cube = self.viewer.isEnabledNaviCube()
        self.viewer.setEnabledNaviCube(False)

    def tearDown(self):
        with suppress(Exception):
            self.viewport.setUpdateBehavior(self._update_behavior)
        with suppress(Exception):
            self.view.setAxisCross(self._had_axis_cross)
        with suppress(Exception):
            self.viewer.setEnabledNaviCube(self._had_navi_cube)

        if FreeCAD.getDocument(self.doc.Name):
            FreeCAD.closeDocument(self.doc.Name)

    def test_render_to_image_uses_requested_dimensions(self):
        self._flush_gui()

        image = self.viewer.renderToImage(width=240, height=135, samples=0)

        self.assertFalse(image.isNull())
        self.assertEqual(image.width(), 240)
        self.assertEqual(image.height(), 135)

    def test_render_to_image_preserves_argument_errors(self):
        with self.assertRaises(TypeError):
            self.viewer.renderToImage(unknown=True)

    def test_grab_framebuffer_uses_raster_orientation(self):
        self.viewer.setGradientBackground("LINEAR")
        self.viewer.setGradientBackgroundColor((1.0, 0.0, 0.0), (0.0, 0.0, 1.0))
        self._flush_gui()

        images = [self.viewer.grabFramebuffer() for _ in range(2)]
        for image in images:
            self.assertFalse(image.isNull())
            self.assertEqual(self.viewport.updateBehavior(), NO_PARTIAL_UPDATE)
            self._assert_gradient_pixels_are_preserved(image)

        image = images[-1]
        x = image.width() // 2
        top = image.pixelColor(x, image.height() // 8)
        bottom = image.pixelColor(x, image.height() * 7 // 8)

        self.assertGreater(top.red(), top.blue())
        self.assertGreater(bottom.blue(), bottom.red())

    def _assert_gradient_pixels_are_preserved(self, image):
        step_x = max(1, image.width() // 24)
        step_y = max(1, image.height() // 14)
        samples = [
            image.pixelColor(x, y)
            for y in range(step_y // 2, image.height(), step_y)
            for x in range(step_x // 2, image.width(), step_x)
        ]
        preserved = sum(color.red() + color.blue() >= 96 for color in samples)
        self.assertGreaterEqual(
            preserved / len(samples),
            0.95,
            "Live framebuffer capture contains discarded or black pixels",
        )

    def _flush_gui(self):
        for _ in range(4):
            FreeCADGui.updateGui()
            QtWidgets.QApplication.processEvents()
            self.view.redraw()
            time.sleep(0.05)
