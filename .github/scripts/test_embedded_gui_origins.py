# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tests for the explicit embedded test's build provenance checks."""
import importlib.util
import json
import runpy
import os
import time
import subprocess
import sys
from pathlib import Path
import tempfile
import unittest
from unittest import mock

spec = importlib.util.spec_from_file_location("embedded_loop", Path(__file__).with_name("test_embedded_gui_loop.py"))
loop = importlib.util.module_from_spec(spec)
spec.loader.exec_module(loop)


class OriginTests(unittest.TestCase):
    def test_launcher_uses_configured_interpreter_in_isolated_mode(self):
        with tempfile.TemporaryDirectory() as d:
            config = Path(d) / "config"
            config.write_text("/configured/python\n")
            launcher = Path(__file__).with_name("run_embedded_gui_loop.py")
            with mock.patch("sys.argv", [str(launcher), str(config)]), \
                 mock.patch("subprocess.Popen") as call, \
                 mock.patch("os.waitid", return_value=object()), \
                 mock.patch("os.killpg"):
                call.return_value.wait.return_value = 9
                with self.assertRaises(SystemExit) as result:
                    runpy.run_path(str(launcher), run_name="__main__")
            self.assertEqual(result.exception.code, 9)
            self.assertEqual(call.call_args.args[0], ["/configured/python", "-I", str(launcher.with_name("test_embedded_gui_loop.py")), str(config)])
            self.assertTrue(call.call_args.kwargs["start_new_session"])

    def test_deadline_success_and_process_group_cleanup(self):
        launcher = Path(__file__).with_name("run_embedded_gui_loop.py")
        with tempfile.TemporaryDirectory() as d:
            pidfile = Path(d) / "pid"
            helper = Path(d) / "helper.py"
            helper.write_text(
                "import subprocess, sys, threading\n"
                "child = subprocess.Popen([sys.executable, '-c', "
                "'import threading; threading.Event().wait(15)'])\n"
                "open(sys.argv[1], 'w').write(str(child.pid))\n"
                "if sys.argv[2] == 'timeout': threading.Event().wait(15)\n"
                "else: raise SystemExit(int(sys.argv[2]))\n"
            )
            for code in ("0", "7", "timeout"):
                with self.subTest(code=code):
                    if pidfile.exists():
                        pidfile.unlink()
                    # Capture the launcher's output: the finite helper inherits
                    # the pipe, so communicate also proves it was closed promptly.
                    driver = ("import runpy, sys; "
                              "run = runpy.run_path(sys.argv[1])['run']; "
                              "raise SystemExit(run([sys.executable, sys.argv[2], "
                              "sys.argv[3], sys.argv[4]], timeout=1))")
                    result = subprocess.run(
                        [sys.executable, "-c", driver, str(launcher), str(helper), str(pidfile), code],
                        stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=4)
                    self.assertEqual(result.returncode, 124 if code == "timeout" else int(code), result.stderr)
                    pid = int(pidfile.read_text())
                    # A zombie has exited even if its new parent has not reaped it.
                    deadline = time.monotonic() + 2
                    while True:
                        try:
                            state = (Path("/proc") / str(pid) / "stat").read_text().split(") ", 1)[1][0]
                        except FileNotFoundError:
                            break
                        if state == "Z":
                            break
                        self.assertLess(time.monotonic(), deadline, "descendant survived cleanup")
                        time.sleep(0.01)

    def test_observation_error_still_cleans_and_reaps(self):
        launcher = Path(__file__).with_name("run_embedded_gui_loop.py")
        run = runpy.run_path(str(launcher))["run"]
        with mock.patch("subprocess.Popen") as popen, \
             mock.patch("os.waitid", side_effect=OSError("observation failed")), \
             mock.patch("os.killpg") as kill:
            popen.return_value.pid = 12345
            with self.assertRaisesRegex(OSError, "observation failed"):
                run(["test"])
            kill.assert_called_once_with(12345, __import__("signal").SIGKILL)
            popen.return_value.wait.assert_called_once_with(timeout=5)

    def test_native_matching_and_mixed_library(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            expected = root / "good" / "libQt6Core.so.6"
            wrong = root / "other" / expected.name
            for file in (expected, wrong):
                file.parent.mkdir(exist_ok=True)
                file.touch()
            maps = root / "maps"
            def write(*paths):
                maps.write_text("".join(f"1-2 r-xp 0 00:00 0 {p}\n" for p in paths))
            write(expected)
            loop.verify_native_libraries([expected], maps)
            for paths in ((wrong,), (expected, wrong), ()):
                write(*paths)
                with self.assertRaises(RuntimeError):
                    loop.verify_native_libraries([expected], maps)

    def test_rejects_wrong_build_module_before_import(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            good, bad = root / "FreeCAD.so", root / "installed.so"
            good.touch(); bad.touch()
            loader = loop.importlib.machinery.ExtensionFileLoader("FreeCAD", str(bad))
            fake = mock.Mock(origin=str(bad), loader=loader)
            with mock.patch.object(loop.importlib.util, "find_spec", return_value=fake):
                with self.assertRaisesRegex(RuntimeError, "build-selected"):
                    loop.load_build_module("FreeCAD", good)
            fake.origin = str(good)
            fake.loader = object()
            with mock.patch.object(loop.importlib.util, "find_spec", return_value=fake):
                with self.assertRaisesRegex(RuntimeError, "build-selected"):
                    loop.load_build_module("FreeCAD", good)

    def test_rejects_wrong_binding_and_interpreter(self):
        with tempfile.TemporaryDirectory() as d:
            root = Path(d)
            good, bad = root / "good", root / "bad"
            good.touch(); bad.touch()
            origins = root / "origins.json"
            origins.write_text(json.dumps({"PySide6": str(good)}))
            config = root / "config"
            config.write_text("\n".join([str(good), str(good), str(good), str(root), str(origins)] + [str(good)] * 5))
            with self.assertRaisesRegex(RuntimeError, "interpreter"):
                loop.setup_runtime(config)
            with mock.patch.object(loop.os.path, "samefile", return_value=True), \
                 mock.patch("importlib.import_module", return_value=mock.Mock(__file__=str(bad))):
                with self.assertRaisesRegex(RuntimeError, "configured interpreter"):
                    loop.setup_runtime(config)


if __name__ == "__main__":
    unittest.main()
