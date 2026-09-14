# SPDX-License-Identifier: LGPL-2.1-or-later
"""Regression tests for the three GUI-runner false-green cases.

These tests do not start FreeCAD. They feed canned listing/run output into
run_gui_tests.py the way pipelines 341-343, a missing-suite listing, and a
zero-test class unit would.
"""

from __future__ import annotations

import unittest

from run_gui_tests import (
    EXPECTED_GUI_SUITES,
    SKETCHER_GUI_CLASS_UNITS,
    discover_gui_suites,
    evaluate_unit_result,
    parse_completed_test_count,
    parse_registered_tests,
    run_gui_modules,
    units_for_module,
)


def _listing(*names: str) -> str:
    return "startup noise\nRegistered test units:\n" + "\n".join(names) + "\n"


def _ran(n: int, ok: bool = True) -> str:
    status = "OK" if ok else "FAILED (failures=1)"
    return f"test_foo ... {'ok' if ok else 'FAIL'}\nRan {n} tests in 0.010s\n{status}\n"


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
