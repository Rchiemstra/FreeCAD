#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Exercise the public embedded loop in a fresh matching stock CPython."""
import importlib.machinery
import importlib.util
import os
from pathlib import Path
import sys


def same_origin(actual, expected):
    return Path(actual).resolve(strict=True) == Path(expected).resolve(strict=True)


def load_build_module(name, expected):
    spec = importlib.util.find_spec(name)
    if (spec is None or not isinstance(spec.loader, importlib.machinery.ExtensionFileLoader)
            or not same_origin(spec.origin, expected)):
        raise RuntimeError(f"{name} is not the build-selected extension")
    module = __import__(name)
    if not same_origin(module.__file__, expected):
        raise RuntimeError(f"{name} changed origin during import")
    return module


def verify_native_libraries(expected, maps_path="/proc/self/maps"):
    # Linux CI: compare loaded objects to the CMake imported targets, including
    # duplicates loaded from another prefix. Resolve symlinks on both sides.
    paths = set()
    stems = {Path(p).resolve(strict=True).name.split(".so", 1)[0] for p in expected}
    for line in Path(maps_path).read_text().splitlines():
        parts = line.split(None, 5)
        if (len(parts) == 6 and parts[5].startswith("/")
                and Path(parts[5]).name.split(".so", 1)[0] in stems):
            paths.add(Path(parts[5]).resolve(strict=True))
    for target in expected:
        target = Path(target).resolve(strict=True)
        stem = target.name.split(".so", 1)[0]
        matches = {p for p in paths if p.name.split(".so", 1)[0] == stem}
        if matches != {target}:
            raise RuntimeError(f"native library {stem} differs from build: {matches}")


def setup_runtime(config):
    import importlib
    import json
    python, freecad, gui, ext, origins_file, *native = Path(config).read_text().splitlines()
    if len(native) != 5 or not os.path.samefile(sys.executable, python):
        raise RuntimeError("child interpreter or build configuration differs from CMake")
    origins = json.loads(Path(origins_file).read_text())
    names = ("PySide6", "PySide6.QtCore", "PySide6.QtGui", "PySide6.QtWidgets", "shiboken6", "shiboken6.Shiboken")
    modules = {}
    for name in names:
        module = importlib.import_module(name)
        if not same_origin(module.__file__, origins[name]):
            raise RuntimeError(f"{name} differs from the configured interpreter's binding")
        modules[name] = module
    verify_native_libraries(native)
    sys.path[:0] = [str(Path(freecad).resolve(strict=True).parent), str(Path(ext).resolve(strict=True))]
    app_module = load_build_module("FreeCAD", freecad)
    gui_module = load_build_module("FreeCADGui", gui)
    verify_native_libraries(native)
    return modules["PySide6.QtCore"], modules["PySide6.QtWidgets"], app_module, gui_module


import socket
import threading
import traceback
import unittest


QtCore = QtWidgets = App = Gui = None


class EmbeddedLoopTest(unittest.TestCase):
    def test_python_progress_and_queued_gui_delivery(self):
        # Supplying QApplication is the supported external-embedding path.
        app = QtWidgets.QApplication.instance() or QtWidgets.QApplication(["embedded-loop-test"])
        Gui.showMainWindow()
        main_thread = QtCore.QThread.currentThread()
        self.assertEqual(main_thread, app.thread())
        events, errors = [], []
        counter = [0]
        ready = threading.Event()
        listener = socket.socket()
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        listener.settimeout(15)

        class Receiver(QtCore.QObject):
            @QtCore.Slot()
            def receive(self):
                try:
                    events.append(("gui", QtCore.QThread.currentThread() == main_thread))
                except BaseException:
                    errors.append(traceback.format_exc())
                finally:
                    app.quit()

        class Sender(QtCore.QObject):
            sent = QtCore.Signal()

        receiver, sender = Receiver(), Sender()
        sender.sent.connect(receiver.receive, QtCore.Qt.ConnectionType.QueuedConnection)

        def worker():
            try:
                with listener.accept()[0] as connection:
                    connection.settimeout(15)
                    if connection.recv(1) != b"R":
                        raise RuntimeError("helper did not send readiness byte")
                    ready.set()
                    # Only the native timer's kill of the helper closes this socket.
                    if connection.recv(1) != b"":
                        raise RuntimeError("unexpected helper data")
                for _ in range(10000):
                    counter[0] += 1
                events.append(("worker", threading.get_ident() != threading.main_thread().ident))
                sender.sent.emit()
            except BaseException:
                errors.append(traceback.format_exc())
                ready.set()

        thread = threading.Thread(target=worker, name="embedded-loop-worker", daemon=True)
        thread.start()
        helper = QtCore.QProcess()
        helper.setProgram(sys.executable)
        helper.setArguments(["-c", "import socket,sys; s=socket.create_connection(('127.0.0.1',int(sys.argv[1]))); s.sendall(b'R'); s.recv(1)", str(listener.getsockname()[1])])
        witness = QtCore.QTimer()
        witness.setSingleShot(True)
        watchdog = QtCore.QTimer()
        watchdog.setSingleShot(True)
        try:
            helper.start()
            self.assertTrue(ready.wait(15), "helper/worker did not connect")
            self.assertEqual(errors, [])
            self.assertEqual(events, [])
            # Native Qt slots run without invoking Python. Killing the helper
            # witnesses loop dispatch and unblocks the Python worker.
            self.assertGreaterEqual(helper.metaObject().indexOfSlot("kill()"), 0)
            self.assertGreaterEqual(app.metaObject().indexOfSlot("quit()"), 0)
            witness.timeout.connect(helper.kill)
            watchdog.timeout.connect(app.quit)
            watchdog.start(10000)
            witness.start(0)
            result = Gui.exec_loop()
        finally:
            witness.stop()
            watchdog.stop()
            if helper.state() != QtCore.QProcess.ProcessState.NotRunning:
                helper.kill()
                helper.waitForFinished(5000)
            listener.close()
            thread.join(15)
        self.assertIsNone(result)
        self.assertFalse(thread.is_alive(), "worker did not finish")
        self.assertEqual(errors, [])
        self.assertEqual(events, [("worker", True), ("gui", True)])
        self.assertEqual(counter[0], 10000)
        self.assertIsInstance(App.Version(), list)  # Python state restored
        sender.sent.disconnect(receiver.receive)
        # Close normally; the parent also requires this process to exit zero.
        self.assertTrue(Gui.getMainWindow().close())


if __name__ == "__main__":
    config = sys.argv.pop(1)
    QtCore, QtWidgets, App, Gui = setup_runtime(config)
    unittest.main()
