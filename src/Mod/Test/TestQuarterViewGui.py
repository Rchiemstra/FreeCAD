# SPDX-License-Identifier: LGPL-2.1-or-later

"""GUI regression for the 3D-view framebuffer resize path.

A main-window resize that lands inside a backing-store flush used to make the
QOpenGLWidget viewport delete the texture wrapper the flush had already
captured, and the compositor then bound the deleted wrapper. That delete happens
on Qt >= 6.6 (destroyFbos drops the QRhiTexture). Qt 6.4 and 6.5 keep the
wrapper, so Ubuntu CI's Qt 6.4.2 does not show the bind. A normal build does
not abort on this test without the fix: the dangling wrapper is only fatal when
its address is reused, which depends on heap layout. The test drives the path so
memory checkers and debug allocators can see it, and checks that the Coin
viewport matches the GL widget afterwards.
"""

import time
import unittest

import FreeCAD as App
import FreeCADGui as Gui
from PySide import QtCore, QtGui, QtWidgets


def _pump(milliseconds):
    loop = QtCore.QEventLoop()
    QtCore.QTimer.singleShot(milliseconds, loop.quit)
    loop.exec()


def _test_dock(window):
    """A dock this test owns, alone in its area, so hiding it always relayouts the 3D view.

    A dock already in the window can be tabified with another one; hiding it
    then leaves the dock area, and the 3D view, the same size.
    """
    docks = [
        dock
        for dock in window.findChildren(QtWidgets.QDockWidget)
        if dock.isVisible() and not dock.isFloating()
    ]
    area = QtCore.Qt.TopDockWidgetArea
    for candidate in (
        QtCore.Qt.TopDockWidgetArea,
        QtCore.Qt.RightDockWidgetArea,
        QtCore.Qt.LeftDockWidgetArea,
        QtCore.Qt.BottomDockWidgetArea,
    ):
        if not any(window.dockWidgetArea(dock) == candidate for dock in docks):
            area = candidate
            break
    dock = QtWidgets.QDockWidget("Quarter view test", window)
    dock.setObjectName("QuarterViewTestDock")
    body = QtWidgets.QWidget(dock)
    body.setMinimumSize(220, 140)
    dock.setWidget(body)
    window.addDockWidget(area, dock)
    dock.show()
    return dock


def _gl_viewport(window):
    mdi = window.findChild(QtWidgets.QMdiArea)
    sub = mdi.currentSubWindow() if mdi is not None else None
    root = sub if sub is not None else window
    found = []
    for graphics in root.findChildren(QtWidgets.QGraphicsView):
        viewport = graphics.viewport()
        if viewport is not None and viewport.inherits("QOpenGLWidget") and viewport.isVisible():
            found.append(viewport)
    if not found and root is not window:
        return _gl_viewport_any(window)
    if not found:
        raise AssertionError("no visible QOpenGLWidget viewport")
    return found[0]


def _gl_viewport_any(window):
    for graphics in window.findChildren(QtWidgets.QGraphicsView):
        viewport = graphics.viewport()
        if viewport is not None and viewport.inherits("QOpenGLWidget") and viewport.isVisible():
            return viewport
    raise AssertionError("no visible QOpenGLWidget viewport")


def _sizes(doc_name, viewport):
    view = Gui.getDocument(doc_name).ActiveView
    coin = tuple(int(value) for value in view.getSize())
    ratio = float(viewport.devicePixelRatio())
    framebuffer = (int(ratio * viewport.width()), int(ratio * viewport.height()))
    return coin, framebuffer


def _repaint_until_fired(window, filter_object, attempts=25):
    """Repaint the main window until its armed paint filter has fired.

    Under load the window can still be waiting for its expose after a resize,
    and a repaint then paints nothing. Nothing is pumped between the paint that
    fires and the return, so the queued replay has not run yet.
    """
    for _ in range(attempts):
        window.repaint()
        if filter_object.done:
            return
        _pump(20)


def _wait_stable(viewport, timeout_ms=1500):
    last = None
    stable = 0
    elapsed = 0
    while elapsed < timeout_ms:
        _pump(40)
        elapsed += 40
        size = (viewport.width(), viewport.height())
        if size == last and size[0] > 0 and size[1] > 0:
            stable += 1
            if stable >= 3:
                return
        else:
            stable = 0
            last = size
    raise AssertionError("viewport size did not settle at %s" % (last,))


class _SwallowBackingStore(QtCore.QObject):
    """Drops flush deliveries so a modal loop inside a paint cannot reenter sync."""

    def eventFilter(self, obj, event):
        if event.type() in (QtCore.QEvent.UpdateRequest, QtCore.QEvent.Paint, QtCore.QEvent.Expose):
            return True
        return False


def _mdi_subwindow(widget):
    while widget is not None:
        if isinstance(widget, QtWidgets.QMdiSubWindow):
            return widget
        widget = widget.parentWidget()
    return None


class _NestedResize(QtCore.QObject):
    """Resizes the GL viewport from inside the main window's paint, once.

    The viewport widget is resized directly. Resizing the top-level window from
    this paint calls syncBackingStore on the window that is already painting.
    """

    def __init__(self, window, viewport, measure):
        super().__init__()
        self.window = window
        self.viewport = viewport
        self.measure = measure
        self.armed = False
        self.done = False
        self.held = False
        self.held_after_modal = False
        self.cpu = 0.0

    def eventFilter(self, obj, event):
        if (
            not self.armed
            or self.done
            or obj is not self.window
            or event.type() != QtCore.QEvent.Paint
        ):
            return False
        self.done = True
        self.armed = False
        width = max(80, self.viewport.width() - 40)
        height = max(80, self.viewport.height() - 30)
        # Fixed so a layout pass inside the modal loop cannot put the widget
        # back to the size Coin already has and hide the hold.
        self.viewport.setFixedSize(width, height)
        coin, framebuffer = self.measure()
        self.held = coin != framebuffer
        swallow = _SwallowBackingStore()
        app = QtWidgets.QApplication.instance()
        app.installEventFilter(swallow)
        try:
            started = time.thread_time()
            loop = QtCore.QEventLoop()
            QtCore.QTimer.singleShot(200, loop.quit)
            loop.exec()
            self.cpu = time.thread_time() - started
        finally:
            app.removeEventFilter(swallow)
        coin, framebuffer = self.measure()
        self.held_after_modal = coin != framebuffer
        return False


class TestQuarterViewGui(unittest.TestCase):
    def setUp(self):
        self.window = Gui.getMainWindow()
        self.geometry = self.window.saveGeometry()
        self.animated = self.window.isAnimated()
        self.dock = _test_dock(self.window)
        self.doc = App.newDocument("QuarterViewGui")
        self.doc.addObject("Part::Box", "Box")
        self.doc.recompute()
        self.window.setAnimated(True)
        self.window.show()

    def tearDown(self):
        if self.doc is not None and self.doc.Name in App.listDocuments():
            App.closeDocument(self.doc.Name)
        self.window.setAnimated(False)
        self.window.removeDockWidget(self.dock)
        self.dock.deleteLater()
        _pump(30)
        self.window.setAnimated(self.animated)
        self.window.restoreGeometry(self.geometry)
        _pump(30)

    def test_animated_dock_resize_and_close(self):
        self.window.resize(1280, 860)
        Gui.updateGui()
        _pump(80)
        self.assertIsNotNone(Gui.getDocument(self.doc.Name).ActiveView)
        if not self.dock.isVisible():
            self.dock.toggleViewAction().trigger()
            _wait_stable(_gl_viewport(self.window))
            Gui.updateGui()

        viewport = _gl_viewport(self.window)
        before = (viewport.width(), viewport.height())
        self.dock.toggleViewAction().trigger()
        _wait_stable(viewport)
        Gui.updateGui()
        after_toggle = (viewport.width(), viewport.height())
        self.assertNotEqual(before, after_toggle, "dock toggle did not relayout the 3D view")
        coin, framebuffer = _sizes(self.doc.Name, viewport)
        self.assertEqual(coin, framebuffer, "after dock animation coin=%s fb=%s" % (coin, framebuffer))

        self.dock.toggleViewAction().trigger()
        _wait_stable(viewport)
        Gui.updateGui()
        coin, framebuffer = _sizes(self.doc.Name, viewport)
        self.assertEqual(coin, framebuffer, "after dock restore coin=%s fb=%s" % (coin, framebuffer))

        viewport = _gl_viewport(self.window)
        nested = _NestedResize(
            self.window,
            viewport,
            lambda: _sizes(self.doc.Name, viewport),
        )
        self.window.installEventFilter(nested)
        try:
            nested.armed = True
            before_nested = _sizes(self.doc.Name, viewport)
            _repaint_until_fired(self.window, nested)
            held_coin, held_fb = _sizes(self.doc.Name, viewport)
            self.assertNotEqual(
                held_coin,
                held_fb,
                "resize was delivered inside the flush (coin=%s fb=%s before=%s)"
                % (held_coin, held_fb, before_nested),
            )
            _pump(40)
            Gui.updateGui()
        finally:
            self.window.removeEventFilter(nested)

        self.assertTrue(nested.done)
        self.assertTrue(nested.held, "Coin viewport already matched during the held resize")
        self.assertTrue(
            nested.held_after_modal,
            "held resize was delivered while the modal loop was still running",
        )
        self.assertLess(
            nested.cpu,
            0.15,
            "modal loop from a paint delivery spun the resize guard (cpu %.3fs)" % nested.cpu,
        )
        viewport = _gl_viewport(self.window)
        coin, framebuffer = _sizes(self.doc.Name, viewport)
        self.assertEqual(
            coin,
            framebuffer,
            "after nested resize coin=%s fb=%s" % (coin, framebuffer),
        )
        self.assertGreater(framebuffer[0], 0)
        self.assertGreater(framebuffer[1], 0)
        viewport.setMinimumSize(0, 0)
        viewport.setMaximumSize(16777215, 16777215)

        name = self.doc.Name
        App.closeDocument(name)
        self.doc = None
        _pump(30)
        self.assertNotIn(name, App.listDocuments())

    def test_stale_replay_keeps_later_size(self):
        self.window.setAnimated(False)
        self.window.resize(1280, 860)
        Gui.updateGui()
        _pump(80)
        if not self.dock.isVisible():
            self.dock.toggleViewAction().trigger()
            Gui.updateGui()
            _pump(40)
        viewport = _gl_viewport(self.window)

        class _Hold(QtCore.QObject):
            def __init__(self, window, gl):
                super().__init__()
                self.window = window
                self.gl = gl
                self.armed = False
                self.done = False
                self.size_a = None

            def eventFilter(self, obj, event):
                if (
                    not self.armed
                    or self.done
                    or obj is not self.window
                    or event.type() != QtCore.QEvent.Paint
                ):
                    return False
                self.done = True
                self.armed = False
                # Viewport only. A top-level resize from this paint syncs the
                # backing store of the window that is already painting.
                self.gl.resize(max(80, self.gl.width() - 70), max(80, self.gl.height() - 50))
                # Device pixels, the unit _sizes() reports for Coin and the framebuffer.
                ratio = float(self.gl.devicePixelRatio())
                self.size_a = (int(ratio * self.gl.width()), int(ratio * self.gl.height()))
                return False

        holder = _Hold(self.window, viewport)
        self.window.installEventFilter(holder)
        try:
            holder.armed = True
            _repaint_until_fired(self.window, holder)
        finally:
            self.window.removeEventFilter(holder)
        self.assertTrue(holder.done)
        self.assertIsNotNone(holder.size_a)

        coin, framebuffer = _sizes(self.doc.Name, viewport)
        self.assertEqual(framebuffer, holder.size_a)
        self.assertNotEqual(coin, framebuffer, "size A was not held")

        # Durable child resize, not a top-level QWindow resize (that delivery
        # is itself a flush). Runs before the queued replay.
        sub = _mdi_subwindow(viewport)
        self.assertIsNotNone(sub, "3D view is not in an MDI subwindow")
        sub.resize(max(120, sub.width() - 90), max(120, sub.height() - 70))
        coin_b, framebuffer_b = _sizes(self.doc.Name, viewport)
        self.assertNotEqual(
            framebuffer_b,
            holder.size_a,
            "size B did not change the viewport (sub %s viewport %s)"
            % (sub.size(), (viewport.width(), viewport.height())),
        )
        self.assertEqual(coin_b, framebuffer_b, "size B was not delivered outside the flush")

        _pump(80)
        Gui.updateGui()
        coin_final, framebuffer_final = _sizes(self.doc.Name, viewport)
        self.assertEqual(framebuffer_final, framebuffer_b)
        self.assertEqual(
            coin_final,
            framebuffer_final,
            "replay restored coin=%s over viewport=%s (held A=%s)"
            % (coin_final, framebuffer_final, holder.size_a),
        )
        self.assertNotEqual(coin_final, holder.size_a)

    def test_window_resize_applies_in_same_update(self):
        # QWidget.resize() writes the widget rect before the QWindow event, so
        # handleResizeEvent sees no change and the layout is not inside the
        # counted scope. Resizing the window handle is the window-system path:
        # the forward runs inside that scope and must still update Coin before
        # the next updateGui, not on a queued replay.
        self.window.setAnimated(False)
        self.window.showNormal()
        mdi = self.window.findChild(QtWidgets.QMdiArea)
        sub = mdi.currentSubWindow() if mdi is not None else None
        if sub is not None:
            sub.showMaximized()
        self.window.resize(1280, 860)
        Gui.updateGui()
        _pump(40)
        viewport = _gl_viewport(self.window)
        before_coin, before_fb = _sizes(self.doc.Name, viewport)
        self.assertEqual(before_coin, before_fb, "setup coin=%s fb=%s" % (before_coin, before_fb))
        handle = self.window.windowHandle()
        self.assertIsNotNone(handle)
        # The test runner is already in an event loop, so updateGui does not
        # flush a posted window-system resize. Deliver that QWindow Resize
        # inside this updateGui round. A held viewport resize is replayed
        # only on a later round; Coin has to match before that.
        delivered = {}

        def deliver():
            handle.resize(1100, 720)
            QtWidgets.QApplication.sendEvent(
                handle, QtGui.QResizeEvent(handle.size(), self.window.size())
            )
            delivered["sizes"] = _sizes(self.doc.Name, viewport)

        QtCore.QTimer.singleShot(0, deliver)
        Gui.updateGui()
        self.assertIn("sizes", delivered, "QWindow resize was not delivered in this updateGui round")
        coin, framebuffer = delivered["sizes"]
        self.assertNotEqual(
            framebuffer,
            before_fb,
            "QWindow resize did not change the 3D view",
        )
        self.assertEqual(
            coin,
            framebuffer,
            "window resize deferred the Coin viewport (coin=%s fb=%s before=%s)"
            % (coin, framebuffer, before_fb),
        )
        self.assertGreater(framebuffer[0], 0)
        self.assertGreater(framebuffer[1], 0)
