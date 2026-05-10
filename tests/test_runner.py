"""
tests/test_runner.py — Offline tests for the Phase 4 Test Runner.

Tests cover:
  - scenario loading / validation
  - assertion evaluators (all 7 types)
  - result writing / loading
  - list_tests() / run_test() with mock bridge

Run with:
    pytest tests/test_runner.py -v
"""
from __future__ import annotations

import math
import os
import sys
import textwrap
import pytest

from runner.scenario import (
    load_scenario, Scenario, Assertion, Goal, RecordSpec, InitialPose,
    list_scenario_files, ScenarioLoadError,
)
from runner.assertions import (
    Telemetry, EEPoseSample, JointStateSample, ContactEvent,
    AssertionResult, evaluate_assertion, evaluate_all,
)
from runner.result import RunResult, write_result, load_result
from runner.runner import list_tests, run_test


# ===========================================================================
# Fixtures and helpers
# ===========================================================================

SCENARIO_YAML = textwrap.dedent("""\
    name: test_move
    description: "A test scenario"
    robot: arm_2dof
    world: empty_world
    initial_pose:
      x: 0.0
      y: 0.0
      z: 0.0
      yaw: 0.0
    goal:
      type: ee_pose
      target:
        x: 0.6
        y: 0.0
        z: 1.8
      tolerance: 0.05
    duration: 10.0
    assertions:
      - type: reach_target_within
        seconds: 8
      - type: no_self_collision
      - type: max_joint_torque_below
        value: 25.0
      - type: sim_time_under
        seconds: 12.0
      - type: pose_within_tolerance
      - type: rtf_above
        value: 0.5
      - type: collision_count_below
        value: 1
    record:
      joint_states: true
      end_effector_pose: true
      contacts: false
      rtf: true
      screenshot_interval: 0.0
""")


def _make_scenario_file(tmp_path, content=SCENARIO_YAML, name="test_move"):
    p = tmp_path / f"{name}.yaml"
    p.write_text(content, encoding="utf-8")
    return p


def _passing_telemetry() -> Telemetry:
    """Telemetry that should PASS all assertions in SCENARIO_YAML."""
    t = Telemetry(scenario_name="test_move", sim_duration=9.0, avg_rtf=0.98)
    # EE reaches target within 5 s
    for i in range(10):
        sim_time = i * 1.0
        # Gradually move toward (0.6, 0, 1.8)
        fraction = min(1.0, i / 5.0)
        t.ee_poses.append(EEPoseSample(
            sim_time=sim_time,
            x=0.6 * fraction,
            y=0.0,
            z=1.8 * fraction,
        ))
    # Joint states with low effort
    for i in range(10):
        t.joint_states.append(JointStateSample(
            sim_time=float(i),
            names=["joint_1", "joint_2"],
            positions=[0.1 * i, -0.05 * i],
            efforts=[2.0, 1.5],
        ))
    # No contacts
    return t


def _failing_telemetry() -> Telemetry:
    """Telemetry that should FAIL most assertions."""
    t = Telemetry(scenario_name="test_move", sim_duration=11.0, avg_rtf=0.3)
    # EE never reaches target
    for i in range(10):
        t.ee_poses.append(EEPoseSample(sim_time=float(i), x=0.0, y=0.0, z=0.0))
    # High joint effort
    t.joint_states.append(JointStateSample(
        sim_time=0.0, names=["j1"], positions=[0.0], efforts=[50.0]
    ))
    # Self-collision
    t.contacts.append(ContactEvent(sim_time=1.0, link_a="link_1", link_b="link_2"))
    return t


# ===========================================================================
# Scenario loading tests
# ===========================================================================

class TestScenarioLoading:

    def test_load_real_scenario(self):
        """Load the actual reach_top_shelf.yaml from the repo."""
        path = os.path.join(
            os.path.dirname(__file__), "..", "tests", "scenarios", "reach_top_shelf.yaml"
        )
        s = load_scenario(path)
        assert s.name == "reach_top_shelf"
        assert s.robot == "arm_2dof"
        assert s.duration == 15.0
        assert len(s.assertions) >= 4
        assert s.source_hash  # non-empty SHA-256

    def test_load_from_tmpfile(self, tmp_path):
        p = _make_scenario_file(tmp_path)
        s = load_scenario(p)
        assert s.name == "test_move"
        assert s.robot == "arm_2dof"
        assert s.goal.tolerance == pytest.approx(0.05)
        assert len(s.assertions) == 7

    def test_missing_file_raises(self, tmp_path):
        with pytest.raises(ScenarioLoadError, match="not found"):
            load_scenario(tmp_path / "nonexistent.yaml")

    def test_bad_yaml_raises(self, tmp_path):
        p = tmp_path / "bad.yaml"
        p.write_text(": : :", encoding="utf-8")
        with pytest.raises(ScenarioLoadError):
            load_scenario(p)

    def test_not_a_mapping_raises(self, tmp_path):
        p = tmp_path / "list.yaml"
        p.write_text("- item1\n- item2\n", encoding="utf-8")
        with pytest.raises(ScenarioLoadError, match="mapping"):
            load_scenario(p)

    def test_missing_name_fails_validation(self, tmp_path):
        content = SCENARIO_YAML.replace("name: test_move\n", "")
        p = _make_scenario_file(tmp_path, content=content, name="no_name")
        with pytest.raises(ScenarioLoadError, match="name is required"):
            load_scenario(p)

    def test_unknown_assertion_type_fails_validation(self, tmp_path):
        # Write a minimal scenario with an unknown assertion type directly
        content = textwrap.dedent("""\
            name: bad_assertion
            robot: arm_2dof
            world: empty_world
            duration: 10.0
            assertions:
              - type: fly_to_moon
        """)
        p = _make_scenario_file(tmp_path, content=content, name="bad_assertion")
        with pytest.raises(ScenarioLoadError, match="unknown type"):
            load_scenario(p)

    def test_list_scenario_files(self, tmp_path):
        for name in ["alpha", "beta", "gamma"]:
            (tmp_path / f"{name}.yaml").write_text("name: x\nrobot: r", encoding="utf-8")
        files = list_scenario_files(tmp_path)
        stems = [f.stem for f in files]
        assert stems == sorted(stems)
        assert "alpha" in stems

    def test_list_scenario_files_empty_dir(self, tmp_path):
        assert list_scenario_files(tmp_path) == []

    def test_list_scenario_files_missing_dir(self, tmp_path):
        assert list_scenario_files(tmp_path / "nonexistent") == []


# ===========================================================================
# Assertion evaluator tests
# ===========================================================================

class TestReachTargetWithin:

    def _a(self, seconds=8.0) -> Assertion:
        return Assertion(type="reach_target_within", params={"seconds": seconds})

    def _goal(self) -> Goal:
        return Goal(type="ee_pose", target={"x": 0.6, "y": 0.0, "z": 1.8}, tolerance=0.05)

    def test_passes_when_target_reached_in_time(self):
        telem = _passing_telemetry()
        r = evaluate_assertion(self._a(), telem, self._goal())
        assert r.passed

    def test_fails_when_target_never_reached(self):
        telem = _failing_telemetry()
        r = evaluate_assertion(self._a(), telem, self._goal())
        assert not r.passed

    def test_fails_when_no_ee_poses(self):
        telem = Telemetry()
        r = evaluate_assertion(self._a(), telem, self._goal())
        assert not r.passed

    def test_fails_when_no_goal(self):
        telem = _passing_telemetry()
        r = evaluate_assertion(self._a(), telem, goal=None)
        assert not r.passed

    def test_time_limit_respected(self):
        """Tight time limit — robot only reaches at t=5, so 3s window must fail."""
        telem = _passing_telemetry()
        r = evaluate_assertion(self._a(seconds=3.0), telem, self._goal())
        assert not r.passed


class TestNoSelfCollision:

    def _a(self) -> Assertion:
        return Assertion(type="no_self_collision", params={})

    def test_passes_with_no_contacts(self):
        telem = _passing_telemetry()
        r = evaluate_assertion(self._a(), telem, goal=None)
        assert r.passed

    def test_fails_with_self_collision(self):
        telem = _failing_telemetry()   # has link_1 ↔ link_2 contact
        r = evaluate_assertion(self._a(), telem, goal=None)
        assert not r.passed

    def test_ground_contacts_ignored(self):
        telem = Telemetry()
        telem.contacts.append(ContactEvent(0.0, "link_1", "ground_plane"))
        r = evaluate_assertion(self._a(), telem, goal=None)
        assert r.passed   # ground contacts are excluded


class TestMaxJointTorqueBelow:

    def _a(self, value=25.0) -> Assertion:
        return Assertion(type="max_joint_torque_below", params={"value": value})

    def test_passes_below_threshold(self):
        telem = _passing_telemetry()   # max effort = 2.0 N·m
        r = evaluate_assertion(self._a(25.0), telem, goal=None)
        assert r.passed

    def test_fails_above_threshold(self):
        telem = _failing_telemetry()   # max effort = 50.0 N·m
        r = evaluate_assertion(self._a(25.0), telem, goal=None)
        assert not r.passed

    def test_fails_with_no_joint_data(self):
        telem = Telemetry()
        r = evaluate_assertion(self._a(), telem, goal=None)
        assert not r.passed


class TestSimTimeUnder:

    def _a(self, seconds=12.0) -> Assertion:
        return Assertion(type="sim_time_under", params={"seconds": seconds})

    def test_passes_when_under_limit(self):
        telem = _passing_telemetry()   # sim_duration = 9.0
        r = evaluate_assertion(self._a(12.0), telem, goal=None)
        assert r.passed

    def test_fails_when_over_limit(self):
        telem = _failing_telemetry()   # sim_duration = 11.0
        r = evaluate_assertion(self._a(10.0), telem, goal=None)
        assert not r.passed


class TestPoseWithinTolerance:

    def _a(self, tol=0.05) -> Assertion:
        return Assertion(type="pose_within_tolerance", params={"tolerance": tol})

    def _goal(self) -> Goal:
        return Goal(target={"x": 0.6, "y": 0.0, "z": 1.8}, tolerance=0.05)

    def test_passes_when_final_pose_in_tolerance(self):
        telem = _passing_telemetry()   # last EE pose = (0.6, 0, 1.8) exactly
        r = evaluate_assertion(self._a(), telem, self._goal())
        assert r.passed

    def test_fails_when_far_from_target(self):
        telem = _failing_telemetry()   # last EE = (0, 0, 0)
        r = evaluate_assertion(self._a(), telem, self._goal())
        assert not r.passed


class TestRTFAbove:

    def _a(self, value=0.5) -> Assertion:
        return Assertion(type="rtf_above", params={"value": value})

    def test_passes_when_rtf_high(self):
        telem = _passing_telemetry()   # avg_rtf = 0.98
        r = evaluate_assertion(self._a(0.5), telem, goal=None)
        assert r.passed

    def test_fails_when_rtf_low(self):
        telem = _failing_telemetry()   # avg_rtf = 0.3
        r = evaluate_assertion(self._a(0.5), telem, goal=None)
        assert not r.passed


class TestCollisionCountBelow:

    def _a(self, value=1) -> Assertion:
        return Assertion(type="collision_count_below", params={"value": value})

    def test_passes_when_no_collisions(self):
        telem = _passing_telemetry()   # no contacts
        r = evaluate_assertion(self._a(1), telem, goal=None)
        assert r.passed

    def test_fails_when_collision_count_at_limit(self):
        telem = _failing_telemetry()   # 1 contact
        r = evaluate_assertion(self._a(1), telem, goal=None)
        assert not r.passed   # 1 is NOT < 1

    def test_fails_on_unknown_type(self):
        telem = Telemetry()
        r = evaluate_assertion(Assertion(type="fly_to_moon"), telem, goal=None)
        assert not r.passed
        assert "Unknown" in r.message


# ===========================================================================
# evaluate_all
# ===========================================================================

class TestEvaluateAll:

    def test_all_pass_with_passing_telemetry(self, tmp_path):
        p = _make_scenario_file(tmp_path)
        scenario = load_scenario(p)
        telem = _passing_telemetry()
        results = evaluate_all(scenario.assertions, telem, scenario.goal)
        assert len(results) == 7
        failed = [r for r in results if not r.passed]
        assert not failed, f"Unexpected failures: {[str(r) for r in failed]}"

    def test_several_fail_with_failing_telemetry(self, tmp_path):
        p = _make_scenario_file(tmp_path)
        scenario = load_scenario(p)
        telem = _failing_telemetry()
        results = evaluate_all(scenario.assertions, telem, scenario.goal)
        failed = [r for r in results if not r.passed]
        assert len(failed) >= 4   # most assertions should fail


# ===========================================================================
# RunResult and result writer
# ===========================================================================

class TestRunResult:

    def test_status_is_pass_when_all_pass(self, tmp_path):
        p = _make_scenario_file(tmp_path)
        scenario = load_scenario(p)
        telem = _passing_telemetry()
        assertion_results = evaluate_all(scenario.assertions, telem, scenario.goal)
        result = RunResult(scenario=scenario, assertion_results=assertion_results,
                           telemetry=telem)
        assert result.status == "pass"

    def test_status_is_fail_when_some_fail(self, tmp_path):
        p = _make_scenario_file(tmp_path)
        scenario = load_scenario(p)
        telem = _failing_telemetry()
        assertion_results = evaluate_all(scenario.assertions, telem, scenario.goal)
        result = RunResult(scenario=scenario, assertion_results=assertion_results,
                           telemetry=telem)
        assert result.status == "fail"

    def test_status_is_error_when_error_message_set(self, tmp_path):
        p = _make_scenario_file(tmp_path)
        scenario = load_scenario(p)
        result = RunResult(scenario=scenario, error_message="Gazebo not running")
        assert result.status == "error"

    def test_run_id_is_non_empty(self, tmp_path):
        p = _make_scenario_file(tmp_path)
        scenario = load_scenario(p)
        result = RunResult(scenario=scenario)
        assert len(result.run_id) > 0
        assert "test_move" in result.run_id

    def test_summary_contains_scenario_name(self, tmp_path):
        p = _make_scenario_file(tmp_path)
        scenario = load_scenario(p)
        result = RunResult(scenario=scenario)
        assert "test_move" in result.summary()


class TestResultWriter:

    def test_write_and_load(self, tmp_path):
        p = _make_scenario_file(tmp_path)
        scenario = load_scenario(p)
        telem = _passing_telemetry()
        ars = evaluate_all(scenario.assertions, telem, scenario.goal)
        result = RunResult(scenario=scenario, assertion_results=ars, telemetry=telem)

        result_path = write_result(result, sim_runs_dir=tmp_path / "sim_runs")
        assert result_path.exists()

        data = load_result(result_path)
        assert data["status"] == "pass"
        assert data["scenario"] == "test_move"
        assert len(data["assertions"]) == 7
        assert "input_hashes" in data
        assert "versions" in data

    def test_telemetry_yaml_written(self, tmp_path):
        p = _make_scenario_file(tmp_path)
        scenario = load_scenario(p)
        telem = _passing_telemetry()
        result = RunResult(scenario=scenario, telemetry=telem)

        write_result(result, sim_runs_dir=tmp_path / "sim_runs")
        telem_path = result.run_dir / "telemetry.yaml"
        assert telem_path.exists()

    def test_error_result_written(self, tmp_path):
        p = _make_scenario_file(tmp_path)
        scenario = load_scenario(p)
        result = RunResult(scenario=scenario, status="error",
                           error_message="Gazebo not running")
        result_path = write_result(result, sim_runs_dir=tmp_path / "sim_runs")
        data = load_result(result_path)
        assert data["status"] == "error"
        assert "Gazebo" in (data.get("error") or "")


# ===========================================================================
# run_test() with mock bridge
# ===========================================================================

class TestRunnerAPI:
    """run_test() and list_tests() with a mock bridge (no Gazebo)."""

    def _make_mock_bridge(self, final_pose=(0.6, 0.0, 1.8), rtf=0.98):
        """Mock bridge that returns pre-defined state every call."""
        from runner.assertions import JointStateSample

        class MockBridge:
            _call_count = 0

            @classmethod
            def spawn_model(cls, model_name, urdf_path, initial_pose):
                pass   # silently ok

            @classmethod
            def resume_simulation(cls):
                pass

            @classmethod
            def pause_simulation(cls):
                pass

            @classmethod
            def get_model_state(cls, model_name):
                cls._call_count += 1
                # Simulate reaching the target on call 6+ (sim_time ≈ 6s)
                t = float(cls._call_count) * 0.5
                fraction = min(1.0, t / 5.0)
                return {
                    "sim_time": t,
                    "rtf": rtf,
                    "end_effector": {
                        "position": {
                            "x": final_pose[0] * fraction,
                            "y": final_pose[1],
                            "z": final_pose[2] * fraction,
                        }
                    },
                    "joint_states": [
                        {"name": "j1", "position": 0.1 * t, "effort": 2.0},
                        {"name": "j2", "position": -0.05 * t, "effort": 1.5},
                    ],
                }

        return MockBridge()

    def test_list_tests_returns_scenario_names(self):
        scenarios_dir = os.path.join(
            os.path.dirname(__file__), "..", "tests", "scenarios"
        )
        names = list_tests(scenarios_dir)
        assert "reach_top_shelf" in names

    def test_run_test_with_mock_bridge_passes(self, tmp_path):
        """Full pipeline with a mock bridge — should PASS reach_top_shelf."""
        p = _make_scenario_file(tmp_path, name="mini_test")
        bridge = self._make_mock_bridge()
        result = run_test(
            "mini_test",
            scenarios_dir=tmp_path,
            sim_runs_dir=tmp_path / "runs",
            bridge_module=bridge,
            poll_interval=0.0,   # no real sleep in tests
        )
        assert result.status == "pass", f"Expected pass; got: {result.summary()}\n" + \
            "\n".join(str(r) for r in result.assertion_results)

    def test_run_test_missing_scenario_gives_error(self, tmp_path):
        result = run_test(
            "does_not_exist",
            scenarios_dir=tmp_path,
            sim_runs_dir=tmp_path / "runs",
        )
        assert result.status == "error"
        assert "not found" in result.error_message.lower() or \
               "Scenario" in result.error_message

    def test_run_test_writes_result_yaml(self, tmp_path):
        p = _make_scenario_file(tmp_path, name="write_test")
        bridge = self._make_mock_bridge()
        result = run_test(
            "write_test",
            scenarios_dir=tmp_path,
            sim_runs_dir=tmp_path / "runs",
            bridge_module=bridge,
            poll_interval=0.0,
        )
        assert result.run_dir is not None
        assert (result.run_dir / "result.yaml").exists()
