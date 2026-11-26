# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regression tests for GUI-runner false greens and hung modules.

These tests do not start FreeCAD. They feed canned listing/run output into
run_gui_tests.py the way pipelines 341-343, a missing-suite listing, and a
zero-test class unit would. Short fake commands verify live output and watchdog cleanup.
"""

from __future__ import annotations

import contextlib
import io
import os
import subprocess
import struct
import sys
import tempfile
import time
import unittest
import zlib
from pathlib import Path
from unittest import mock

import run_gui_tests as runner
from run_gui_tests import (
    EXPECTED_GUI_SUITES,
    NEWLY_REQUIRED_GUI_SUITES,
    SKETCHER_GUI_CLASS_UNITS,
    discover_gui_suites,
    evaluate_unit_result,
    find_console_executable,
    parse_completed_test_count,
    parse_final_outcome,
    parse_registered_tests,
    run_gui_modules,
    run_and_capture,
    units_for_module,
)


CONSOLE_SUITES: tuple[str, ...] = ("BaseTests", "Document", "TestPartApp")
FULL_SUITES = EXPECTED_GUI_SUITES


def _listing(*names: str, with_footer: bool = True) -> str:
    body = "startup noise\nRegistered test units:\n\n" + "\n".join(names) + "\n"
    if with_footer:
        body += (
            "\nPlease choose one or use 0 for all\n\n"
            + "-" * 70
            + "\nRan 0 tests in 0.000s\n\nNO TESTS RAN\nSystem exit\n"
        )
    return body


def _ran(n: int, ok: bool = True) -> str:
    status = "OK" if ok else "FAILED (failures=1)"
    return f"test_foo ... {'ok' if ok else 'FAIL'}\nRan {n} tests in 0.010s\n{status}\n"


def _unittest_output(ran: int, outcome: str | None) -> str:
    body = "test_a (Mod.TestA) ... ok\n" + "-" * 70 + f"\nRan {ran} tests in 0.412s\n\n"
    return body + (outcome + "\n" if outcome is not None else "")


@contextlib.contextmanager
def _dummy_freecad():
    with tempfile.TemporaryDirectory() as tmp:
        gui = Path(tmp) / "FreeCAD"
        console = Path(tmp) / "FreeCADCmd"
        for path in (gui, console):
            path.touch()
            path.chmod(0o755)
        yield str(gui)


def _quiet_run_gui_modules(run) -> int:
    with _dummy_freecad() as gui, contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return run_gui_modules(gui, run=run, crash_helper=False)


def _fake_freecad(
    per_unit: dict[str, tuple[int, str]],
    default=(0, _unittest_output(3, "OK")),
    *,
    console_suites: tuple[str, ...] = CONSOLE_SUITES,
    gui_suites: tuple[str, ...] = (*FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES),
):
    seen: list[str] = []

    def run(cmd: list[str]) -> tuple[int, str]:
        if cmd[-1] == "-t":
            if Path(cmd[0]).name.startswith("FreeCADCmd"):
                return 0, _listing(*console_suites, with_footer=True)
            return 0, _listing(*console_suites, *gui_suites, with_footer=True)
        seen.append(cmd[-1])
        return per_unit.get(cmd[-1], default)

    return run, seen


class ModuleWatchdogTests(unittest.TestCase):
    def test_attach_denied_uses_bounded_parent_rerun_after_killing_original(self):
        with subprocess.Popen(
            [sys.executable, "-c", "import time; time.sleep(30)"], start_new_session=True
        ) as original:
            calls = []

            def gdb_capture(args, timeout, *, interrupt=False):
                calls.append(args)
                if "-p" in args:
                    self.assertIsNone(original.poll())
                    return 1, "ptrace: Operation not permitted.\n"
                self.assertIsNotNone(original.poll())
                self.assertEqual(args[-3:], ["FreeCAD", "-t", "FakeGui"])
                self.assertEqual(timeout, 0.2)
                self.assertTrue(interrupt)
                self.assertIn("thread apply all bt 30", args)
                return 0, "#0 native_hang\n"

            try:
                with mock.patch(
                    "run_gui_tests.shutil.which", return_value="/usr/bin/gdb"
                ), mock.patch(
                    "run_gui_tests._gdb_capture", side_effect=gdb_capture
                ), contextlib.redirect_stdout(
                    io.StringIO()
                ):
                    output = runner._timeout_backtrace(
                        original, ["catchsegv", "stdbuf", "-oL", "FreeCAD", "-t", "FakeGui"], 0.2
                    )
                self.assertIn("#0 native_hang", output)
                self.assertEqual(len(calls), 2)
            finally:
                runner._kill_process_group(original)

    def test_output_is_streamed_before_exit_and_still_returned(self):
        # The child cannot finish until the runner delivers its first line.
        with tempfile.TemporaryDirectory() as tmp:
            release = Path(tmp) / "release"
            script = (
                "import pathlib, time\n"
                "print('first line', flush=True)\n"
                f"release = pathlib.Path({str(release)!r})\n"
                "deadline = time.monotonic() + 2\n"
                "while not release.exists() and time.monotonic() < deadline: time.sleep(.01)\n"
                "print('streamed' if release.exists() else 'buffered', flush=True)\n"
            )

            class LiveOutput(io.StringIO):
                def write(self, value):
                    if "first line" in value:
                        release.touch()
                    return super().write(value)

            stdout = LiveOutput()
            with contextlib.redirect_stdout(stdout):
                rc, output = run_and_capture([sys.executable, "-c", script])
            self.assertEqual(rc, 0)
            self.assertEqual(output, "first line\nstreamed\n")
            self.assertEqual(stdout.getvalue(), output)

    def test_timeout_is_failure_even_after_ok_summary(self):
        script = "import time; print('Ran 1 test\\nOK', flush=True); time.sleep(2)"
        started = time.monotonic()
        with mock.patch("run_gui_tests.shutil.which", return_value=None), mock.patch.dict(
            os.environ, FREECAD_GUI_TEST_MODULE_TIMEOUT="0.2"
        ):
            with contextlib.redirect_stdout(io.StringIO()) as stdout:
                rc, output = run_and_capture([sys.executable, "-c", script, "FakeGui"])
        self.assertEqual(rc, 124)
        self.assertNotEqual(evaluate_unit_result(rc, output)[0], 0)
        self.assertLess(time.monotonic() - started, 1.5)
        self.assertIn("TIMEOUT: FakeGui after 0.2 s", stdout.getvalue())

    def test_timeout_without_summary_keeps_timeout_diagnosis(self):
        rc, error = evaluate_unit_result(124, "TIMEOUT: FakeGui after 900 s\n")
        self.assertEqual(rc, 124)
        self.assertIn("timed out", error)

    @unittest.skipUnless(sys.platform == "linux", "Linux process groups and /proc")
    def test_timeout_kills_children_including_when_leader_exits(self):
        for new_group in (False, True):
            with self.subTest(new_group=new_group):
                with tempfile.TemporaryDirectory() as tmp:
                    pidfile = Path(tmp) / "child.pid"
                    script = (
                        "import subprocess, sys, pathlib, time, os\n"
                        f"child = subprocess.Popen([sys.executable, '-c', 'import time; time.sleep(30)'], preexec_fn=os.setpgrp if {new_group} else None)\n"
                        f"pathlib.Path({str(pidfile)!r}).write_text(str(child.pid))\n"
                        "print('child started', flush=True)\n"
                        # Leader exits, but its child keeps stdout open.
                    )
                    with mock.patch(
                        "run_gui_tests.shutil.which", return_value=None
                    ), mock.patch.dict(os.environ, FREECAD_GUI_TEST_MODULE_TIMEOUT="0.2"):
                        with contextlib.redirect_stdout(io.StringIO()):
                            rc, _ = run_and_capture([sys.executable, "-c", script, "FakeGui"])
                    pid = int(pidfile.read_text())
                    status = Path(f"/proc/{pid}/stat")
                    deadline = time.monotonic() + 1
                    while True:
                        try:
                            if status.read_text().split()[2] == "Z":
                                break
                        except (FileNotFoundError, ProcessLookupError):
                            break
                        if time.monotonic() > deadline:
                            os.kill(pid, 9)
                            self.fail(f"child {pid} survived module timeout (rc={rc})")
                        time.sleep(0.01)
                    self.assertNotEqual(rc, 0)


class ParseTests(unittest.TestCase):
    def test_parse_registered_tests_skips_preamble(self):
        names = parse_registered_tests(_listing("GuiDocument", "TestPartGui"))
        self.assertEqual(names, ["GuiDocument", "TestPartGui"])

    def test_parse_registered_tests_stops_at_footer_and_ignores_shutdown_output(self):
        output = (
            _listing("Workbench", "TestPartPreview", "AssemblyTests.TestIsolateSelectionGui", with_footer=True)
            + "ShutdownWord\n"
        )
        names = parse_registered_tests(output)
        self.assertEqual(
            names,
            ["Workbench", "TestPartPreview", "AssemblyTests.TestIsolateSelectionGui"],
        )

    def test_parse_registered_tests_stops_at_unittest_trailer_without_footer(self):
        output = (
            "Registered test units:\n\n"
            "Workbench\n"
            "TestPartPreview\n\n"
            + "-" * 70
            + "\nRan 0 tests in 0.000s\n\nOK\nSystem exit\n"
        )
        self.assertEqual(parse_registered_tests(output), ["Workbench", "TestPartPreview"])

    def test_parse_registered_tests_handles_console_preamble_and_non_identifier_noise(self):
        output = (
            "\n"
            + "-" * 70
            + "\nRan 0 tests in 0.000s\n\nNO TESTS RAN\n"
            "FreeCAD 27.1.0\n\n"
            "Registered test units:\n\n"
            "BaseTests\n"
            "Warning: stray diagnostic message\n"
            "Document\n\n"
            "Please choose one or use 0 for all\n"
        )
        self.assertEqual(parse_registered_tests(output), ["BaseTests", "Document"])

    def test_find_console_executable(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            for name in ("FreeCAD", "FreeCADCmd", "FreeCAD.exe", "FreeCADCmd.exe"):
                path = directory / name
                path.touch()
                path.chmod(0o755)
            with mock.patch.dict(os.environ, {"PATH": str(directory)}):
                self.assertEqual(find_console_executable("FreeCAD"), str(directory / "FreeCADCmd"))
                self.assertEqual(find_console_executable("FreeCAD.exe"), str(directory / "FreeCADCmd.exe"))
                self.assertEqual(find_console_executable("FreeCADCmd"), str(directory / "FreeCADCmd"))
                self.assertEqual(find_console_executable(str(directory / "FreeCAD")), str(directory / "FreeCADCmd"))
                self.assertIsNone(find_console_executable("custom-binary"))

    def test_parse_completed_test_count_uses_last_summary(self):
        output = "Ran 2 tests in 0.1s\nOK\nRan 12 tests in 4.6s\nOK\n"
        self.assertEqual(parse_completed_test_count(output), 12)

    def test_parse_completed_test_count_missing(self):
        self.assertIsNone(parse_completed_test_count("System exit\n"))


class DiscoveryFalseGreenTests(unittest.TestCase):
    def test_failed_listing_is_preserved(self):
        rc, suites, err = discover_gui_suites(
            1, _listing(*CONSOLE_SUITES, *FULL_SUITES), 0, _listing(*CONSOLE_SUITES)
        )
        self.assertEqual(rc, 1)
        self.assertEqual(suites, [])
        self.assertIn("discovery failed", err.lower())

    def test_empty_list_after_clean_listing_is_failure(self):
        # Pipelines 341-343: ~7s, no modules, exit 0.
        rc, suites, err = discover_gui_suites(
            0, "FreeCAD 26.3.0\nSystem exit\n", 0, _listing(*CONSOLE_SUITES)
        )
        self.assertEqual(rc, 2)
        self.assertEqual(suites, [])
        self.assertIn("Registered test units", err)

    def test_header_without_units_is_failure(self):
        rc, _, err = discover_gui_suites(
            0, "Registered test units:\n\n", 0, _listing(*CONSOLE_SUITES)
        )
        self.assertEqual(rc, 2)
        self.assertIn("no registered units", err.lower())

    def test_missing_expected_suite_is_failure(self):
        names = [n for n in FULL_SUITES if n != "TestSketcherGui"]
        rc, suites, err = discover_gui_suites(
            0, _listing(*CONSOLE_SUITES, *names), 0, _listing(*CONSOLE_SUITES)
        )
        self.assertEqual(rc, 3)
        self.assertNotIn("TestSketcherGui", suites)
        self.assertIn("TestSketcherGui", err)

    def test_full_expected_set_discovers(self):
        rc, suites, err = discover_gui_suites(
            0,
            _listing(*CONSOLE_SUITES, *FULL_SUITES, "TestExtraGui", with_footer=True),
            0,
            _listing(*CONSOLE_SUITES, with_footer=True),
        )
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertIn("TestExtraGui", suites)
        for name in FULL_SUITES:
            self.assertIn(name, suites)
        for name in CONSOLE_SUITES:
            self.assertNotIn(name, suites)

    def test_discovers_gui_suites_without_gui_in_name(self):
        nameless = (
            "Workbench",
            "Menu",
            "TestGraphicsViewWrapping",
            "TestMDIView",
            "TestRubberbandSelection",
            "TestSelectionVisual",
            "TestCornerAxisCrossVisual",
            "TestCoinNodeSnapshots",
            "TestViewProviderLink",
            "TestPartPreview",
        )
        rc, suites, err = discover_gui_suites(
            0,
            _listing(*CONSOLE_SUITES, *FULL_SUITES, *nameless, with_footer=True),
            0,
            _listing(*CONSOLE_SUITES, with_footer=True),
        )
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertEqual(suites, [*FULL_SUITES, *nameless])

    def test_preserves_shared_name_suite_registered_in_both_init_and_init_gui(self):
        console = (*CONSOLE_SUITES, "SharedSuite")
        gui = (*CONSOLE_SUITES, "SharedSuite", "TestPartPreview", "SharedSuite", *FULL_SUITES)
        rc, suites, err = discover_gui_suites(
            0,
            _listing(*gui, with_footer=True),
            0,
            _listing(*console, with_footer=True),
        )
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertEqual(suites, ["TestPartPreview", "SharedSuite", *FULL_SUITES])

    def test_preserves_shared_name_gui_or_expected_suite_in_both_registries(self):
        console = (*CONSOLE_SUITES, "TestSharedGui", "GuiDocument")
        gui = (*CONSOLE_SUITES, "TestSharedGui", *FULL_SUITES, "TestPartPreview")
        rc, suites, err = discover_gui_suites(
            0,
            _listing(*gui, with_footer=True),
            0,
            _listing(*console, with_footer=True),
        )
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertEqual(suites, ["TestSharedGui", *FULL_SUITES, "TestPartPreview"])

    def test_preserves_registration_order_and_deduplicates_duplicates(self):
        console = ("BaseTests", "BaseTests", "Document")
        gui = (
            "BaseTests",
            "BaseTests",
            "Document",
            "Workbench",
            "Menu",
            "Workbench",
            *FULL_SUITES,
            "TestPartPreview",
            "TestPartPreview",
        )
        rc, suites, err = discover_gui_suites(
            0,
            _listing(*gui, with_footer=True),
            0,
            _listing(*console, with_footer=True),
        )
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertEqual(suites, ["Workbench", "Menu", *FULL_SUITES, "TestPartPreview"])

    def test_failed_console_listing_is_preserved(self):
        rc, suites, err = discover_gui_suites(
            0, _listing(*CONSOLE_SUITES, *FULL_SUITES), 19, "boom\n"
        )
        self.assertEqual(rc, 19)
        self.assertEqual(suites, [])
        self.assertIn("console test discovery failed", err.lower())

        sig_rc, sig_suites, sig_err = discover_gui_suites(
            0, _listing(*CONSOLE_SUITES, *FULL_SUITES), -11, ""
        )
        self.assertEqual(sig_rc, 1)
        self.assertEqual(sig_suites, [])
        self.assertIn("console test discovery failed", sig_err.lower())

    def test_malformed_console_listing_missing_header_is_failure(self):
        rc, suites, err = discover_gui_suites(
            0, _listing(*CONSOLE_SUITES, *FULL_SUITES), 0, "FreeCADCmd 27.1.0\n"
        )
        self.assertEqual(rc, 2)
        self.assertEqual(suites, [])
        self.assertIn("Registered test units", err)

    def test_empty_console_listing_with_footer_is_failure(self):
        rc, suites, err = discover_gui_suites(
            0,
            _listing(*CONSOLE_SUITES, *FULL_SUITES, with_footer=True),
            0,
            "Registered test units:\n\nPlease choose one or use 0 for all\n",
        )
        self.assertEqual(rc, 2)
        self.assertEqual(suites, [])
        self.assertIn("console test discovery listed no registered units", err.lower())

    def test_malformed_listing_with_only_garbage_lines_is_failure(self):
        garbage = "Registered test units:\n\nnot a valid suite\nPlease choose one or use 0 for all\n"
        rc_gui, suites_gui, err_gui = discover_gui_suites(
            0, garbage, 0, _listing(*CONSOLE_SUITES, with_footer=True)
        )
        self.assertEqual(rc_gui, 2)
        self.assertEqual(suites_gui, [])
        self.assertIn("no registered units", err_gui.lower())

        rc_cmd, suites_cmd, err_cmd = discover_gui_suites(
            0, _listing(*CONSOLE_SUITES, *FULL_SUITES, with_footer=True), 0, garbage
        )
        self.assertEqual(rc_cmd, 2)
        self.assertEqual(suites_cmd, [])
        self.assertIn("console test discovery listed no registered units", err_cmd.lower())

    def test_gui_listing_matching_console_only_fails_even_without_expected_suites(self):
        rc, suites, err = discover_gui_suites(
            0,
            _listing(*CONSOLE_SUITES, with_footer=True),
            0,
            _listing(*CONSOLE_SUITES, with_footer=True),
            expected_suites=(),
        )
        self.assertEqual(rc, 3)
        self.assertEqual(suites, [])
        self.assertIn("No GUI tests found", err)


class CompletedTestsAndAggregate(unittest.TestCase):
    def test_aggregate_testsketcher_gui_is_first_unit(self):
        units = units_for_module("TestSketcherGui")
        self.assertEqual(units[0], "TestSketcherGui")
        self.assertEqual(tuple(units[1:]), SKETCHER_GUI_CLASS_UNITS)

    def test_other_modules_are_not_split(self):
        self.assertEqual(units_for_module("GuiDocument"), ["GuiDocument"])

    def test_zero_tests_is_failure_even_on_exit_zero(self):
        rc, err = evaluate_unit_result(0, "Ran 0 tests in 0.001s\nOK\n")
        self.assertEqual(rc, 4)
        self.assertIn("0 tests", err)

    def test_missing_ran_summary_is_failure(self):
        rc, err = evaluate_unit_result(0, "System exit\n")
        self.assertEqual(rc, 4)
        self.assertIn("Ran N tests", err)

    def test_segfault_stops_as_failure(self):
        rc, err = evaluate_unit_result(-11, "")
        self.assertEqual(rc, 1)
        self.assertIn("segfault", err.lower())

    def test_successful_run_counts(self):
        rc, err = evaluate_unit_result(0, _ran(12))
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")


class UnitVerdicts(unittest.TestCase):
    """evaluate_unit_result(rc, output) -- the per-unit gate."""

    def test_exit0_with_failed_outcome_is_rejected(self):
        rc, _ = evaluate_unit_result(0, _unittest_output(12, "FAILED (failures=1)"))
        self.assertNotEqual(rc, 0)

    def test_exit0_with_errors_outcome_is_rejected(self):
        rc, _ = evaluate_unit_result(0, _unittest_output(12, "FAILED (errors=2, skipped=1)"))
        self.assertNotEqual(rc, 0)

    def test_exit0_positive_count_without_final_outcome_is_rejected(self):
        rc, _ = evaluate_unit_result(0, _unittest_output(12, None))
        self.assertNotEqual(rc, 0)

    def test_zero_tests_python312_wording_is_rejected(self):
        rc, _ = evaluate_unit_result(0, _unittest_output(0, "NO TESTS RAN"))
        self.assertNotEqual(rc, 0)

    def test_normal_success_is_accepted(self):
        self.assertEqual(evaluate_unit_result(0, _unittest_output(12, "OK")), (0, ""))

    def test_partial_skip_is_accepted(self):
        rc, err = evaluate_unit_result(0, _unittest_output(12, "OK (skipped=4)"))
        self.assertEqual((rc, err), (0, ""))

    def test_diagnostic_text_mentioning_failed_is_not_an_outcome(self):
        out = (
            "RPC GUI dispatch FAILED on attempt 1, retrying\n"
            "Report view: FAILED to load optional icon\n" + _unittest_output(7, "OK")
        )
        self.assertEqual(evaluate_unit_result(0, out), (0, ""))

    def test_outcome_must_belong_to_last_summary(self):
        out = _unittest_output(2, "OK") + _unittest_output(12, None)
        rc, _ = evaluate_unit_result(0, out)
        self.assertNotEqual(rc, 0)

    def test_nonzero_exit_with_ok_outcome_is_rejected(self):
        rc, _ = evaluate_unit_result(1, _unittest_output(12, "OK"))
        self.assertNotEqual(rc, 0)

    def test_parse_final_outcome_reads_skipped_counts(self):
        outcome, counts = parse_final_outcome(_unittest_output(12, "OK (skipped=4)"))
        self.assertEqual(outcome, "OK")
        self.assertEqual(counts, {"skipped": 4})


class GateVerdicts(unittest.TestCase):
    """run_gui_modules(...) -- the whole e2e gate as freecad-e2e.sh runs it."""

    def test_newly_discovered_suites_execute_and_failure_fails_gate(self):
        nameless = (
            "Workbench",
            "Menu",
            "TestGraphicsViewWrapping",
            "TestMDIView",
            "TestRubberbandSelection",
            "TestSelectionVisual",
            "TestCornerAxisCrossVisual",
            "TestCoinNodeSnapshots",
            "TestViewProviderLink",
            "TestPartPreview",
        )
        run, seen = _fake_freecad(
            {"Workbench": (1, _unittest_output(2, "FAILED (failures=1)"))},
            gui_suites=(*FULL_SUITES, *nameless),
        )
        self.assertNotEqual(_quiet_run_gui_modules(run), 0)
        self.assertEqual([name for name in seen if name in nameless], list(nameless))

    def test_console_discovery_failure_runs_no_suites(self):
        calls = []

        def run(cmd):
            calls.append(cmd)
            if Path(cmd[0]).name.startswith("FreeCADCmd"):
                return 17, "console listing failed\n"
            return 0, _listing(*CONSOLE_SUITES, *FULL_SUITES, with_footer=True)

        self.assertEqual(_quiet_run_gui_modules(run), 17)
        self.assertEqual([cmd[-1] for cmd in calls], ["-t", "-t"])

    def test_exit0_failed_outcome_in_mandatory_suite_fails_gate(self):
        run, _ = _fake_freecad({"TestPartGui": (0, _unittest_output(9, "FAILED (failures=1)"))})
        self.assertNotEqual(_quiet_run_gui_modules(run), 0)

    def test_exit0_without_final_outcome_fails_gate(self):
        run, _ = _fake_freecad({"TestDraftGui": (0, _unittest_output(9, None))})
        self.assertNotEqual(_quiet_run_gui_modules(run), 0)

    def test_every_test_skipped_in_mandatory_suite_fails_gate(self):
        run, _ = _fake_freecad({"TestCAMGui": (0, _unittest_output(6, "OK (skipped=6)"))})
        self.assertNotEqual(_quiet_run_gui_modules(run), 0)

    def test_all_skipped_in_non_mandatory_extra_unit_is_allowed(self):
        run, seen = _fake_freecad(
            {
                "SketcherTests.TestOnViewParameterGui.TestOnViewParameterGui": (
                    0,
                    _unittest_output(2, "OK (skipped=2)"),
                )
            }
        )
        self.assertEqual(_quiet_run_gui_modules(run), 0)
        self.assertIn("SketcherTests.TestOnViewParameterGui.TestOnViewParameterGui", seen)

    def test_partial_skip_in_mandatory_suite_passes_gate(self):
        run, _ = _fake_freecad({"TestSketcherGui": (0, _unittest_output(40, "OK (skipped=3)"))})
        self.assertEqual(_quiet_run_gui_modules(run), 0)

    def test_failed_aggregate_rc1_then_passing_classes_fails_gate(self):
        run, seen = _fake_freecad({"TestSketcherGui": (1, _unittest_output(40, "FAILED (failures=2)"))})
        self.assertNotEqual(_quiet_run_gui_modules(run), 0)
        self.assertEqual(seen[seen.index("TestSketcherGui") + 1], SKETCHER_GUI_CLASS_UNITS[0])

    def test_failed_aggregate_exit0_then_passing_classes_fails_gate(self):
        run, _ = _fake_freecad({"TestSketcherGui": (0, _unittest_output(40, "FAILED (failures=2)"))})
        self.assertNotEqual(_quiet_run_gui_modules(run), 0)

    def test_normal_successful_run_passes_gate(self):
        run, seen = _fake_freecad({})
        self.assertEqual(_quiet_run_gui_modules(run), 0)
        for suite in FULL_SUITES:
            self.assertIn(suite, seen)


class Task76ReviewRegressions(unittest.TestCase):
    def test_truncated_gui_listing_with_all_old_required_suites_fails(self):
        rc, suites, _ = discover_gui_suites(
            0, _listing(*CONSOLE_SUITES, *FULL_SUITES, with_footer=False),
            0, _listing(*CONSOLE_SUITES, with_footer=True),
        )
        self.assertNotEqual(rc, 0)
        self.assertEqual(suites, [])

    def test_truncated_console_listing_fails(self):
        rc, suites, _ = discover_gui_suites(
            0, _listing(*CONSOLE_SUITES, *FULL_SUITES, with_footer=True),
            0, _listing(*CONSOLE_SUITES, with_footer=False),
        )
        self.assertNotEqual(rc, 0)
        self.assertEqual(suites, [])

    def test_trailer_before_footer_is_not_a_complete_listing(self):
        incomplete = _listing(*CONSOLE_SUITES, *FULL_SUITES, with_footer=False) + (
            "-----" * 14 + "\nRan 0 tests in 0.0s\nOK\n"
            "Please choose one or use 0 for all\n"
        )
        rc, suites, _ = discover_gui_suites(
            0, incomplete, 0, _listing(*CONSOLE_SUITES, with_footer=True)
        )
        self.assertNotEqual(rc, 0)
        self.assertEqual(suites, [])

    def test_all_skipped_linkview_fails_real_runner_verdict(self):
        run, seen = _fake_freecad(
            {"TestViewProviderLink": (0, _unittest_output(8, "OK (skipped=8)"))},
            gui_suites=(*FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES),
        )
        self.assertNotEqual(_quiet_run_gui_modules(run), 0)
        self.assertIn("TestViewProviderLink", seen)

    def test_missing_gui_sibling_does_not_use_other_path_console(self):
        with tempfile.TemporaryDirectory() as tmp:
            first, second = Path(tmp) / "a", Path(tmp) / "b"
            first.mkdir(); second.mkdir()
            gui = first / "FreeCAD"; gui.touch(); gui.chmod(0o755)
            wrong = second / "FreeCADCmd"; wrong.touch(); wrong.chmod(0o755)
            with mock.patch.dict(os.environ, {"PATH": os.pathsep.join((str(first), str(second)))}):
                self.assertIsNone(find_console_executable("FreeCAD"))

    def test_path_symlink_uses_real_gui_binary_sibling(self):
        with tempfile.TemporaryDirectory() as tmp:
            linkdir, realdir = Path(tmp) / "links", Path(tmp) / "real"
            linkdir.mkdir(); realdir.mkdir()
            real = realdir / "FreeCAD"; real.touch(); real.chmod(0o755)
            sibling = realdir / "FreeCADCmd"; sibling.touch(); sibling.chmod(0o755)
            (linkdir / "FreeCAD").symlink_to(real)
            with mock.patch.dict(os.environ, {"PATH": str(linkdir)}):
                self.assertEqual(find_console_executable("FreeCAD"), str(sibling))

    def test_each_new_suite_requires_a_non_skipped_test(self):
        for suite in NEWLY_REQUIRED_GUI_SUITES:
            with self.subTest(suite=suite):
                run, seen = _fake_freecad({suite: (0, _unittest_output(3, "OK (skipped=3)"))})
                self.assertNotEqual(_quiet_run_gui_modules(run), 0)
                self.assertIn(suite, seen)

    def test_partial_skip_in_linkview_passes(self):
        run, seen = _fake_freecad({"TestViewProviderLink": (0, _unittest_output(8, "OK (skipped=7)"))})
        self.assertEqual(_quiet_run_gui_modules(run), 0)
        self.assertIn("TestViewProviderLink", seen)

    def test_truncated_listings_fail_end_to_end_without_running_units(self):
        for truncated in ("gui", "console"):
            with self.subTest(truncated=truncated):
                calls = []
                def run(cmd):
                    calls.append(cmd)
                    if cmd[-1] != "-t":
                        return 0, _ran(1)
                    is_console = Path(cmd[0]).name.startswith("FreeCADCmd")
                    names = CONSOLE_SUITES if is_console else (*CONSOLE_SUITES, *FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES)
                    return 0, _listing(*names, with_footer=(truncated != ("console" if is_console else "gui")))
                self.assertNotEqual(_quiet_run_gui_modules(run), 0)
                self.assertTrue(all(cmd[-1] == "-t" for cmd in calls))

    def test_split_path_never_runs_the_other_console(self):
        with tempfile.TemporaryDirectory() as tmp:
            first, second = Path(tmp) / "a", Path(tmp) / "b"
            first.mkdir(); second.mkdir()
            for path in (first / "FreeCAD", second / "FreeCADCmd"):
                path.touch(); path.chmod(0o755)
            calls = []
            def run(cmd):
                calls.append(cmd)
                return 0, _listing(*CONSOLE_SUITES, *FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES)
            with mock.patch.dict(os.environ, {"PATH": os.pathsep.join((str(first), str(second)))}):
                self.assertNotEqual(run_gui_modules("FreeCAD", run=run, crash_helper=False), 0)
            self.assertEqual(calls, [[str(first / "FreeCAD"), "-t"]])

    def test_symlink_directory_console_does_not_replace_real_sibling(self):
        with tempfile.TemporaryDirectory() as tmp:
            links, real = Path(tmp) / "links", Path(tmp) / "real"
            links.mkdir(); real.mkdir()
            executable = real / "FreeCAD"; executable.touch(); executable.chmod(0o755)
            (links / "FreeCAD").symlink_to(executable)
            wrong = links / "FreeCADCmd"; wrong.touch(); wrong.chmod(0o755)
            with mock.patch.dict(os.environ, {"PATH": str(links)}):
                self.assertIsNone(find_console_executable("FreeCAD"))

    def test_symlink_resolution_is_stable_across_listing_and_execution(self):
        with tempfile.TemporaryDirectory() as tmp:
            links, real, other = (Path(tmp) / name for name in ("links", "real", "other"))
            for directory in (links, real, other):
                directory.mkdir()
            for path in (real / "FreeCAD", real / "FreeCADCmd", other / "FreeCAD", other / "FreeCADCmd"):
                path.touch(); path.chmod(0o755)
            (links / "FreeCAD").symlink_to(real / "FreeCAD")
            calls = []
            def run(cmd):
                calls.append(cmd)
                if len(calls) == 1:
                    os.environ["PATH"] = str(other)
                if cmd[-1] == "-t":
                    names = CONSOLE_SUITES if Path(cmd[0]).name.startswith("FreeCADCmd") else (*CONSOLE_SUITES, *FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES)
                    return 0, _listing(*names)
                return 0, _ran(1)
            with mock.patch.dict(os.environ, {"PATH": str(links)}), contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(run_gui_modules("FreeCAD", run=run, crash_helper=False), 0)
            self.assertEqual(calls[0], [str(real / "FreeCAD"), "-t"])
            self.assertEqual(calls[1], [str(real / "FreeCADCmd"), "-t"])
            self.assertTrue(all(str(real / "FreeCAD") in cmd for cmd in calls[2:]))

    def test_explicit_other_console_is_rejected(self):
        with _dummy_freecad() as gui, tempfile.TemporaryDirectory() as tmp:
            wrong = Path(tmp) / "FreeCADCmd"; wrong.touch(); wrong.chmod(0o755)
            run, seen = _fake_freecad({})
            self.assertNotEqual(run_gui_modules(gui, freecad_cmd_exec=str(wrong), run=run, crash_helper=False), 0)
            self.assertEqual(seen, [])

    def test_foreign_console_symlink_is_rejected_even_as_explicit_override(self):
        with tempfile.TemporaryDirectory() as tmp:
            build, foreign = Path(tmp) / "build", Path(tmp) / "foreign"
            build.mkdir(); foreign.mkdir()
            gui = build / "FreeCAD"; gui.touch(); gui.chmod(0o755)
            other = foreign / "FreeCADCmd"; other.touch(); other.chmod(0o755)
            (build / "FreeCADCmd").symlink_to(other)
            calls = []
            def run(cmd):
                calls.append(cmd)
                return 0, _listing(*CONSOLE_SUITES, *FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES)
            for override in (None, str(other)):
                with self.subTest(override=override), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    calls.clear()
                    self.assertNotEqual(run_gui_modules(str(gui), freecad_cmd_exec=override,
                                                       run=run, crash_helper=False), 0)
                    self.assertEqual(calls, [[str(gui), "-t"]])
            self.assertIsNone(find_console_executable(str(gui)))

    def test_same_build_console_alias_and_explicit_alias_resolve_to_target(self):
        with tempfile.TemporaryDirectory() as tmp:
            build, aliases = Path(tmp) / "build", Path(tmp) / "aliases"
            build.mkdir(); aliases.mkdir()
            gui = build / "FreeCAD"; gui.touch(); gui.chmod(0o755)
            actual = build / "FreeCADCmd-real"; actual.touch(); actual.chmod(0o755)
            (build / "FreeCADCmd").symlink_to(actual)
            alias = aliases / "console"; alias.symlink_to(actual)
            self.assertEqual(find_console_executable(str(gui)), str(actual))
            calls = []
            def run(cmd):
                calls.append(cmd)
                if cmd[-1] == "-t":
                    names = CONSOLE_SUITES if cmd[0] == str(actual) else (*CONSOLE_SUITES, *FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES)
                    return 0, _listing(*names)
                return 0, _ran(1)
            with contextlib.redirect_stdout(io.StringIO()):
                self.assertEqual(run_gui_modules(str(gui), freecad_cmd_exec=str(alias),
                                                run=run, crash_helper=False), 0)
            self.assertEqual(calls[1], [str(actual), "-t"])

    def test_console_symlink_retarget_after_selection_does_not_change_execution(self):
        for explicit in (False, True):
            with self.subTest(explicit=explicit), tempfile.TemporaryDirectory() as tmp:
                build, foreign = Path(tmp) / "build", Path(tmp) / "foreign"
                build.mkdir(); foreign.mkdir()
                gui = build / "FreeCAD"; gui.touch(); gui.chmod(0o755)
                actual = build / "FreeCADCmd-real"; actual.touch(); actual.chmod(0o755)
                other = foreign / "FreeCADCmd"; other.touch(); other.chmod(0o755)
                sibling = build / "FreeCADCmd"; sibling.symlink_to(actual)
                original = runner.find_console_executable
                def select_then_retarget(path):
                    selected = original(path)
                    sibling.unlink(); sibling.symlink_to(other)
                    return selected
                calls = []
                def run(cmd):
                    calls.append(cmd)
                    if cmd[-1] == "-t":
                        names = CONSOLE_SUITES if cmd[0] == str(actual) else (*CONSOLE_SUITES, *FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES)
                        return 0, _listing(*names)
                    return 0, _ran(1)
                with mock.patch.object(runner, "find_console_executable", side_effect=select_then_retarget), contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                    self.assertEqual(run_gui_modules(str(gui), freecad_cmd_exec=str(actual) if explicit else None,
                                                    run=run, crash_helper=False), 0)
                self.assertEqual(calls[1], [str(actual), "-t"])


class EndToEndFakeFreeCAD(unittest.TestCase):
    def setUp(self):
        self.context = _dummy_freecad()
        self.gui = self.context.__enter__()
        self.addCleanup(self.context.__exit__, None, None, None)

    def test_empty_discovery_does_not_return_zero(self):
        def run(cmd):
            self.assertEqual(cmd[-1], "-t")
            return 0, "FreeCAD ready\n"

        self.assertEqual(run_gui_modules(self.gui, run=run, crash_helper=False), 2)

    def test_missing_suite_does_not_return_zero(self):
        def run(cmd):
            return 0, _listing("GuiDocument")

        self.assertEqual(run_gui_modules(self.gui, run=run, crash_helper=False), 3)

    def test_failed_discovery_exit_is_preserved(self):
        def run(cmd):
            return 17, "boom\n"

        self.assertEqual(run_gui_modules(self.gui, run=run, crash_helper=False), 17)

    def test_aggregate_sketcher_runs_before_class_units(self):
        seen: list[str] = []

        def run(cmd):
            if cmd[-1] == "-t":
                if Path(cmd[0]).name.startswith("FreeCADCmd"):
                    return 0, _listing(*CONSOLE_SUITES)
                return 0, _listing(*CONSOLE_SUITES, *FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES)
            seen.append(cmd[-1])
            return 0, _ran(5)

        rc = run_gui_modules(self.gui, run=run, crash_helper=False)
        self.assertEqual(rc, 0)
        sketcher = [name for name in seen if name == "TestSketcherGui" or name.startswith("SketcherTests.")]
        self.assertEqual(sketcher[0], "TestSketcherGui")
        self.assertEqual(tuple(sketcher[1:]), SKETCHER_GUI_CLASS_UNITS)

    def test_zero_test_module_fails_the_gate(self):
        def run(cmd):
            if cmd[-1] == "-t":
                if Path(cmd[0]).name.startswith("FreeCADCmd"):
                    return 0, _listing(*CONSOLE_SUITES, with_footer=True)
                return 0, _listing(*CONSOLE_SUITES, *FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES, with_footer=True)
            if cmd[-1] == "GuiDocument":
                return 0, "Ran 0 tests in 0.0s\nOK\n"
            return 0, _ran(1)

        self.assertEqual(run_gui_modules(self.gui, run=run, crash_helper=False), 4)

    def test_runs_nameless_gui_suites_and_skips_console_only_suites(self):
        extra = ("Workbench", "Menu", "TestPartPreview")
        run, seen = _fake_freecad({}, gui_suites=(*FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES))
        self.assertEqual(_quiet_run_gui_modules(run), 0)
        for suite in (*FULL_SUITES, *extra):
            self.assertIn(suite, seen)
        for suite in CONSOLE_SUITES:
            self.assertNotIn(suite, seen)
        self.assertNotIn("Please choose one or use 0 for all", seen)
        self.assertNotIn("NO TESTS RAN", seen)
        self.assertNotIn("System exit", seen)

    def test_failed_console_discovery_fails_gate_without_running_modules(self):
        seen: list[str] = []

        def run(cmd):
            if cmd[-1] == "-t":
                if Path(cmd[0]).name.startswith("FreeCADCmd"):
                    return 13, "console failed\n"
                return 0, _listing(*CONSOLE_SUITES, *FULL_SUITES, with_footer=True)
            seen.append(cmd[-1])
            return 0, _ran(1)

        self.assertEqual(_quiet_run_gui_modules(run), 13)
        self.assertEqual(seen, [])

    def test_empty_console_discovery_fails_gate_without_running_modules(self):
        seen: list[str] = []

        def run(cmd):
            if cmd[-1] == "-t":
                if Path(cmd[0]).name.startswith("FreeCADCmd"):
                    return 0, "Registered test units:\n\nPlease choose one or use 0 for all\n"
                return 0, _listing(*CONSOLE_SUITES, *FULL_SUITES, with_footer=True)
            seen.append(cmd[-1])
            return 0, _ran(1)

        self.assertEqual(_quiet_run_gui_modules(run), 2)
        self.assertEqual(seen, [])

    def test_missing_companion_console_executable_fails_gate(self):
        with tempfile.TemporaryDirectory() as tmp:
            gui = Path(tmp) / "custom_gui"
            gui.touch()
            gui.chmod(0o755)
            def run(cmd):
                self.assertEqual(cmd, [str(gui), "-t"])
                return 0, _listing(*CONSOLE_SUITES, *FULL_SUITES, with_footer=True)

            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
                rc = run_gui_modules(str(gui), run=run, crash_helper=False)
            self.assertEqual(rc, 2)


def _build_fcrash(path: Path, frames: list[tuple[str, int]], *, code: int) -> None:
    """Write a v1 .fcrash with the same layout as src/Base/CrashReporter/Format.h."""
    strings = bytearray()
    packed_frames = bytearray()
    for module, offset in frames:
        encoded = module.encode()
        strings += struct.pack("<H", len(encoded)) + encoded
        string_offset = len(strings) - (2 + len(encoded))
        packed_frames += struct.pack("<QQII", 0x1000 + offset, offset, string_offset, 0)
    frame_table = 128
    string_table = frame_table + len(packed_frames)
    file_size = string_table + len(strings) + 4
    header = struct.pack(
        "<IIQQqIIIIIIIIIIBBBBB",
        0x52434346,
        1,
        0,
        1,
        0,
        42,
        code,
        len(frames),
        file_size,
        0,
        frame_table,
        string_table,
        0xFFFFFFFF,
        0xFFFFFFFF,
        0xFFFFFFFF,
        2,
        1,
        27,
        1,
        0,
    )
    if len(header) != 77:
        raise AssertionError(len(header))
    body = header + bytes(51) + bytes(packed_frames) + bytes(strings)
    if len(body) != file_size - 4:
        raise AssertionError((len(body), file_size))
    path.write_bytes(body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF))


def _capture_modules(run, **kwargs) -> tuple[int, str, str]:
    stdout, stderr = io.StringIO(), io.StringIO()
    with _dummy_freecad() as gui, contextlib.redirect_stdout(stdout), contextlib.redirect_stderr(stderr):
        rc = run_gui_modules(gui, run=run, crash_helper=False, **kwargs)
    return rc, stdout.getvalue(), stderr.getvalue()


class CrashReportDiagnostics(unittest.TestCase):
    def test_signal_death_prints_decoded_frames(self):
        cases = ((-11, 11, "SIGSEGV"), (-6, 6, "SIGABRT"), (245, 11, "SIGSEGV"))
        for child_rc, fault, fault_name in cases:
            with self.subTest(rc=child_rc):
                with tempfile.TemporaryDirectory() as tmp:
                    directory = Path(tmp)
                    _build_fcrash(
                        directory / "crash-1-1.fcrash",
                        [("libStale.so", 0x10)],
                        code=fault,
                    )

                    def run(cmd, _rc=child_rc, _fault=fault, _dir=directory):
                        if Path(cmd[0]).name == "gdb":
                            return 0, "thread apply all bt"
                        if cmd[-1] == "-t":
                            if Path(cmd[0]).name.startswith("FreeCADCmd"):
                                return 0, _listing(*CONSOLE_SUITES)
                            return 0, _listing(*CONSOLE_SUITES, *FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES)
                        if cmd[-1] == "GuiDocument":
                            _build_fcrash(
                                _dir / "crash-9-9.fcrash",
                                [("libFreeCADApp.so", 0x1234)],
                                code=_fault,
                            )
                            return _rc, ""
                        return 0, _ran(1)

                    def symbolize(module: str, offset: int) -> str | None:
                        if module == "libFreeCADApp.so" and offset == 0x1234:
                            return "App::Document::recompute at src/App/Document.cpp:10"
                        return None

                    def which(name: str) -> str | None:
                        return "/usr/bin/gdb" if name == "gdb" else None

                    with mock.patch("run_gui_tests.shutil.which", side_effect=which):
                        rc, _stdout, stderr = _capture_modules(
                            run,
                            crash_reports_dir=directory,
                            symbolize=symbolize,
                        )
                    self.assertEqual(rc, 1)
                    self.assertIn(
                        "#0 libFreeCADApp.so+0x1234 "
                        "App::Document::recompute at src/App/Document.cpp:10",
                        stderr,
                    )
                    self.assertIn(fault_name, stderr)
                    self.assertNotIn("libStale.so", stderr)
                    self.assertLess(
                        stderr.index("libFreeCADApp.so+0x1234"),
                        stderr.index("Re-running GuiDocument under gdb"),
                    )

    def test_signal_death_without_report_prints_one_line(self):
        def run(cmd):
            if Path(cmd[0]).name == "gdb":
                return 0, "thread apply all bt"
            if cmd[-1] == "-t":
                if Path(cmd[0]).name.startswith("FreeCADCmd"):
                    return 0, _listing(*CONSOLE_SUITES)
                return 0, _listing(*CONSOLE_SUITES, *FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES)
            if cmd[-1] == "GuiDocument":
                return -11, ""
            return 0, _ran(1)

        with tempfile.TemporaryDirectory() as tmp:
            rc, _stdout, stderr = _capture_modules(
                run, crash_reports_dir=Path(tmp)
            )
        self.assertEqual(rc, 1)
        self.assertEqual(stderr.count("No crash report (.fcrash) found."), 1)
        self.assertLess(
            stderr.index("No crash report (.fcrash) found."),
            stderr.index("Re-running") if "Re-running" in stderr else len(stderr),
        )

    def test_clean_exit_prints_nothing_about_crash_reports(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            _build_fcrash(directory / "crash-1-1.fcrash", [("libStale.so", 0x10)], code=11)

            def run(cmd):
                if cmd[-1] == "-t":
                    if Path(cmd[0]).name.startswith("FreeCADCmd"):
                        return 0, _listing(*CONSOLE_SUITES)
                    return 0, _listing(*CONSOLE_SUITES, *FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES)
                return 0, _ran(1)

            rc, _stdout, stderr = _capture_modules(run, crash_reports_dir=directory)
        self.assertEqual(rc, 0)
        self.assertNotIn("Crash report", stderr)
        self.assertNotIn("No crash report", stderr)
        self.assertNotIn("libStale.so", stderr)


class SupportedLauncherTests(unittest.TestCase):
    def test_default_modes_use_resolved_gui_and_companion_without_build_cache(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for mode in ("installed", "path", "direct", "macos"):
                with self.subTest(mode=mode):
                    directory = (root / mode / "FreeCAD.app/Contents/MacOS" if mode == "macos"
                                 else root / mode / "bin")
                    directory.mkdir(parents=True)
                    gui, console = directory / "FreeCAD", directory / "FreeCADCmd"
                    for exe in (gui, console):
                        exe.write_text("#!/bin/sh\nexit 0\n")
                        exe.chmod(0o755)
                    alias_dir = root / mode / "aliases"
                    alias_dir.mkdir()
                    (alias_dir / "FreeCAD").symlink_to(gui)
                    calls = []
                    def run(cmd):
                        calls.append(cmd)
                        if cmd[-1] == "-t":
                            suites = CONSOLE_SUITES if cmd[0] == str(console) else (*CONSOLE_SUITES, *FULL_SUITES, *NEWLY_REQUIRED_GUI_SUITES)
                            return 0, _listing(*suites)
                        return 0, _ran(1)
                    arg = {"installed": str(root / mode), "path": None,
                           "direct": str(gui), "macos": str(gui)}[mode]
                    real_run = runner.run_gui_modules
                    with mock.patch.dict(os.environ, {"PATH": str(alias_dir)}), \
                         mock.patch.object(runner, "_ensure_headless_gl"), \
                         mock.patch.object(runner, "run_gui_modules", side_effect=lambda exe: real_run(exe, run=run, crash_helper=False, crash_reports_dir=root / "no-reports")), \
                         mock.patch.object(runner, "_log"):
                        self.assertEqual(runner.main(["runner"] + ([arg] if arg else [])), 0)
                    self.assertEqual(calls[0], [str(gui), "-t"])
                    self.assertEqual(calls[1], [str(console), "-t"])
                    self.assertTrue(all(call[0] == str(gui) for call in calls[2:]))


class EmbeddedOriginTests(unittest.TestCase):
    def test_missing_wrong_mixed_and_non_extension_origins(self):
        import importlib.machinery
        import types
        import test_embedded_gui_loop as embedded
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            build, other = root / "lib", root / "other"
            build.mkdir(); other.mkdir()
            good, wrong = build / "FreeCAD.so", other / "FreeCADGui.so"
            good.touch(); wrong.touch()
            loader = importlib.machinery.ExtensionFileLoader("FreeCAD", str(good))
            with mock.patch.object(embedded.importlib.util, "find_spec", return_value=None):
                with self.assertRaises(RuntimeError):
                    embedded.load_build_module("FreeCAD", good)
            for path, candidate_loader in ((wrong, loader), (good, object())):
                spec = types.SimpleNamespace(origin=str(path), loader=candidate_loader)
                with mock.patch.object(embedded.importlib.util, "find_spec", return_value=spec):
                    with self.assertRaises(RuntimeError):
                        embedded.load_build_module("FreeCADGui", good)



if __name__ == "__main__":
    unittest.main()
