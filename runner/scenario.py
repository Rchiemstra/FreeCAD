"""
runner/scenario.py — scenario YAML loader and validator.

A scenario file describes:
  - which robot and world to use
  - the initial pose
  - a goal
  - a list of assertions to evaluate after the run
  - telemetry recording settings

Example::

    from runner.scenario import load_scenario, Scenario

    s = load_scenario("tests/scenarios/reach_top_shelf.yaml")
    print(s.name, s.robot, len(s.assertions))
"""
from __future__ import annotations

import hashlib
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Optional


# ---------------------------------------------------------------------------
# Dataclasses
# ---------------------------------------------------------------------------

@dataclass
class Goal:
    type: str = "ee_pose"          # ee_pose | joint_config | none
    target: dict[str, float] = field(default_factory=dict)
    tolerance: float = 0.05        # metres (or radians for joint_config)


@dataclass
class Assertion:
    type: str = ""
    params: dict[str, Any] = field(default_factory=dict)

    def __repr__(self) -> str:
        return f"Assertion(type={self.type!r}, params={self.params})"


@dataclass
class RecordSpec:
    joint_states:        bool  = True
    end_effector_pose:   bool  = True
    contacts:            bool  = False
    rtf:                 bool  = True
    screenshot_interval: float = 0.0   # 0 = no screenshots


@dataclass
class InitialPose:
    x:   float = 0.0
    y:   float = 0.0
    z:   float = 0.0
    yaw: float = 0.0


@dataclass
class Scenario:
    name:         str          = ""
    description:  str          = ""
    robot:        str          = ""          # stem name (e.g. "arm_2dof")
    world:        str          = ""          # stem name (e.g. "empty_world")
    initial_pose: InitialPose  = field(default_factory=InitialPose)
    goal:         Goal         = field(default_factory=Goal)
    duration:     float        = 30.0       # max simulation seconds
    assertions:   list[Assertion] = field(default_factory=list)
    record:       RecordSpec   = field(default_factory=RecordSpec)
    source_path:  Optional[Path] = None     # absolute path to the YAML file
    source_hash:  str          = ""         # SHA-256 of raw YAML bytes

    def validate(self) -> list[str]:
        """Return a list of validation errors (empty = valid)."""
        errors: list[str] = []
        if not self.name:
            errors.append("name is required")
        if not self.robot:
            errors.append("robot is required")
        if self.duration <= 0:
            errors.append(f"duration must be positive, got {self.duration}")
        known_assertion_types = {
            "reach_target_within",
            "no_self_collision",
            "max_joint_torque_below",
            "sim_time_under",
            "pose_within_tolerance",
            "rtf_above",
            "collision_count_below",
        }
        for i, a in enumerate(self.assertions):
            if a.type not in known_assertion_types:
                errors.append(
                    f"assertions[{i}]: unknown type {a.type!r} "
                    f"(known: {sorted(known_assertion_types)})"
                )
        return errors


# ---------------------------------------------------------------------------
# Loader
# ---------------------------------------------------------------------------

class ScenarioLoadError(Exception):
    """Raised when a scenario file cannot be loaded or is invalid."""


def load_scenario(path: str | Path) -> Scenario:
    """
    Load a scenario YAML file and return a validated ``Scenario``.

    Parameters
    ----------
    path : str | Path
        Absolute or relative path to the scenario YAML file.

    Raises
    ------
    ScenarioLoadError
        If the file cannot be read, parsed, or fails validation.
    """
    p = Path(path).resolve()
    if not p.exists():
        raise ScenarioLoadError(f"Scenario file not found: {p}")

    try:
        raw = p.read_bytes()
    except OSError as exc:
        raise ScenarioLoadError(f"Cannot read {p}: {exc}") from exc

    try:
        import yaml  # type: ignore
        data = yaml.safe_load(raw.decode("utf-8"))
    except Exception as exc:
        raise ScenarioLoadError(f"YAML parse error in {p}: {exc}") from exc

    if not isinstance(data, dict):
        raise ScenarioLoadError(f"Scenario YAML must be a mapping, got {type(data).__name__}")

    scenario = _parse_scenario(data)
    scenario.source_path = p
    scenario.source_hash = hashlib.sha256(raw).hexdigest()

    errors = scenario.validate()
    if errors:
        raise ScenarioLoadError(
            f"Scenario {p} has {len(errors)} validation error(s):\n"
            + "\n".join(f"  - {e}" for e in errors)
        )

    return scenario


def _parse_scenario(data: dict) -> Scenario:
    """Parse a raw YAML dict into a Scenario object."""
    goal_raw = data.get("goal", {})
    goal = Goal(
        type     =goal_raw.get("type", "ee_pose"),
        target   =goal_raw.get("target", {}),
        tolerance=float(goal_raw.get("tolerance", 0.05)),
    )

    ip_raw = data.get("initial_pose", {})
    initial_pose = InitialPose(
        x  =float(ip_raw.get("x",   0.0)),
        y  =float(ip_raw.get("y",   0.0)),
        z  =float(ip_raw.get("z",   0.0)),
        yaw=float(ip_raw.get("yaw", 0.0)),
    )

    assertions: list[Assertion] = []
    for a in data.get("assertions", []):
        a_type = a.get("type", "")
        params = {k: v for k, v in a.items() if k != "type"}
        assertions.append(Assertion(type=a_type, params=params))

    rec_raw = data.get("record", {})
    record = RecordSpec(
        joint_states       =bool(rec_raw.get("joint_states",        True)),
        end_effector_pose  =bool(rec_raw.get("end_effector_pose",   True)),
        contacts           =bool(rec_raw.get("contacts",            False)),
        rtf                =bool(rec_raw.get("rtf",                 True)),
        screenshot_interval=float(rec_raw.get("screenshot_interval", 0.0)),
    )

    return Scenario(
        name        =str(data.get("name",        "")),
        description =str(data.get("description", "")),
        robot       =str(data.get("robot",       "")),
        world       =str(data.get("world",       "")),
        initial_pose=initial_pose,
        goal        =goal,
        duration    =float(data.get("duration",  30.0)),
        assertions  =assertions,
        record      =record,
    )


def list_scenario_files(scenarios_dir: str | Path) -> list[Path]:
    """Return sorted list of *.yaml files in scenarios_dir."""
    d = Path(scenarios_dir)
    if not d.exists():
        return []
    return sorted(d.glob("*.yaml"))
