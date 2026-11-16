"""Opt-in GUI diagnostics without native timers, FreeCAD, or a real Qt loop."""
from __future__ import annotations

import ast
import contextlib
import importlib.util
import json
import os
from pathlib import Path
import shutil
import tempfile
from types import SimpleNamespace
import unittest
from unittest import mock


REPO = Path(__file__).resolve().parents[2]


def load(name, filename):
    spec = importlib.util.spec_from_file_location(name, REPO / "tools/launcher" / filename)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


runtime = load("gui_hang_diagnostics_under_test", "gui_hang_diagnostics.py")
launcher = load("start_freecad_diagnostics_under_test", "start_freecad_impl.py")


class Clock:
    def __init__(self):
        self.now = 100.0

    def __call__(self):
        return self.now


class Faults:
    """Model one-shot faulthandler state, without starting a native watchdog."""
    def __init__(self, *, enabled=False):
        self.enabled = enabled
        self.enable_calls = []
        self.disable_calls = 0
        self.arms = []
        self.cancels = 0
        self.pending = None

    def is_enabled(self):
        return self.enabled

    def enable(self, **kwargs):
        self.enable_calls.append(kwargs)
        self.enabled = True

    def disable(self):
        self.disable_calls += 1
        self.enabled = False

    def dump_traceback_later(self, timeout, **kwargs):
        self.arms.append((timeout, kwargs))
        self.pending = kwargs

    def cancel_dump_traceback_later(self):
        self.cancels += 1
        self.pending = None

    def fire(self, text=b"Timeout: synthetic Python stack\n"):
        if self.pending is None:
            return False
        pending, self.pending = self.pending, None
        pending["file"].write(text)
        return True


class Signal:
    def __init__(self):
        self.callbacks = []

    def connect(self, callback):
        self.callbacks.append(callback)

    def emit(self):
        for callback in self.callbacks:
            callback()


class Timer:
    def __init__(self, parent):
        self.parent = parent
        self.timeout = Signal()
        self.interval = None
        self.stopped = False

    def start(self, interval):
        self.interval = interval

    def stop(self):
        self.stopped = True


class RuntimeTests(unittest.TestCase):
    def setUp(self):
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.directory = Path(self.stack.enter_context(tempfile.TemporaryDirectory()))
        self.clock = Clock()
        self.faults = Faults()

    def monitor(self, **kwargs):
        monitor = runtime.GuiHeartbeat(
            self.directory, fault_handler=self.faults, clock=self.clock, **kwargs
        )
        self.addCleanup(monitor.close)
        return monitor

    def status(self):
        return json.loads((self.directory / "heartbeat.json").read_text())

    def test_healthy_qt_ticks_rearm_one_nonfatal_timeout(self):
        monitor = self.monitor()
        self.assertEqual(self.status()["heartbeat_sequence"], 0)
        self.clock.now += 2
        monitor.tick()
        self.assertEqual(self.status()["heartbeat_sequence"], 1)
        self.assertEqual(len(self.faults.arms), 2)
        for timeout, flags in self.faults.arms:
            self.assertEqual(timeout, 30)
            self.assertIs(flags["repeat"], False)
            self.assertIs(flags["exit"], False)
        self.assertFalse(self.status()["model_data_or_authentication_logged"])

    def test_dump_occurs_once_while_hung_and_rearms_only_on_recovery(self):
        monitor = self.monitor()
        self.clock.now += 35
        self.assertTrue(self.faults.fire())
        self.assertFalse(self.faults.fire())
        self.assertEqual(len(self.faults.arms), 1)
        self.assertEqual(self.status()["heartbeat_sequence"], 0)
        monitor.tick()
        self.assertEqual(self.status()["recovery_count"], 1)
        self.assertEqual(len(self.faults.arms), 2)
        self.assertIn(b"synthetic Python stack", (self.directory / "python-timeout-last.txt").read_bytes())
        self.assertEqual((self.directory / "python-timeout-current.txt").stat().st_size, 0)

    def test_recovery_overwrites_latest_trace_without_numbered_history(self):
        monitor = self.monitor()
        for text in (b"first", b"second"):
            self.clock.now += 31
            self.faults.fire(text)
            monitor.tick()
        self.assertEqual((self.directory / "python-timeout-last.txt").read_bytes(), b"second")
        self.assertEqual(self.status()["recovery_count"], 2)
        self.assertEqual(len(list(self.directory.glob("python-timeout-*.txt"))), 2)

    def test_trace_at_cancellation_boundary_is_retained_even_with_shorter_clock_gap(self):
        monitor = self.monitor()
        self.clock.now += 29.999
        self.faults.fire(b"watchdog fired at cancellation boundary")
        monitor.tick()
        self.assertEqual(self.status()["recovery_count"], 1)
        self.assertEqual((self.directory / "python-timeout-current.txt").stat().st_size, 0)

    def test_oversized_foreign_trace_is_capped_and_monitor_stops(self):
        monitor = self.monitor()
        with mock.patch.object(runtime, "MAX_TRACE_BYTES", 8):
            self.faults.fire(b"0123456789abcdef")
            self.clock.now += 31
            monitor.tick()
        self.assertTrue(monitor.closed)
        self.assertEqual(self.status()["phase"], "diagnostics_failed")
        self.assertEqual((self.directory / "python-timeout-last.txt").stat().st_size, 8)
        self.assertEqual((self.directory / "python-timeout-current.txt").stat().st_size, 0)

    def test_bad_clock_stops_diagnostics_without_escaping_qt_callback(self):
        monitor = self.monitor()
        self.clock.now = float("nan")
        monitor.tick()
        self.assertTrue(monitor.closed)
        self.assertEqual(self.status()["phase"], "diagnostics_failed")

    def test_status_io_failure_does_not_escape_callback_or_cleanup(self):
        monitor = self.monitor()
        with mock.patch.object(runtime, "_status", side_effect=OSError("synthetic failure")):
            self.clock.now += 2
            monitor.tick()
            monitor.close()
        self.assertTrue(monitor.closed)
        self.assertTrue(monitor.trace.closed)
        self.assertTrue(monitor.crash.closed)

    def test_existing_fatal_handler_is_preserved(self):
        self.faults.enabled = True
        monitor = self.monitor()
        monitor.close()
        self.assertEqual(self.faults.enable_calls, [])
        self.assertEqual(self.faults.disable_calls, 0)
        self.assertFalse(self.status()["fatal_handler_installed_here"])

    def test_close_cancels_and_disables_only_owned_handler_idempotently(self):
        monitor = self.monitor()
        monitor.close()
        monitor.close()
        self.assertEqual(self.faults.disable_calls, 1)
        self.assertIsNone(self.faults.pending)
        self.assertEqual(self.status()["phase"], "qt_about_to_quit_or_monitor_stopped")

    def test_partial_file_open_failure_closes_first_descriptor(self):
        (self.directory / "python-faults.txt").touch()
        original_open = Path.open
        streams = []

        def opened(path, *args, **kwargs):
            stream = original_open(path, *args, **kwargs)
            if path.name == "python-timeout-current.txt":
                streams.append(stream)
            return stream

        with mock.patch.object(Path, "open", opened):
            with self.assertRaises(FileExistsError):
                self.monitor()
        self.assertEqual(len(streams), 1)
        self.assertTrue(streams[0].closed)

    def test_injected_qt_timer_drives_heartbeat_and_quit_cleanup(self):
        app = SimpleNamespace(aboutToQuit=Signal())
        monitor = runtime.install(
            self.directory, qt_core=SimpleNamespace(QTimer=Timer), application=app,
            fault_handler=self.faults, clock=self.clock,
        )
        self.addCleanup(monitor.close)
        self.assertEqual(monitor.timer.interval, 2000)
        self.assertIs(monitor.timer.parent, app)
        self.clock.now += 2
        monitor.timer.timeout.emit()
        self.assertEqual(monitor.sequence, 1)
        self.assertIs(runtime.install(
            self.directory, qt_core=SimpleNamespace(QTimer=Timer), application=app,
            fault_handler=self.faults, clock=self.clock,
        ), monitor)
        app.aboutToQuit.emit()
        self.assertTrue(monitor.closed)
        self.assertTrue(monitor.timer.stopped)

    def test_unqualified_python_runtime_is_rejected_before_installing_handlers(self):
        with mock.patch.object(runtime.sys, "version_info", (3, 14, 0)):
            with self.assertRaisesRegex(RuntimeError, "3.10-3.13"):
                runtime.install(self.directory, application=object(), qt_core=object(), fault_handler=self.faults)
        self.assertEqual(self.faults.arms, [])


class LaunchPreparationTests(unittest.TestCase):
    def setUp(self):
        self.stack = contextlib.ExitStack()
        self.addCleanup(self.stack.close)
        self.repo = Path(self.stack.enter_context(tempfile.TemporaryDirectory(prefix="FreeCAD source ")))
        self.stack.enter_context(mock.patch.object(launcher, "_repo_root", return_value=self.repo))

    def test_both_flags_are_opt_in_and_not_forwarded(self):
        self.assertFalse(launcher._parse_args([]).diagnostics)
        for flag in ("--diagnostics", "--gui-diagnostics"):
            parsed = launcher._parse_args([flag, "model with spaces.FCStd"])
            self.assertTrue(parsed.diagnostics)
            self.assertEqual(parsed.args, ["model with spaces.FCStd"])

    def test_runtime_macro_is_before_models_and_native_log_is_added(self):
        model = "model ' with spaces.FCStd"
        directory, args = runtime.prepare_launch(self.repo, [model, "second.FCStd"])
        self.assertEqual(args[:2], ["--log-file", str(directory / "FreeCAD.log")])
        self.assertEqual(args[2], str(directory / "GuiDiagnostics.FCMacro"))
        self.assertEqual(args[3:], [model, "second.FCStd"])
        ast.parse((directory / "GuiDiagnostics.FCMacro").read_text())
        metadata = json.loads((directory / "session.json").read_text())
        self.assertFalse(metadata["watchdog_exit"])
        self.assertNotIn(model, (directory / "session.json").read_text())
        self.assertNotIn(model, (directory / "GuiDiagnostics.FCMacro").read_text())

    def test_existing_native_log_options_and_helper_options_are_preserved(self):
        for log in (["--log-file", "custom.log"], ["--log-file=custom.log"], ["--write-log"], ["-l"]):
            with self.subTest(log=log):
                original = ["--profile-name", "throwaway", "--rpc-wait-timeout", "95", *log, "part.FCStd"]
                directory, result = runtime.prepare_launch(self.repo, original)
                self.assertEqual(result, [str(directory / "GuiDiagnostics.FCMacro"), *original])

    def test_launches_have_separate_private_directories_and_frozen_runtime_copy(self):
        first, _ = runtime.prepare_launch(self.repo, [])
        second, _ = runtime.prepare_launch(self.repo, [])
        self.assertNotEqual(first, second)
        self.assertEqual(first.parent, self.repo / "diagnostics")
        self.assertEqual((first / "gui_hang_diagnostics.py").read_bytes(), Path(runtime.__file__).read_bytes())
        if os.name != "nt":
            self.assertEqual(first.stat().st_mode & 0o777, 0o700)

    def test_profile_resolution_matches_env_override_and_helper_name(self):
        self.assertEqual(launcher._diagnostic_isolated_profile([], {}), self.repo / ".freecad-mcp-isolated")
        for args in (["--profile-name", "named"], ["--profile-name=named"]):
            self.assertEqual(launcher._diagnostic_isolated_profile(args, {}), self.repo / "named")
        override = self.repo / "custom profile"
        self.assertEqual(launcher._diagnostic_isolated_profile(["--profile-name", "ignored"], {
            "FREECAD_MCP_PROFILE_DIR": str(override),
        }), override)
        for args in (["--profile-name"], ["--profile-name=../escape"], ["--profile-name="]):
            with self.assertRaises(ValueError):
                launcher._diagnostic_isolated_profile(args, {})

    def test_authenticated_setup_then_diagnostics_then_launch(self):
        events = []

        def run(command, **kwargs):
            events.append(("subprocess", command))
            return SimpleNamespace(returncode=0)

        def prepare(profile, arguments):
            events.append(("diagnostics", profile))
            return [str(self.repo / "GuiDiagnostics.FCMacro"), *arguments]

        def launch(command, environment, status, timeout):
            events.append(("subprocess", command))
            self.assertEqual(status, self.repo / "exit-status.json")
            return 0

        with mock.patch.object(launcher, "_freecad_mcp_dir", return_value=self.repo / "mcp"), mock.patch.object(
            launcher.subprocess, "run", side_effect=run
        ), mock.patch.object(launcher, "_diagnostic_launch_args", side_effect=prepare), mock.patch.dict(
            os.environ, {}, clear=True
        ), mock.patch.object(launcher, "_launch_diagnostic_isolated", side_effect=launch):
            self.assertEqual(launcher.main(["--authenticated-isolated", "--diagnostics", "part.FCStd"]), 0)
        self.assertTrue(events[0][1][-1].endswith("setup_isolated_profile.py"))
        self.assertEqual(events[1], ("diagnostics", self.repo / ".freecad-mcp-isolated"))
        self.assertEqual(events[2][1][-2:], [str(self.repo / "GuiDiagnostics.FCMacro"), "part.FCStd"])

    def test_default_authenticated_launch_does_not_create_diagnostic_files(self):
        with mock.patch.object(launcher, "_freecad_mcp_dir", return_value=self.repo / "mcp"), mock.patch.object(
            launcher.subprocess, "run", return_value=SimpleNamespace(returncode=0)
        ), mock.patch.object(launcher, "_diagnostic_launch_args") as prepare:
            self.assertEqual(launcher.main(["--authenticated-isolated"]), 0)
        prepare.assert_not_called()
        self.assertFalse((self.repo / ".freecad-mcp-isolated").exists())

    def test_setup_failure_stops_before_diagnostics_preparation(self):
        with mock.patch.object(launcher, "_freecad_mcp_dir", return_value=self.repo / "mcp"), mock.patch.object(
            launcher.subprocess, "run", return_value=SimpleNamespace(returncode=7)
        ), mock.patch.object(launcher, "_diagnostic_launch_args") as prepare:
            self.assertEqual(launcher.main(["--authenticated-isolated", "--diagnostics"]), 7)
        prepare.assert_not_called()

    def test_diagnostics_preparation_failure_does_not_launch_freecad(self):
        with mock.patch.object(launcher, "_freecad_mcp_dir", return_value=self.repo / "mcp"), mock.patch.object(
            launcher.subprocess, "run", return_value=SimpleNamespace(returncode=0)
        ) as run, mock.patch.object(launcher, "_diagnostic_launch_args", side_effect=OSError("not writable")):
            self.assertEqual(launcher.main(["--authenticated-isolated", "--diagnostics"]), 1)
        self.assertEqual(run.call_count, 1)

    def test_legacy_launch_also_places_macro_before_files(self):
        process = mock.Mock(spec=launcher.subprocess.Popen)
        with contextlib.ExitStack() as patches:
            for name in ("_ensure_pixi_external_mods", "_ensure_mcp_addon_installed", "_ensure_mcp_auto_start"):
                patches.enter_context(mock.patch.object(launcher, name))
            patches.enter_context(mock.patch.object(launcher, "_default_freecad", return_value=launcher.sys.executable))
            patches.enter_context(mock.patch.object(launcher, "_port_is_occupied", return_value=False))
            patches.enter_context(mock.patch.object(launcher, "_freecad_user_data_base", return_value=self.repo))
            start = patches.enter_context(mock.patch.object(launcher, "_start_freecad", return_value=process))
            self.assertEqual(launcher.main(["--diagnostics", "--no-wait-for-mcp", "part.FCStd"]), 0)
        argv = start.call_args.args[1]
        self.assertTrue(argv[-2].endswith("GuiDiagnostics.FCMacro"))
        self.assertEqual(argv[-1], "part.FCStd")


class ExitMonitorTests(unittest.TestCase):
    def setUp(self):
        self.directory = Path(tempfile.mkdtemp())
        self.addCleanup(shutil.rmtree, self.directory)
        self.status = self.directory / "exit-status.json"
        self.process = SimpleNamespace(pid=900, poll=mock.Mock(return_value=None))

    def publish(self, value):
        self.status.write_text(json.dumps(value))
        return self.process

    def test_automatic_status_argument_stays_with_helper_and_precedes_macro(self):
        macro = str(self.directory / "GuiDiagnostics.FCMacro")
        status, argv, timeout = launcher._diagnostic_exit_monitor_args([macro, "part.FCStd"])
        self.assertEqual(status, self.status)
        self.assertEqual(argv, ["--exit-status-file", str(self.status), macro, "part.FCStd"])
        self.assertEqual(timeout, 900)

    def test_explicit_status_and_startup_timeout_are_preserved(self):
        original = ["--exit-status-file=" + str(self.status), "--rpc-wait-timeout", "135", "part.FCStd"]
        status, argv, timeout = launcher._diagnostic_exit_monitor_args(original)
        self.assertEqual(status, self.status)
        self.assertEqual(argv, original)
        self.assertEqual(timeout, 135)

    def test_supervised_launch_gets_no_automatic_exit_option(self):
        original = ["--supervise", "GuiDiagnostics.FCMacro"]
        status, argv, _ = launcher._diagnostic_exit_monitor_args(original)
        self.assertIsNone(status)
        self.assertEqual(argv, original)
        with self.assertRaisesRegex(ValueError, "cannot be combined"):
            launcher._diagnostic_exit_monitor_args([*original, "--exit-status-file", str(self.status)])

    def test_invalid_or_repeated_helper_options_are_refused(self):
        for argv in (
            ["--exit-status-file"], ["--exit-status-file="],
            ["--exit-status-file", "a", "--exit-status-file", "b"],
            ["--rpc-wait-timeout=nan"], ["--rpc-wait-timeout=-1"],
        ):
            with self.subTest(argv=argv), self.assertRaises(ValueError):
                launcher._diagnostic_exit_monitor_args(argv)

    def test_fresh_authenticated_pending_status_returns_without_waiting_for_child_exit(self):
        with mock.patch.object(launcher.subprocess, "Popen", side_effect=lambda *a, **k: self.publish({
            "pid": 1000, "returncode": None,
        })) as popen, mock.patch.object(launcher.time, "sleep") as sleep:
            self.assertEqual(launcher._launch_diagnostic_isolated(["helper"], {}, self.status, 10), 0)
        sleep.assert_not_called()
        self.assertEqual(popen.call_args.kwargs["stdin"], launcher.subprocess.DEVNULL)
        self.assertFalse(hasattr(self.process, "terminate"))

    def test_raw_signal_and_normal_early_exit_are_distinguished_from_a_stall(self):
        for code in (-11, 0, 7):
            with self.subTest(code=code):
                self.status.unlink(missing_ok=True)
                with mock.patch.object(launcher.subprocess, "Popen", side_effect=lambda *a, **k: self.publish({
                    "pid": 1000, "returncode": code,
                })):
                    self.assertEqual(launcher._launch_diagnostic_isolated(["helper"], {}, self.status, 10), code or 1)
                self.assertEqual(json.loads(self.status.read_text())["returncode"], code)

    def test_stale_pending_status_is_not_readiness(self):
        self.publish({"pid": 800, "returncode": None})
        self.process.poll.side_effect = [None, 9]
        with mock.patch.object(launcher.subprocess, "Popen", return_value=self.process), mock.patch.object(
            launcher.time, "sleep"
        ):
            self.assertEqual(launcher._launch_diagnostic_isolated(["helper"], {}, self.status, 10), 9)

    def test_status_schema_and_read_size_are_bounded(self):
        for record in ({"pid": True, "returncode": None}, {"pid": 1, "returncode": "0"}, {"pid": 1}, "x" * 5000):
            with self.subTest(record_type=type(record).__name__):
                self.status.unlink(missing_ok=True)
                with mock.patch.object(launcher.subprocess, "Popen", side_effect=lambda *a, **k: self.publish(record)):
                    with self.assertRaises(ValueError):
                        launcher._launch_diagnostic_isolated(["helper"], {}, self.status, 10)

    def test_monitor_timeout_leaves_child_lifecycle_to_existing_helper(self):
        with mock.patch.object(launcher.subprocess, "Popen", return_value=self.process), mock.patch.object(
            launcher.time, "monotonic", side_effect=[0.0, 20.0]
        ):
            self.assertEqual(launcher._launch_diagnostic_isolated(["helper"], {}, self.status, 1), 1)
        self.process.poll.assert_not_called()


if __name__ == "__main__":
    unittest.main()
