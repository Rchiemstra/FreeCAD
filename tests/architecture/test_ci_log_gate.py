# SPDX-License-Identifier: LGPL-2.1-or-later
"""Unit and black-box regression tests for the Woodpecker CI log error gate.

Run with:
    python3 -m unittest discover -s tests/architecture -p test_ci_log_gate.py
or:
    python3 -m pytest tests/architecture/test_ci_log_gate.py

These tests never launch FreeCAD.
"""

from __future__ import annotations

import importlib.util
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
CHECKER_PATH = ROOT / "ci/woodpecker/check-log-errors.py"
BASELINE_PATH = ROOT / "ci/woodpecker/log-error-baseline.json"
E2E_SCRIPT = ROOT / "ci/woodpecker/freecad-e2e.sh"
INTEGRATION_SCRIPT = ROOT / "ci/woodpecker/freecad-integration-tests.sh"
CI_LOGS_DIR = ROOT.parent / "ci-logs"


def _load_checker():
    spec = importlib.util.spec_from_file_location("check_log_errors", CHECKER_PATH)
    module = importlib.util.module_from_spec(spec)
    assert spec.loader is not None
    spec.loader.exec_module(module)
    return module


checker = _load_checker()


class NormalizationAndMatchingTests(unittest.TestCase):
    def test_coin_error_pointer_normalization(self):
        line = (
            "Coin error in SoGroup::removeChild(): tried to remove non-existent "
            "child 0x5a7283d31270 (Separator)"
        )
        self.assertEqual(
            checker.extract_signature(line),
            "Coin error in SoGroup::removeChild(): tried to remove non-existent "
            "child 0x<ADDR> (Separator)",
        )

    def test_coin_warning_and_read_error_after_unittest_prefix(self):
        warn_line = (
            "test_example (AssemblyTests.TestDrag.test_example) ... "
            "Coin warning in SbRotation::setValue(): Quarternion has zero length "
            "=> undefined rotation."
        )
        self.assertEqual(
            checker.extract_signature(warn_line),
            "Coin warning in SbRotation::setValue(): Quarternion has zero length "
            "=> undefined rotation.",
        )
        read_line = "Coin read error: Unexpected EOF in /tmp/fc-scene-abc123/model.iv"
        self.assertEqual(
            checker.extract_signature(read_line),
            "Coin read error: Unexpected EOF in /tmp/<TMP>",
        )

    def test_qt_warnings_including_unflushed_stdout_prefix(self):
        glued = (
            "Select 2 elements from 2 separate parts"
            "QObject::killTimer: Timers cannot be stopped from another thread"
        )
        self.assertEqual(
            checker.extract_signature(glued),
            "QObject::killTimer: Timers cannot be stopped from another thread",
        )
        categorized = "qt.qpa.xcb: could not connect to display :99"
        self.assertEqual(checker.extract_signature(categorized), categorized)
        single_colon = "QXcbConnection: XCB error: 3 (BadWindow) pid=4321"
        self.assertEqual(
            checker.extract_signature(single_colon),
            "QXcbConnection: XCB error: 3 (BadWindow) pid=<PID>",
        )

    def test_freecad_runtime_errors(self):
        self.assertEqual(
            checker.extract_signature(
                "Unhandled Base::Exception caught in GUIApplication::notify."
            ),
            "Unhandled Base::Exception caught in GUIApplication::notify.",
        )
        self.assertEqual(
            checker.extract_signature(
                "ControlSingleton::closeDialog: Cannot close dialog of nullptr document"
            ),
            "ControlSingleton::closeDialog: Cannot close dialog of nullptr document",
        )

    def test_vtk_preamble_and_volatile_fields_normalized(self):
        line_a = (
            "2026-11-21 15:44:42.952 ( 967.500s) [        D0354C80]       "
            "vtkXMLReader.cxx:306    ERR| vtkXMLMultiBlockDataReader "
            "(0x58bfd9fe7920): Error opening file /tmp/vtk_extract_datadir/datafile.vtm"
        )
        line_b = (
            "2026-12-01 01:02:03.456 ( 12.345s) [         97FC6C0]       "
            "vtkXMLReader.cxx:410    ERR| vtkXMLMultiBlockDataReader "
            "(0x7fff12340000): Error opening file /tmp/other_dir_xyz/file.vtm"
        )
        expected = (
            "VTK ERR| vtkXMLMultiBlockDataReader (0x<ADDR>): Error opening file /tmp/<TMP>"
        )
        self.assertEqual(checker.extract_signature(line_a), expected)
        self.assertEqual(checker.extract_signature(line_b), expected)

    def test_volatile_pid_uuid_timestamp_duration_and_var_tmp(self):
        raw = (
            "Coin error: worker pid=98765 PID: 1234 pid 555 "
            "job 123e4567-e89b-12d3-a456-426614174000 at 2026-11-21T19:00:00.123Z "
            "took 12.345s (710ms) writing /var/tmp/scratch_99/out.iv"
        )
        self.assertEqual(
            checker.extract_signature(raw),
            "Coin error: worker pid=<PID> PID: <PID> pid <PID> "
            "job <UUID> at <TIMESTAMP> took <DURATION> (<DURATION>) writing /tmp/<TMP>",
        )

    def test_fatal_and_non_matching_lines(self):
        self.assertEqual(
            checker.extract_signature(
                "Fatal Python error: Segmentation fault in 0xdeadbeef"
            ),
            "Fatal Python error: Segmentation fault in 0x<ADDR>",
        )
        for clean in (
            "test_ok (TestPartGui.TestPartGui.test_ok) ... ok",
            "Ran 12 tests in 0.456s",
            "<TopoShape> TopoShapeExpansion.cpp(1942): unnamed lower element Vertex1",
            "<Exception> division by zero",
            "WARNING: Running pip as the 'root' user",
        ):
            with self.subTest(line=clean):
                self.assertIsNone(checker.extract_signature(clean))


class ModuleAttributionTests(unittest.TestCase):
    def test_e2e_attribution_and_unattributed_preamble(self):
        lines = [
            "Coin warning in startup: before any module",
            "Running GUI tests for module: TestDraftGui",
            "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x1111 (Separator)",
            "Running GUI tests for module: TestArchGui",
            "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x2222 (Switch)",
            "Running GUI tests for module: TestCleanGui",
            "test_clean (TestCleanGui.Case.test_clean) ... ok",
        ]
        fmt, seen, observed = checker.scan_lines(lines)
        self.assertEqual(fmt, "e2e")
        self.assertEqual(
            seen,
            {checker.UNATTRIBUTED_MODULE, "TestDraftGui", "TestArchGui", "TestCleanGui"},
        )
        self.assertEqual(
            observed[checker.UNATTRIBUTED_MODULE],
            {"Coin warning in startup: before any module": 1},
        )
        self.assertEqual(
            observed["TestDraftGui"],
            {
                "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x<ADDR> (Separator)": 1
            },
        )
        self.assertEqual(
            observed["TestArchGui"],
            {
                "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x<ADDR> (Switch)": 1
            },
        )
        self.assertNotIn("TestCleanGui", observed)

    def test_integration_attribution_single_and_two_line_docstring_headers(self):
        lines = [
            "Coin warning in cli_init: before first test",
            "test_first (AlphaMod.Case.test_first) ... ok",
            "test_inline (BetaMod.Case.test_inline) ... Coin warning in Beta: inline",
            "ok",
            "test_with_doc (GammaMod.Case.test_with_doc)",
            "Docstring summary line ... QObject::startTimer: Timers cannot be started from another thread",
            "ok",
        ]
        fmt, seen, observed = checker.scan_lines(lines)
        self.assertEqual(fmt, "integration")
        self.assertEqual(seen, {checker.UNATTRIBUTED_MODULE, "AlphaMod", "BetaMod", "GammaMod"})
        self.assertEqual(
            observed[checker.UNATTRIBUTED_MODULE],
            {"Coin warning in cli_init: before first test": 1},
        )
        self.assertEqual(observed["BetaMod"], {"Coin warning in Beta: inline": 1})
        self.assertEqual(
            observed["GammaMod"],
            {"QObject::startTimer: Timers cannot be started from another thread": 1},
        )

    def test_pytest_attribution(self):
        lines = [
            "Coin warning in startup: before pytest items",
            "tests/live_gui/test_live_gui_cleanup.py .....                            [  2%]",
            "tests/live_gui/test_live_gui_review_group_branch.py .FREECAD DIED during test",
            "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x1111 (Separator)",
            "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x2222 (Switch)",
            "tests/live_gui/test_live_gui_round10.py ........                         [ 17%]",
        ]
        fmt, seen, observed = checker.scan_lines(lines)
        self.assertEqual(fmt, "pytest")
        self.assertEqual(
            seen,
            {
                checker.UNATTRIBUTED_MODULE,
                "tests/live_gui/test_live_gui_cleanup.py",
                "tests/live_gui/test_live_gui_review_group_branch.py",
                "tests/live_gui/test_live_gui_round10.py",
            },
        )
        self.assertEqual(
            observed[checker.UNATTRIBUTED_MODULE],
            {"Coin warning in startup: before pytest items": 1},
        )
        self.assertEqual(
            observed["tests/live_gui/test_live_gui_review_group_branch.py"],
            {
                "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x<ADDR> (Separator)": 1,
                "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x<ADDR> (Switch)": 1,
            },
        )
        self.assertNotIn("tests/live_gui/test_live_gui_cleanup.py", observed)
        self.assertNotIn("tests/live_gui/test_live_gui_round10.py", observed)

    def test_flat_fallback_buckets_deterministically(self):
        lines = [
            "plain log line",
            "Coin error in Foo::bar(): bad child 0xabc",
        ]
        fmt, seen, observed = checker.scan_lines(lines)
        self.assertEqual(fmt, "flat")
        self.assertEqual(seen, {checker.UNATTRIBUTED_MODULE})
        self.assertEqual(
            observed,
            {checker.UNATTRIBUTED_MODULE: {"Coin error in Foo::bar(): bad child 0x<ADDR>": 1}},
        )


class GateSemanticsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.directory = Path(self.temp.name)

    def write_baseline(self, modules: dict) -> Path:
        path = self.directory / "baseline.json"
        path.write_text(json.dumps({"schema_version": 1, "modules": modules}), encoding="utf-8")
        return path

    def write_log(self, text: str, name: str = "test.log") -> Path:
        path = self.directory / name
        path.write_text(text, encoding="utf-8")
        return path

    def run_cli(self, *args) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(CHECKER_PATH), *map(str, args)],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_pass_when_counts_equal_baseline(self):
        sig = "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x<ADDR> (Separator)"
        baseline = self.write_baseline({"TestDraftGui": {sig: 2}, "OtherUnseenMod": {sig: 5}})
        log = self.write_log(
            "Running GUI tests for module: TestDraftGui\n"
            "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x100 (Separator)\n"
            "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x200 (Separator)\n"
        )
        res = self.run_cli("--baseline", baseline, log)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("log-gate: PASS", res.stdout)
        self.assertNotIn("NOTE:", res.stdout)
        self.assertEqual(res.stderr, "")

    def test_pass_with_note_when_counts_below_baseline(self):
        sig_sep = "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x<ADDR> (Separator)"
        sig_sw = "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x<ADDR> (Switch)"
        baseline = self.write_baseline(
            {
                "TestDraftGui": {sig_sep: 3, sig_sw: 2},
                "UnseenModule": {sig_sep: 10},
            }
        )
        # TestDraftGui has 1 Separator (< 3) and 0 Switch (< 2); UnseenModule didn't run.
        log = self.write_log(
            "Running GUI tests for module: TestDraftGui\n"
            "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x100 (Separator)\n"
        )
        res = self.run_cli("--baseline", baseline, log)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("log-gate: PASS", res.stdout)
        self.assertIn("NOTE: [TestDraftGui] below baseline (actual=1, baseline=3):", res.stdout)
        self.assertIn("NOTE: [TestDraftGui] below baseline (actual=0, baseline=2):", res.stdout)
        self.assertNotIn("UnseenModule", res.stdout)

    def test_fail_when_count_exceeds_baseline(self):
        sig = "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x<ADDR> (Separator)"
        baseline = self.write_baseline({"TestDraftGui": {sig: 1}})
        log = self.write_log(
            "Running GUI tests for module: TestDraftGui\n"
            "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x100 (Separator)\n"
            "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x200 (Separator)\n"
        )
        res = self.run_cli("--baseline", baseline, log)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("FAIL: [TestDraftGui] count above baseline (actual=2, baseline=1):", res.stderr)
        self.assertIn(sig, res.stderr)
        self.assertIn("log-gate: FAIL", res.stderr)

    def test_fail_when_new_signature_appears(self):
        sig_old = "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x<ADDR> (Separator)"
        baseline = self.write_baseline({"TestDraftGui": {sig_old: 1}})
        log = self.write_log(
            "Running GUI tests for module: TestDraftGui\n"
            "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x100 (Separator)\n"
            "Coin read error: corrupt scene 0x999\n"
        )
        res = self.run_cli("--baseline", baseline, log)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("FAIL: [TestDraftGui] new error signature (actual=1, baseline=0):", res.stderr)
        self.assertIn("Coin read error: corrupt scene 0x<ADDR>", res.stderr)

    def test_fail_closed_on_invalid_baseline_or_missing_log(self):
        bad_baseline = self.directory / "bad.json"
        bad_baseline.write_text("{not-json", encoding="utf-8")
        log = self.write_log("clean\n")
        res = self.run_cli("--baseline", bad_baseline, log)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("FATAL:", res.stderr)

        good_baseline = self.write_baseline({})
        res_missing = self.run_cli("--baseline", good_baseline, self.directory / "missing.log")
        self.assertNotEqual(res_missing.returncode, 0)
        self.assertIn("FATAL:", res_missing.stderr)

    def test_object_baseline_passes_at_max_and_no_note_between_one_and_max(self):
        sig = (
            "Coin warning in SoDelayQueueSensor::unschedule(): "
            "tried to unschedule a sensor which is not scheduled"
        )
        baseline = self.write_baseline(
            {
                "TestAssemblyGui": {
                    sig: {
                        "max": 4,
                        "observed": "963: 2, 969: 3, 970: 3",
                        "reason": "The sensor is or is not scheduled depending on event-loop timing.",
                    }
                }
            }
        )
        for count in (1, 3, 4):
            with self.subTest(count=count):
                log = self.write_log(
                    "Running GUI tests for module: TestAssemblyGui\n" + f"{sig}\n" * count,
                    name=f"obj_{count}.log",
                )
                res = self.run_cli("--baseline", baseline, log)
                self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
                self.assertIn("log-gate: PASS", res.stdout)
                self.assertIn("below_baseline=0", res.stdout)
                self.assertNotIn("NOTE:", res.stdout)
                self.assertEqual(res.stderr, "")

    def test_object_baseline_fails_at_max_plus_one(self):
        sig = (
            "Coin warning in SoDelayQueueSensor::unschedule(): "
            "tried to unschedule a sensor which is not scheduled"
        )
        baseline = self.write_baseline(
            {
                "TestAssemblyGui": {
                    sig: {
                        "max": 4,
                        "observed": "963: 2, 969: 3, 970: 3",
                        "reason": "The sensor is or is not scheduled depending on event-loop timing.",
                    }
                }
            }
        )
        log = self.write_log(
            "Running GUI tests for module: TestAssemblyGui\n" + f"{sig}\n" * 5
        )
        res = self.run_cli("--baseline", baseline, log)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("FAIL: [TestAssemblyGui] count above baseline max (actual=5, max=4):", res.stderr)
        self.assertIn(sig, res.stderr)
        self.assertIn("log-gate: FAIL", res.stderr)

    def test_object_baseline_prints_removable_note_at_zero(self):
        sig = (
            "Coin warning in SoDelayQueueSensor::unschedule(): "
            "tried to unschedule a sensor which is not scheduled"
        )
        baseline = self.write_baseline(
            {
                "TestAssemblyGui": {
                    sig: {
                        "max": 4,
                        "observed": "963: 2, 969: 3, 970: 3",
                        "reason": "The sensor is or is not scheduled depending on event-loop timing.",
                    }
                },
                "UnseenModule": {
                    sig: {
                        "max": 4,
                        "observed": "963: 2, 969: 3, 970: 3",
                        "reason": "The sensor is or is not scheduled depending on event-loop timing.",
                    }
                },
            }
        )
        log = self.write_log(
            "Running GUI tests for module: TestAssemblyGui\n"
            "test_ok (TestAssemblyGui.Case.test_ok) ... ok\n"
        )
        res = self.run_cli("--baseline", baseline, log)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("log-gate: PASS", res.stdout)
        self.assertIn(
            f"NOTE: [TestAssemblyGui] below baseline (actual=0, max=4): entry can be removed: {sig}",
            res.stdout,
        )
        self.assertNotIn("UnseenModule", res.stdout)
        self.assertEqual(res.stderr, "")

    def test_malformed_object_baseline_entries_fail_with_clear_error(self):
        sig = "Coin warning in SoDelayQueueSensor::unschedule(): tried to unschedule a sensor which is not scheduled"
        log = self.write_log("Running GUI tests for module: TestAssemblyGui\n")
        bad_entries = {
            "missing_max": (
                {"observed": "963: 28, 969: 29, 970: 28", "reason": "timing"},
                "missing required 'max'",
            ),
            "empty_object": ({}, "missing required 'max'"),
            "string_max": ({"max": "35"}, "invalid 'max'"),
            "float_max": ({"max": 35.5}, "invalid 'max'"),
            "bool_max": ({"max": True}, "invalid 'max'"),
            "none_max": ({"max": None}, "invalid 'max'"),
            "negative_max": ({"max": -1}, "invalid 'max'"),
            "unknown_key": ({"max": 35, "limit": 35}, "unexpected key"),
            "non_string_reason": ({"max": 35, "reason": 123}, "'reason'"),
        }
        for name, (entry, expected_msg) in bad_entries.items():
            with self.subTest(case=name):
                baseline = self.write_baseline({"TestAssemblyGui": {sig: entry}})
                with self.assertRaisesRegex(ValueError, expected_msg):
                    checker.load_baseline(baseline)
                res = self.run_cli("--baseline", baseline, log)
                self.assertNotEqual(res.returncode, 0)
                self.assertIn("FATAL:", res.stderr)
                self.assertIn(expected_msg, res.stderr)

    def test_checked_in_baseline_is_valid(self):
        loaded = checker.load_baseline(BASELINE_PATH)
        self.assertIn("TestDraftGui", loaded)
        self.assertIn("TestArchGui", loaded)
        self.assertIn("femtest", loaded)
        self.assertNotIn("tests/live_gui/test_live_gui_review_group_branch.py", loaded)
        sep_sig = (
            "Coin error in SoGroup::removeChild(): tried to remove non-existent "
            "child 0x<ADDR> (Separator)"
        )
        sw_sig = (
            "Coin error in SoGroup::removeChild(): tried to remove non-existent "
            "child 0x<ADDR> (Switch)"
        )
        delay_sig = (
            "Coin warning in SoDelayQueueSensor::unschedule(): "
            "tried to unschedule a sensor which is not scheduled"
        )
        self.assertEqual(loaded["TestDraftGui"][sep_sig] + loaded["TestDraftGui"][sw_sig], 208)
        self.assertEqual(loaded["TestArchGui"][sep_sig] + loaded["TestArchGui"][sw_sig], 166)

        expected_objects = {
            ("AssemblyTests.TestIsolateSelectionGui", delay_sig): 35,
            ("TestAssemblyGui", delay_sig): 42,
        }
        for module, entries in loaded.items():
            for sig, entry in entries.items():
                key = (module, sig)
                if key in expected_objects:
                    self.assertIsInstance(entry, dict)
                    self.assertEqual(entry["max"], expected_objects[key])
                    self.assertIsInstance(entry.get("observed"), str)
                    self.assertIsInstance(entry.get("reason"), str)
                else:
                    self.assertIsInstance(entry, int)
                    self.assertNotIsInstance(entry, bool)


class ShellHookTests(unittest.TestCase):
    def run_e2e_hook(self, runner_output: str, runner_rc: int, baseline: dict | None = None):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            baseline_file = directory / "baseline.json"
            baseline_file.write_text(
                json.dumps({"schema_version": 1, "modules": baseline or {}}),
                encoding="utf-8",
            )
            stubs = {
                "gdb": "exit 0\n",
                "mktemp": 'printf "%s\\n" "$TEST_LOG"\n',
                "xvfb-run": (
                    'printf "%s" "$TEST_OUTPUT"\n'
                    'exit "$TEST_RC"\n'
                ),
            }
            for name, body in stubs.items():
                stub = directory / name
                stub.write_text("#!/bin/sh\n" + body, encoding="utf-8")
                stub.chmod(0o755)
            env = os.environ.copy()
            env.update(
                PATH=f"{directory}:{env['PATH']}",
                TEST_LOG=str(directory / "e2e.log"),
                TEST_OUTPUT=runner_output,
                TEST_RC=str(runner_rc),
                FREECAD_LOG_ERROR_BASELINE=str(baseline_file),
            )
            return subprocess.run(
                ["sh", str(E2E_SCRIPT)],
                cwd=directory,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )

    def test_e2e_hook_streams_and_passes_when_clean(self):
        out = (
            "Running GUI tests for module: TestPartGui\n"
            "test_a (TestPartGui.Case.test_a) ... ok\n"
            "Ran 1 test in 0.1s\n\nOK\n"
        )
        res = self.run_e2e_hook(out, runner_rc=0)
        self.assertEqual(res.returncode, 0, res.stdout + res.stderr)
        self.assertIn("Running GUI tests for module: TestPartGui", res.stdout)
        self.assertIn("log-gate: PASS", res.stdout)

    def test_e2e_hook_fails_on_new_coin_error_even_when_runner_exits_zero(self):
        out = (
            "Running GUI tests for module: TestPartGui\n"
            "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0xdead (Separator)\n"
            "Ran 1 test in 0.1s\n\nOK\n"
        )
        res = self.run_e2e_hook(out, runner_rc=0)
        self.assertEqual(res.returncode, 1)
        self.assertIn("Running GUI tests for module: TestPartGui", res.stdout)
        self.assertIn("FAIL: [TestPartGui] new error signature", res.stderr)

    def test_e2e_hook_preserves_runner_exit_code_and_still_runs_log_gate(self):
        out = (
            "Running GUI tests for module: TestPartGui\n"
            "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0xdead (Separator)\n"
        )
        res = self.run_e2e_hook(out, runner_rc=4)
        self.assertEqual(res.returncode, 4)
        self.assertIn("FAIL: [TestPartGui] new error signature", res.stderr)

        res_clean = self.run_e2e_hook("Running GUI tests for module: TestPartGui\n", runner_rc=124)
        self.assertEqual(res_clean.returncode, 124)
        self.assertIn("log-gate: PASS", res_clean.stdout)

    def run_integration_hook(self, output: str, rc: int, baseline: dict | None = None):
        with tempfile.TemporaryDirectory() as temp:
            directory = Path(temp)
            baseline_file = directory / "baseline.json"
            baseline_file.write_text(
                json.dumps({"schema_version": 1, "modules": baseline or {}}),
                encoding="utf-8",
            )
            for name, body in {
                "mktemp": 'printf "%s\\n" "$TEST_LOG"\n',
                "timeout": 'printf "%s" "$TEST_OUTPUT"\nexit "$TEST_RC"\n',
            }.items():
                stub = directory / name
                stub.write_text("#!/bin/sh\n" + body, encoding="utf-8")
                stub.chmod(0o755)
            env = os.environ.copy()
            env.update(
                PATH=f"{directory}:{env['PATH']}",
                TEST_LOG=str(directory / "integration.log"),
                TEST_OUTPUT=output,
                TEST_RC=str(rc),
                FREECAD_LOG_ERROR_BASELINE=str(baseline_file),
            )
            return subprocess.run(
                ["sh", str(INTEGRATION_SCRIPT)],
                cwd=directory,
                env=env,
                capture_output=True,
                text=True,
                check=False,
            )

    def test_integration_hook_fails_on_new_error_despite_green_test_summary(self):
        out = (
            "test_foo (ModFoo.Case.test_foo) ... "
            "Coin error in SoGroup::removeChild(): tried to remove non-existent child 0x123 (Separator)\n"
            "ok\n"
            "Ran 1 test in 0.1s\n\nOK\n"
        )
        res = self.run_integration_hook(out, rc=0)
        self.assertEqual(res.returncode, 1)
        self.assertIn("integration: executed=1", res.stdout)
        self.assertIn("FAIL: [ModFoo] new error signature", res.stderr)

    def test_integration_hook_preserves_test_failure_and_reports_log_gate(self):
        out = (
            "test_foo (ModFoo.Case.test_foo) ... "
            "QObject::startTimer: Timers cannot be started from another thread\n"
            "FAIL\n"
            "Ran 1 test in 0.1s\n\nFAILED (failures=1)\n"
        )
        res = self.run_integration_hook(out, rc=1)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn("integration process exit 1", res.stderr)
        self.assertIn("FAIL: [ModFoo] new error signature", res.stderr)


@unittest.skipUnless(CI_LOGS_DIR.is_dir(), "local CI log corpus not present")
class CiLogsCorpusTests(unittest.TestCase):
    def run_cli(self, *args) -> subprocess.CompletedProcess[str]:
        return subprocess.run(
            [sys.executable, str(CHECKER_PATH), *map(str, args)],
            capture_output=True,
            text=True,
            check=False,
        )

    def test_gated_ci_logs_pass_with_checked_in_baseline(self):
        e2e_logs = sorted(CI_LOGS_DIR.glob("*-freecad-e2e-success.log"))
        int_logs = sorted(CI_LOGS_DIR.glob("*-freecad-integration-tests-success.log"))
        self.assertGreater(len(e2e_logs), 0, f"no e2e logs found in {CI_LOGS_DIR}")
        self.assertGreater(len(int_logs), 0, f"no integration logs found in {CI_LOGS_DIR}")
        for fmt, logs in (("e2e", e2e_logs), ("integration", int_logs)):
            for log in logs:
                with self.subTest(log=log.name, fmt=fmt):
                    res = self.run_cli("--format", fmt, "--baseline", BASELINE_PATH, log)
                    self.assertEqual(
                        res.returncode,
                        0,
                        f"{log.name} failed gate:\nstdout:\n{res.stdout}\nstderr:\n{res.stderr}",
                    )
                    self.assertIn("log-gate: PASS", res.stdout)
                    self.assertNotIn("NOTE:", res.stdout)

    def test_injected_coin_error_fails_on_real_e2e_log(self):
        source = CI_LOGS_DIR / "969-freecad-e2e-success.log"
        if not source.is_file():
            self.skipTest(f"{source} not present")
        banner = "Running GUI tests for module: TestDraftGui\n"
        original = source.read_text(encoding="utf-8", errors="replace")
        self.assertIn(banner, original)
        injected = original.replace(
            banner,
            banner
            + "Coin error in SoGroup::removeChild(): tried to remove non-existent "
            + "child 0xdeadbeef (Separator)\n",
            1,
        )
        with tempfile.TemporaryDirectory() as temp:
            copy_path = Path(temp) / source.name
            copy_path.write_text(injected, encoding="utf-8")
            res = self.run_cli("--format", "e2e", "--baseline", BASELINE_PATH, copy_path)
        self.assertNotEqual(res.returncode, 0)
        self.assertIn(
            "FAIL: [TestDraftGui] count above baseline (actual=75, baseline=74):",
            res.stderr,
        )
        self.assertIn(
            "Coin error in SoGroup::removeChild(): tried to remove non-existent "
            "child 0x<ADDR> (Separator)",
            res.stderr,
        )
        self.assertIn("log-gate: FAIL", res.stderr)


if __name__ == "__main__":
    unittest.main()
