# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regression tests for the three GUI-runner false-green cases.

These tests do not start FreeCAD. They feed canned listing/run output into
run_gui_tests.py the way pipelines 341-343, a missing-suite listing, and a
zero-test class unit would.
"""

from __future__ import annotations

import contextlib
import io
import unittest

from run_gui_tests import (
    EXPECTED_GUI_SUITES,
    SKETCHER_GUI_CLASS_UNITS,
    discover_gui_suites,
    evaluate_unit_result,
    parse_completed_test_count,
    parse_final_outcome,
    parse_registered_tests,
    run_gui_modules,
    units_for_module,
)


def _listing(*names: str) -> str:
    return "startup noise\nRegistered test units:\n" + "\n".join(names) + "\n"


def _ran(n: int, ok: bool = True) -> str:
    status = "OK" if ok else "FAILED (failures=1)"
    return f"test_foo ... {'ok' if ok else 'FAIL'}\nRan {n} tests in 0.010s\n{status}\n"


def _unittest_output(ran: int, outcome: str | None) -> str:
    body = "test_a (Mod.TestA) ... ok\n" + "-" * 70 + f"\nRan {ran} tests in 0.412s\n\n"
    return body + (outcome + "\n" if outcome is not None else "")


def _quiet_run_gui_modules(run) -> int:
    with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
        return run_gui_modules("FreeCAD", run=run, crash_helper=False)


def _fake_freecad(per_unit: dict[str, tuple[int, str]], default=(0, _unittest_output(3, "OK"))):
    seen: list[str] = []

    def run(cmd: list[str]) -> tuple[int, str]:
        if cmd[-1] == "-t":
            return 0, _listing(*FULL_SUITES)
        seen.append(cmd[-1])
        return per_unit.get(cmd[-1], default)

    return run, seen


FULL_SUITES = EXPECTED_GUI_SUITES


class ParseTests(unittest.TestCase):
    def test_parse_registered_tests_skips_preamble(self):
        names = parse_registered_tests(_listing("GuiDocument", "TestPartGui"))
        self.assertEqual(names, ["GuiDocument", "TestPartGui"])

    def test_parse_completed_test_count_uses_last_summary(self):
        output = "Ran 2 tests in 0.1s\nOK\nRan 12 tests in 4.6s\nOK\n"
        self.assertEqual(parse_completed_test_count(output), 12)

    def test_parse_completed_test_count_missing(self):
        self.assertIsNone(parse_completed_test_count("System exit\n"))


class DiscoveryFalseGreenTests(unittest.TestCase):
    def test_failed_listing_is_preserved(self):
        rc, suites, err = discover_gui_suites(1, _listing(*FULL_SUITES))
        self.assertEqual(rc, 1)
        self.assertEqual(suites, [])
        self.assertIn("discovery failed", err.lower())

    def test_empty_list_after_clean_listing_is_failure(self):
        # Pipelines 341-343: ~7s, no modules, exit 0.
        rc, suites, err = discover_gui_suites(0, "FreeCAD 26.3.0\nSystem exit\n")
        self.assertEqual(rc, 2)
        self.assertEqual(suites, [])
        self.assertIn("Registered test units", err)

    def test_header_without_units_is_failure(self):
        rc, _, err = discover_gui_suites(0, "Registered test units:\n\n")
        self.assertEqual(rc, 2)
        self.assertIn("no registered units", err.lower())

    def test_missing_expected_suite_is_failure(self):
        names = [n for n in FULL_SUITES if n != "TestSketcherGui"]
        rc, suites, err = discover_gui_suites(0, _listing(*names))
        self.assertEqual(rc, 3)
        self.assertNotIn("TestSketcherGui", suites)
        self.assertIn("TestSketcherGui", err)

    def test_full_expected_set_discovers(self):
        rc, suites, err = discover_gui_suites(0, _listing(*FULL_SUITES, "TestExtraGui"))
        self.assertEqual(rc, 0)
        self.assertEqual(err, "")
        self.assertIn("TestExtraGui", suites)
        for name in FULL_SUITES:
            self.assertIn(name, suites)


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


class EndToEndFakeFreeCAD(unittest.TestCase):
    def test_empty_discovery_does_not_return_zero(self):
        def run(cmd):
            self.assertEqual(cmd[-1], "-t")
            return 0, "FreeCAD ready\n"

        self.assertEqual(run_gui_modules("FreeCAD", run=run, crash_helper=False), 2)

    def test_missing_suite_does_not_return_zero(self):
        def run(cmd):
            return 0, _listing("GuiDocument")

        self.assertEqual(run_gui_modules("FreeCAD", run=run, crash_helper=False), 3)

    def test_failed_discovery_exit_is_preserved(self):
        def run(cmd):
            return 17, "boom\n"

        self.assertEqual(run_gui_modules("FreeCAD", run=run, crash_helper=False), 17)

    def test_aggregate_sketcher_runs_before_class_units(self):
        seen: list[str] = []

        def run(cmd):
            if cmd[-1] == "-t":
                return 0, _listing(*FULL_SUITES)
            seen.append(cmd[-1])
            return 0, _ran(5)

        rc = run_gui_modules("FreeCAD", run=run, crash_helper=False)
        self.assertEqual(rc, 0)
        sketcher = [name for name in seen if name == "TestSketcherGui" or name.startswith("SketcherTests.")]
        self.assertEqual(sketcher[0], "TestSketcherGui")
        self.assertEqual(tuple(sketcher[1:]), SKETCHER_GUI_CLASS_UNITS)

    def test_zero_test_module_fails_the_gate(self):
        def run(cmd):
            if cmd[-1] == "-t":
                return 0, _listing(*FULL_SUITES)
            if cmd[-1] == "GuiDocument":
                return 0, "Ran 0 tests in 0.0s\nOK\n"
            return 0, _ran(1)

        self.assertEqual(run_gui_modules("FreeCAD", run=run, crash_helper=False), 4)


if __name__ == "__main__":
    unittest.main()
