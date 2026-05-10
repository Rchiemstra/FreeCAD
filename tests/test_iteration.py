"""
tests/test_iteration.py — offline unit tests for Phase 5: iteration loops.

All tests run without FreeCAD, Gazebo, or network access.  Live paths are
monkeypatched or replaced with in-process mocks.

Coverage targets:
  - policy.py:    ParameterRule, Policy, DEFAULT_ARM_2DOF_POLICY
  - parameter.py: _build_get_code, _build_set_code (code string tests)
  - loop.py:      IterationLoop (with mock bridge and mock runner)
  - sweep.py:     linspace, arange, SweepRunner
  - report.py:    compare_results, summarize_failure, diff_results
"""
from __future__ import annotations

import sys
import types
from pathlib import Path
from unittest.mock import MagicMock, patch

import pytest

# Ensure repo root is on sys.path
repo_root = Path(__file__).parent.parent
if str(repo_root) not in sys.path:
    sys.path.insert(0, str(repo_root))


# ============================================================================
# Shared fixtures / helpers
# ============================================================================

def _make_assertion_result(atype: str, passed: bool, msg: str = "") -> "AssertionResult":
    from runner.assertions import AssertionResult
    return AssertionResult(assertion_type=atype, passed=passed, message=msg)


def _make_run_result(run_id: str, status: str, assertions: list) -> "RunResult":
    """Build a minimal RunResult without touching the file system."""
    from runner.result import RunResult
    from runner.scenario import Scenario, Goal
    scenario = Scenario(
        name="test_scenario",
        robot="arm_2dof",
        world="empty",
        goal=Goal(type="ee_pose", target={"x": 0, "y": 0, "z": 1}),
    )
    return RunResult(
        run_id           =run_id,
        scenario         =scenario,
        status           =status,
        assertion_results=assertions,
        error_message    ="" if status != "error" else "simulated error",
    )


# ============================================================================
# Policy tests
# ============================================================================

class TestParameterRule:
    def test_basic_construction(self):
        from iteration.policy import ParameterRule
        rule = ParameterRule(name="link1_length", min_val=0.1, max_val=0.9, step=0.05)
        assert rule.name == "link1_length"
        assert rule.min_val == 0.1
        assert rule.max_val == 0.9
        assert rule.step == 0.05

    def test_default_unit(self):
        from iteration.policy import ParameterRule
        rule = ParameterRule(name="test", min_val=0, max_val=1, step=0.1)
        assert rule.unit == ""   # default is empty

    def test_check_in_range(self):
        from iteration.policy import ParameterRule
        rule = ParameterRule(name="x", min_val=0.0, max_val=1.0, step=0.1)
        ok, msg = rule.check(0.5)
        assert ok
        assert msg == ""

    def test_check_at_min(self):
        from iteration.policy import ParameterRule
        rule = ParameterRule(name="x", min_val=0.2, max_val=1.0, step=0.1)
        ok, msg = rule.check(0.2)
        assert ok

    def test_check_at_max(self):
        from iteration.policy import ParameterRule
        rule = ParameterRule(name="x", min_val=0.2, max_val=1.0, step=0.1)
        ok, msg = rule.check(1.0)
        assert ok

    def test_check_below_min(self):
        from iteration.policy import ParameterRule
        rule = ParameterRule(name="x", min_val=0.2, max_val=1.0, step=0.1)
        ok, msg = rule.check(0.1)
        assert not ok
        assert "0.1" in msg or "x" in msg

    def test_check_above_max(self):
        from iteration.policy import ParameterRule
        rule = ParameterRule(name="x", min_val=0.2, max_val=1.0, step=0.1)
        ok, msg = rule.check(1.5)
        assert not ok

    def test_clamp_below(self):
        from iteration.policy import ParameterRule
        rule = ParameterRule(name="x", min_val=0.2, max_val=1.0, step=0.1)
        assert rule.clamp(0.0) == pytest.approx(0.2)

    def test_clamp_above(self):
        from iteration.policy import ParameterRule
        rule = ParameterRule(name="x", min_val=0.2, max_val=1.0, step=0.1)
        assert rule.clamp(2.0) == pytest.approx(1.0)

    def test_clamp_in_range(self):
        from iteration.policy import ParameterRule
        rule = ParameterRule(name="x", min_val=0.2, max_val=1.0, step=0.1)
        assert rule.clamp(0.5) == pytest.approx(0.5)

    def test_snap_rounds_to_step(self):
        from iteration.policy import ParameterRule
        rule = ParameterRule(name="x", min_val=0.0, max_val=1.0, step=0.1)
        assert rule.snap(0.23) == pytest.approx(0.2, abs=1e-9)

    def test_snap_rounds_up(self):
        from iteration.policy import ParameterRule
        rule = ParameterRule(name="x", min_val=0.0, max_val=1.0, step=0.1)
        assert rule.snap(0.26) == pytest.approx(0.3, abs=1e-9)


class TestPolicy:
    def _make_policy(self):
        from iteration.policy import Policy, ParameterRule
        return Policy(rules=[
            ParameterRule("link1_length", 0.2, 0.8, 0.05),
            ParameterRule("link2_length", 0.1, 0.6, 0.05),
        ])

    def test_check_valid(self):
        p = self._make_policy()
        ok, errors = p.check_all({"link1_length": 0.5, "link2_length": 0.3})
        assert ok
        assert errors == []

    def test_check_invalid(self):
        p = self._make_policy()
        ok, errors = p.check_all({"link1_length": 1.5})
        assert not ok
        assert len(errors) == 1

    def test_check_unknown_param_disallowed(self):
        from iteration.policy import Policy, ParameterRule
        p = Policy(rules=[ParameterRule("link1_length", 0.2, 0.8, 0.05)], allow_unknown=False)
        ok, errors = p.check_all({"unknown_param": 0.5})
        assert not ok

    def test_check_unknown_param_allowed(self):
        from iteration.policy import Policy, ParameterRule
        p = Policy(rules=[ParameterRule("link1_length", 0.2, 0.8, 0.05)], allow_unknown=True)
        ok, errors = p.check_all({"unknown_param": 0.5})
        assert ok

    def test_clamp_returns_bounded_dict(self):
        p = self._make_policy()
        clamped = p.clamp_all({"link1_length": 99.9, "link2_length": -1.0})
        assert clamped["link1_length"] == pytest.approx(0.8)
        assert clamped["link2_length"] == pytest.approx(0.1)

    def test_snap_returns_snapped_dict(self):
        p = self._make_policy()
        snapped = p.snap_all({"link1_length": 0.53})
        assert snapped["link1_length"] == pytest.approx(0.55, abs=1e-9)


class TestDefaultPolicy:
    def test_default_policy_exists(self):
        from iteration.policy import DEFAULT_ARM_2DOF_POLICY
        assert DEFAULT_ARM_2DOF_POLICY is not None

    def test_default_policy_has_rules(self):
        from iteration.policy import DEFAULT_ARM_2DOF_POLICY
        assert len(DEFAULT_ARM_2DOF_POLICY.rules) >= 2

    def test_default_policy_link1_in_range(self):
        from iteration.policy import DEFAULT_ARM_2DOF_POLICY
        ok, _ = DEFAULT_ARM_2DOF_POLICY.check_all({"link1_length": 0.5})
        assert ok


# ============================================================================
# Parameter code-generation tests (no live FreeCAD needed)
# ============================================================================

class TestParameterCodeGen:
    def test_get_spreadsheet_alias(self):
        from iteration.parameter import _build_get_code
        code = _build_get_code("link1_length", "")
        assert "Spreadsheet::Sheet" in code
        assert "link1_length" in code

    def test_get_dot_notation(self):
        from iteration.parameter import _build_get_code
        code = _build_get_code("Box.Length", "")
        assert "getObject" in code
        assert "Box" in code
        assert "Length" in code

    def test_get_named_doc(self):
        from iteration.parameter import _build_get_code
        code = _build_get_code("link1_length", "MyDoc")
        assert "getDocument" in code
        assert "MyDoc" in code

    def test_set_spreadsheet_alias(self):
        from iteration.parameter import _build_set_code
        code = _build_set_code("link1_length", 0.5, "", True)
        assert "Spreadsheet::Sheet" in code
        assert "0.5" in code
        assert "recompute" in code

    def test_set_dot_notation(self):
        from iteration.parameter import _build_set_code
        code = _build_set_code("Box.Length", 100.0, "", True)
        assert "getObject" in code
        assert "Box" in code
        assert "100.0" in code

    def test_set_no_recompute(self):
        from iteration.parameter import _build_set_code
        code = _build_set_code("link1_length", 0.5, "", recompute=False)
        assert "recompute" not in code

    def test_connection_error_on_rpc_failure(self):
        """_rpc_execute should raise ConnectionError when server is not running."""
        from iteration.parameter import _rpc_execute
        with pytest.raises(ConnectionError, match="not reachable"):
            _rpc_execute("print(1)", "http://localhost:19999")


# ============================================================================
# SweepRunner helpers
# ============================================================================

class TestLinspace:
    def test_five_values(self):
        from iteration.sweep import linspace
        vals = linspace(0.0, 1.0, 5)
        assert len(vals) == 5
        assert vals[0] == pytest.approx(0.0)
        assert vals[-1] == pytest.approx(1.0)

    def test_one_value(self):
        from iteration.sweep import linspace
        vals = linspace(0.3, 0.7, 1)
        assert vals == [0.3]

    def test_zero_count(self):
        from iteration.sweep import linspace
        vals = linspace(0.0, 1.0, 0)
        assert vals == []


class TestArange:
    def test_basic(self):
        from iteration.sweep import arange
        vals = arange(0.0, 0.5, 0.1)
        assert len(vals) == 5
        assert vals[0] == pytest.approx(0.0)

    def test_negative_step(self):
        from iteration.sweep import arange
        vals = arange(1.0, 0.0, -0.25)
        assert len(vals) == 4
        assert vals[0] == pytest.approx(1.0)
        assert vals[-1] == pytest.approx(0.25)

    def test_zero_step_raises(self):
        from iteration.sweep import arange
        with pytest.raises(ValueError):
            arange(0.0, 1.0, 0.0)


# ============================================================================
# Report tests
# ============================================================================

class TestCompareResults:
    def _results(self):
        ar1 = [
            _make_assertion_result("reach_target_within", True),
            _make_assertion_result("rtf_above", False, "rtf=0.4 < 0.5"),
        ]
        ar2 = [
            _make_assertion_result("reach_target_within", False, "did not reach"),
            _make_assertion_result("rtf_above", True),
        ]
        return [
            _make_run_result("run-A", "fail", ar1),
            _make_run_result("run-B", "fail", ar2),
        ]

    def test_compare_returns_report(self):
        from iteration.report import compare_results
        r = compare_results(self._results())
        assert r.text != ""
        assert "run-A" in r.text or "✓" in r.text

    def test_compare_identifies_best(self):
        from iteration.report import compare_results
        r = compare_results(self._results())
        # Both have 1 pass — best is the first one encountered
        assert r.best_run_id != ""

    def test_compare_empty(self):
        from iteration.report import compare_results
        r = compare_results([])
        assert "no results" in r.text.lower()

    def test_compare_single(self):
        from iteration.report import compare_results
        results = [_make_run_result("solo", "pass", [
            _make_assertion_result("reach_target_within", True),
        ])]
        r = compare_results(results)
        assert r.best_run_id == "solo"
        assert r.worst_run_id == "solo"


class TestSummarizeFailure:
    def test_pass_message(self):
        from iteration.report import summarize_failure
        r = _make_run_result("x", "pass", [
            _make_assertion_result("reach_target_within", True),
        ])
        msg = summarize_failure(r)
        assert "PASS" in msg

    def test_fail_message(self):
        from iteration.report import summarize_failure
        r = _make_run_result("x", "fail", [
            _make_assertion_result("reach_target_within", False, "too far"),
        ])
        msg = summarize_failure(r)
        assert "FAIL" in msg
        assert "reach_target_within" in msg

    def test_error_message(self):
        from iteration.report import summarize_failure
        r = _make_run_result("x", "error", [])
        msg = summarize_failure(r)
        assert "ERROR" in msg

    def test_suggestions_for_reach(self):
        from iteration.report import summarize_failure
        r = _make_run_result("x", "fail", [
            _make_assertion_result("reach_target_within", False),
        ])
        msg = summarize_failure(r)
        assert "link" in msg.lower() or "reach" in msg.lower()


class TestDiffResults:
    def test_newly_passing(self):
        from iteration.report import diff_results
        a = _make_run_result("A", "fail", [_make_assertion_result("rtf_above", False)])
        b = _make_run_result("B", "pass", [_make_assertion_result("rtf_above", True)])
        d = diff_results(a, b)
        assert "rtf_above" in d.newly_passing
        assert d.improved()

    def test_newly_failing(self):
        from iteration.report import diff_results
        a = _make_run_result("A", "pass", [_make_assertion_result("rtf_above", True)])
        b = _make_run_result("B", "fail", [_make_assertion_result("rtf_above", False)])
        d = diff_results(a, b)
        assert "rtf_above" in d.newly_failing
        assert not d.improved()

    def test_unchanged(self):
        from iteration.report import diff_results
        a = _make_run_result("A", "pass", [_make_assertion_result("rtf_above", True)])
        b = _make_run_result("B", "pass", [_make_assertion_result("rtf_above", True)])
        d = diff_results(a, b)
        assert d.newly_passing == []
        assert d.newly_failing == []
        assert "rtf_above" in d.unchanged_pass


# ============================================================================
# IterationLoop (offline — mock bridge + mock run_test)
# ============================================================================

def _make_mock_run_result(status: str = "pass") -> "RunResult":
    from runner.assertions import AssertionResult
    return _make_run_result(
        run_id     ="mock-run-001",
        status     =status,
        assertions =[AssertionResult("reach_target_within", status == "pass", "")],
    )


class TestIterationLoop:
    def _make_loop(self, run_status: str = "pass"):
        from iteration.loop import IterationLoop
        from iteration.policy import DEFAULT_ARM_2DOF_POLICY

        mock_bridge = MagicMock()
        loop = IterationLoop(
            scenario_name="reach_top_shelf",
            policy       =DEFAULT_ARM_2DOF_POLICY,
            bridge_module=mock_bridge,
        )
        return loop

    def _patch_externals(self, run_status: str = "pass"):
        """Context manager that patches set_parameter, export_urdf, run_test."""
        mock_result = _make_mock_run_result(run_status)
        patchers = [
            patch("iteration.loop.IterationLoop._set_params", return_value=""),
            patch("iteration.loop.IterationLoop._export_urdf", return_value=""),
            patch("runner.runner.run_test", return_value=mock_result),
        ]
        return patchers

    def test_policy_violation_returns_error(self):
        from iteration.loop import IterationLoop
        from iteration.policy import DEFAULT_ARM_2DOF_POLICY

        # Give a value way outside policy bounds
        loop = IterationLoop("s", DEFAULT_ARM_2DOF_POLICY)
        result = loop.run_once({"link1_length": 999.0})
        assert result.error != ""
        assert result.run_result is None

    def test_run_once_success(self):
        loop = self._make_loop()
        patchers = self._patch_externals("pass")
        mocks = [p.start() for p in patchers]
        try:
            r = loop.run_once({"link1_length": 0.5})
            assert r.run_result is not None
            assert r.error == ""
            assert r.passed()
        finally:
            for p in patchers:
                p.stop()

    def test_run_once_failure_propagated(self):
        loop = self._make_loop()
        patchers = self._patch_externals("fail")
        for p in patchers:
            p.start()
        try:
            r = loop.run_once({"link1_length": 0.5})
            assert not r.passed()
        finally:
            for p in patchers:
                p.stop()

    def test_second_run_has_diff(self):
        loop = self._make_loop()
        patchers = self._patch_externals("pass")
        for p in patchers:
            p.start()
        try:
            r1 = loop.run_once({"link1_length": 0.4})
            r2 = loop.run_once({"link1_length": 0.5})
            assert r1.diff is None          # first run has no baseline
            assert r2.diff is not None      # second run diffs against first
        finally:
            for p in patchers:
                p.stop()

    def test_history_accumulates(self):
        loop = self._make_loop()
        patchers = self._patch_externals()
        for p in patchers:
            p.start()
        try:
            loop.run_once({"link1_length": 0.3})
            loop.run_once({"link1_length": 0.4})
            assert len(loop.history) == 2
        finally:
            for p in patchers:
                p.stop()

    def test_set_param_connection_error(self):
        """If FreeCAD is not running, run_once returns an error result."""
        from iteration.loop import IterationLoop
        from iteration.policy import DEFAULT_ARM_2DOF_POLICY

        loop = IterationLoop("s", DEFAULT_ARM_2DOF_POLICY)
        with patch(
            "iteration.loop.IterationLoop._set_params",
            side_effect=lambda p: "FreeCAD not reachable"
        ):
            with patch("iteration.loop.IterationLoop._export_urdf", return_value=""):
                # _set_params now returns the error string directly
                with patch(
                    "iteration.parameter.set_parameter",
                    side_effect=ConnectionError("not reachable"),
                ):
                    r = loop.run_once({"link1_length": 0.5})
                    # _set_params returns non-empty string → error result
                    assert r.error != ""


class TestSweepRunner:
    def _make_runner(self):
        from iteration.sweep import SweepRunner
        from iteration.policy import DEFAULT_ARM_2DOF_POLICY
        return SweepRunner("reach_top_shelf", DEFAULT_ARM_2DOF_POLICY)

    def test_linspace_calls_run_for_each_value(self):
        runner = self._make_runner()
        mock_result = _make_mock_run_result("pass")
        with patch("iteration.loop.IterationLoop.run_once", return_value=mock_result) as m:
            runner.linspace("link1_length", 0.3, 0.7, 5)
            assert m.call_count == 5

    def test_arange_calls_run_for_each_value(self):
        runner = self._make_runner()
        mock_result = _make_mock_run_result("pass")
        with patch("iteration.loop.IterationLoop.run_once", return_value=mock_result) as m:
            runner.arange("link1_length", 0.3, 0.6, 0.1)
            assert m.call_count == 3   # 0.3, 0.4, 0.5

    def test_results_accumulated(self):
        runner = self._make_runner()
        mock_result = _make_mock_run_result("pass")
        with patch("iteration.loop.IterationLoop.run_once", return_value=mock_result):
            runner.sweep("link1_length", [0.3, 0.5])
            assert len(runner.results) == 2

    def test_compare_delegates_to_loop(self):
        runner = self._make_runner()
        mock_run_result = _make_mock_run_result("pass")
        from iteration.loop import IterationResult
        mock_iter_result = IterationResult(params={"link1_length": 0.3}, run_result=mock_run_result)
        with patch("iteration.loop.IterationLoop.run_once", return_value=mock_iter_result):
            runner.sweep("link1_length", [0.3])
        report = runner.compare()
        assert hasattr(report, "text")
