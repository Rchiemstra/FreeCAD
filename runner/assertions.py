"""
runner/assertions.py — assertion evaluators for Phase 4 Test Runner.

Each evaluator receives:
  - assertion : Assertion  (type + params from scenario YAML)
  - telemetry : Telemetry  (recorded data from a simulation run)

Returns an AssertionResult(passed, message, detail).

Supported assertion types (v1):
  reach_target_within   — EE pose reaches goal within N seconds
  no_self_collision     — zero inter-link contacts during run
  max_joint_torque_below — max |effort| below threshold (N·m)
  sim_time_under        — total sim time is below limit
  pose_within_tolerance — final EE pose is within XYZ tolerance of goal
  rtf_above             — average real-time factor >= threshold
  collision_count_below — total number of collision events < limit
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Any, Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from runner.scenario import Assertion, Goal


# ---------------------------------------------------------------------------
# Telemetry record — produced by the executor during a simulation run
# ---------------------------------------------------------------------------

@dataclass
class EEPoseSample:
    """One end-effector pose sample."""
    sim_time: float = 0.0
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0


@dataclass
class JointStateSample:
    """One joint-state sample."""
    sim_time: float = 0.0
    names:     list[str]   = field(default_factory=list)
    positions: list[float] = field(default_factory=list)
    efforts:   list[float] = field(default_factory=list)


@dataclass
class ContactEvent:
    sim_time:  float = 0.0
    link_a:    str   = ""
    link_b:    str   = ""


@dataclass
class Telemetry:
    """
    Recorded simulation data for a single run.

    Produced by ``executor.py`` during a live Gazebo run, or loaded from
    a ``sim_runs/<id>/telemetry.yaml`` file for post-hoc analysis.
    """
    scenario_name:    str   = ""
    sim_duration:     float = 0.0        # actual sim seconds elapsed
    avg_rtf:          float = 0.0        # average real-time factor
    ee_poses:         list[EEPoseSample]      = field(default_factory=list)
    joint_states:     list[JointStateSample]  = field(default_factory=list)
    contacts:         list[ContactEvent]      = field(default_factory=list)


# ---------------------------------------------------------------------------
# AssertionResult
# ---------------------------------------------------------------------------

@dataclass
class AssertionResult:
    assertion_type: str
    passed:         bool
    message:        str        # short human-readable summary
    detail:         str = ""   # optional extended info

    def __str__(self) -> str:
        status = "PASS" if self.passed else "FAIL"
        return f"[{status}] {self.assertion_type}: {self.message}"


# ---------------------------------------------------------------------------
# Evaluator registry
# ---------------------------------------------------------------------------

_EVALUATORS: dict[str, Any] = {}


def _register(name: str):
    def decorator(fn):
        _EVALUATORS[name] = fn
        return fn
    return decorator


def evaluate_assertion(
    assertion: "Assertion",
    telemetry: Telemetry,
    goal: Optional["Goal"] = None,
) -> AssertionResult:
    """
    Evaluate one assertion against the recorded telemetry.

    Parameters
    ----------
    assertion : Assertion
        The assertion spec from the scenario YAML.
    telemetry : Telemetry
        Recorded simulation data.
    goal : Goal | None
        The scenario goal (needed for pose-based assertions).

    Returns
    -------
    AssertionResult
    """
    fn = _EVALUATORS.get(assertion.type)
    if fn is None:
        return AssertionResult(
            assertion_type=assertion.type,
            passed=False,
            message=f"Unknown assertion type: {assertion.type!r}",
        )
    try:
        return fn(assertion, telemetry, goal)
    except Exception as exc:
        return AssertionResult(
            assertion_type=assertion.type,
            passed=False,
            message=f"Evaluator error: {exc}",
        )


def evaluate_all(
    assertions: "list[Assertion]",
    telemetry: Telemetry,
    goal: Optional["Goal"] = None,
) -> list[AssertionResult]:
    """Evaluate all assertions in order. Never raises."""
    return [evaluate_assertion(a, telemetry, goal) for a in assertions]


# ---------------------------------------------------------------------------
# Individual evaluators
# ---------------------------------------------------------------------------

@_register("reach_target_within")
def _reach_target_within(assertion, telemetry, goal):
    """EE pose is within goal.tolerance of goal.target at some time <= seconds."""
    seconds = float(assertion.params.get("seconds", 10.0))

    if goal is None or not goal.target:
        return AssertionResult(
            "reach_target_within", False,
            "No goal target defined in scenario"
        )

    tx = float(goal.target.get("x", 0.0))
    ty = float(goal.target.get("y", 0.0))
    tz = float(goal.target.get("z", 0.0))
    tol = float(goal.tolerance)

    if not telemetry.ee_poses:
        return AssertionResult(
            "reach_target_within", False,
            "No end-effector pose samples recorded"
        )

    for sample in telemetry.ee_poses:
        if sample.sim_time > seconds:
            break
        dist = math.sqrt(
            (sample.x - tx) ** 2 +
            (sample.y - ty) ** 2 +
            (sample.z - tz) ** 2
        )
        if dist <= tol:
            return AssertionResult(
                "reach_target_within", True,
                f"Reached at t={sample.sim_time:.2f}s, dist={dist:.4f}m "
                f"(tol={tol}m)"
            )

    # Find minimum distance for diagnostics
    min_dist = min(
        math.sqrt((s.x - tx)**2 + (s.y - ty)**2 + (s.z - tz)**2)
        for s in telemetry.ee_poses
    )
    return AssertionResult(
        "reach_target_within", False,
        f"Did not reach target within {seconds}s — "
        f"min dist={min_dist:.4f}m, tol={tol}m"
    )


@_register("no_self_collision")
def _no_self_collision(assertion, telemetry, goal):
    """No contact events between robot links (contacts with ground excluded)."""
    robot_contacts = [
        c for c in telemetry.contacts
        if "ground" not in c.link_a.lower() and "ground" not in c.link_b.lower()
    ]
    if robot_contacts:
        links = {f"{c.link_a}↔{c.link_b}" for c in robot_contacts}
        return AssertionResult(
            "no_self_collision", False,
            f"{len(robot_contacts)} self-collision event(s): {', '.join(sorted(links))}"
        )
    return AssertionResult(
        "no_self_collision", True,
        "No self-collisions detected"
    )


@_register("max_joint_torque_below")
def _max_joint_torque_below(assertion, telemetry, goal):
    """Maximum |effort| across all joints and all samples is below threshold."""
    threshold = float(assertion.params.get("value", 25.0))

    if not telemetry.joint_states:
        return AssertionResult(
            "max_joint_torque_below", False,
            "No joint state samples recorded"
        )

    max_effort = 0.0
    worst_joint = ""
    for sample in telemetry.joint_states:
        for name, effort in zip(sample.names, sample.efforts):
            if abs(effort) > max_effort:
                max_effort = abs(effort)
                worst_joint = name

    passed = max_effort < threshold
    return AssertionResult(
        "max_joint_torque_below", passed,
        f"max |effort|={max_effort:.3f} N·m "
        f"({'<' if passed else '>='} threshold={threshold} N·m, "
        f"joint={worst_joint!r})"
    )


@_register("sim_time_under")
def _sim_time_under(assertion, telemetry, goal):
    """Total simulated time is below the given limit."""
    limit = float(assertion.params.get("seconds", 30.0))
    passed = telemetry.sim_duration < limit
    return AssertionResult(
        "sim_time_under", passed,
        f"sim_duration={telemetry.sim_duration:.3f}s "
        f"({'<' if passed else '>='} limit={limit}s)"
    )


@_register("pose_within_tolerance")
def _pose_within_tolerance(assertion, telemetry, goal):
    """Final EE pose is within XYZ tolerance of the goal target."""
    if not telemetry.ee_poses:
        return AssertionResult(
            "pose_within_tolerance", False,
            "No end-effector pose samples recorded"
        )

    # Use goal or inline params
    if goal and goal.target:
        tx = float(goal.target.get("x", 0.0))
        ty = float(goal.target.get("y", 0.0))
        tz = float(goal.target.get("z", 0.0))
        tol = float(assertion.params.get("tolerance", goal.tolerance))
    else:
        target = assertion.params.get("target", {})
        tx  = float(target.get("x", 0.0))
        ty  = float(target.get("y", 0.0))
        tz  = float(target.get("z", 0.0))
        tol = float(assertion.params.get("tolerance", 0.05))

    final = telemetry.ee_poses[-1]
    dist  = math.sqrt((final.x - tx)**2 + (final.y - ty)**2 + (final.z - tz)**2)
    passed = dist <= tol
    return AssertionResult(
        "pose_within_tolerance", passed,
        f"Final EE dist={dist:.4f}m "
        f"({'<=' if passed else '>'} tol={tol}m)"
    )


@_register("rtf_above")
def _rtf_above(assertion, telemetry, goal):
    """Average real-time factor is at or above the threshold."""
    threshold = float(assertion.params.get("value", 0.5))
    passed    = telemetry.avg_rtf >= threshold
    return AssertionResult(
        "rtf_above", passed,
        f"avg_rtf={telemetry.avg_rtf:.3f} "
        f"({'>=' if passed else '<'} threshold={threshold})"
    )


@_register("collision_count_below")
def _collision_count_below(assertion, telemetry, goal):
    """Total number of contact events is below the given limit."""
    limit   = int(assertion.params.get("value", 0))
    count   = len(telemetry.contacts)
    passed  = count < limit if limit > 0 else count == 0
    return AssertionResult(
        "collision_count_below", passed,
        f"contact_events={count} "
        f"({'<' if passed else '>='} limit={limit})"
    )
