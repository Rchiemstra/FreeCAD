"""Detached native Qt/watchdog regression; never opens a FreeCAD document."""
from __future__ import annotations

import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import textwrap
import unittest


ROOT = Path(__file__).resolve().parents[2]
HAS_QT = importlib.util.find_spec("PySide6") is not None


@unittest.skipUnless(sys.platform == "linux" and HAS_QT, "requires Linux and the private Qt runtime")
class NativeWatchdogTests(unittest.TestCase):
    def test_native_gil_held_stall_retains_trace_and_qt_recovers(self):
        # A child owns the global faulthandler timer and Qt application, keeping
        # this experiment independent of other tests' applications/handlers.
        script = textwrap.dedent("""
            import ctypes
            import importlib.util
            import json
            from pathlib import Path
            import sys
            from PySide6 import QtCore

            source, folder = map(Path, sys.argv[1:])
            spec = importlib.util.spec_from_file_location('native_watchdog_under_test', source)
            runtime = importlib.util.module_from_spec(spec)
            spec.loader.exec_module(runtime)
            runtime.HEARTBEAT_INTERVAL_MS = 50
            app = QtCore.QCoreApplication([])
            monitor = runtime.install(folder, qt_core=QtCore, application=app)
            monitor.stall_seconds = .20
            monitor.tick()

            def deliberate_native_stall():
                # PyDLL retains the GIL during the native call. A Python
                # watchdog thread would be unable to sample this interval.
                sleep = ctypes.PyDLL(None).usleep
                sleep.argtypes = [ctypes.c_uint]
                sleep.restype = ctypes.c_int
                assert sleep(350_000) == 0

            QtCore.QTimer.singleShot(100, deliberate_native_stall)
            QtCore.QTimer.singleShot(800, app.quit)
            assert app.exec() == 0
            status = json.loads((folder / 'heartbeat.json').read_text())
            trace = (folder / 'python-timeout-last.txt').read_text()
            assert status['recovery_count'] >= 1, status
            assert status['heartbeat_sequence'] >= 2, status
            assert status['phase'] == 'qt_about_to_quit_or_monitor_stopped', status
            assert 'Timeout' in trace and 'deliberate_native_stall' in trace, trace
            assert (folder / 'python-timeout-current.txt').stat().st_size == 0
            assert (folder / 'python-faults.txt').stat().st_size == 0
            print(json.dumps({'recovery_count': status['recovery_count'],
                              'retained_trace_bytes': len(trace.encode()),
                              'native_stall_captured': True}))
        """)
        with tempfile.TemporaryDirectory(prefix="freecad-native-watchdog-") as folder:
            completed = subprocess.run(
                [sys.executable, "-c", script,
                 str(ROOT / "tools/launcher/gui_hang_diagnostics.py"), folder],
                capture_output=True, text=True, timeout=15, check=False,
            )
        self.assertEqual(completed.returncode, 0, completed.stdout + completed.stderr)
        self.assertIn('"native_stall_captured": true', completed.stdout)


if __name__ == "__main__":
    unittest.main()
