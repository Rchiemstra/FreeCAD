"""
runner/executor.py — scenario executor.

Runs a scenario against a live or mocked Gazebo simulation:
  1. Export robot + world (via bridge.handoff)
  2. Spawn in Gazebo and start clock
  3. Poll state for scenario.duration seconds, recording telemetry
  4. Stop simulation
  5. Return (Telemetry, status)

When Gazebo is not running the executor fails with a descriptive error and
returns an empty Telemetry — the assertion evaluator handles it gracefully.
"""
from __future__ import annotations

import time
import logging
import os
from dataclasses import dataclass, field
from typing import Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from runner.scenario import Scenario

from runner.assertions import (
    Telemetry, EEPoseSample, JointStateSample, ContactEvent,
)

log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Executor
# ---------------------------------------------------------------------------

class ScenarioExecutor:
    """
    Runs one scenario and records telemetry.

    Parameters
    ----------
    scenario : Scenario
    bridge_module : module | None
        Inject a mock bridge for testing (default: bridge.gazebo_bridge).
    poll_interval : float
        Seconds between Gazebo state polls during the run. Default 0.1 s.
    """

    def __init__(
        self,
        scenario: "Scenario",
        bridge_module=None,
        poll_interval: float = 0.1,
    ):
        self._scenario      = scenario
        self._bridge        = bridge_module
        self._poll_interval = poll_interval

    def run(self) -> tuple[Telemetry, str]:
        """
        Execute the scenario and return (Telemetry, status_message).

        Returns
        -------
        telemetry : Telemetry
            Recorded data (may be empty if execution failed early).
        status : str
            "ok" or an error description.
        """
        scenario = self._scenario
        bridge   = self._bridge or self._load_bridge()
        telemetry = Telemetry(scenario_name=scenario.name)

        if bridge is None:
            return telemetry, "Gazebo bridge not available (Gazebo not running?)"

        # --- Step 1: spawn model ---
        try:
            log.info("[Executor] Spawning %s in world %s", scenario.robot, scenario.world)
            bridge.spawn_model(
                model_name =scenario.robot,
                urdf_path  =self._urdf_path(),
                initial_pose=vars(scenario.initial_pose),
            )
        except Exception as exc:
            return telemetry, f"spawn_model failed: {exc}"

        # --- Step 2: resume / start clock ---
        try:
            bridge.resume_simulation()
        except Exception as exc:
            log.debug("resume_simulation: %s (may be ok if already running)", exc)

        # --- Step 3: poll for scenario.duration ---
        start_wall = time.monotonic()
        sim_times: list[float] = []
        rtf_samples: list[float] = []

        while True:
            elapsed_wall = time.monotonic() - start_wall
            try:
                raw = bridge.get_model_state(scenario.robot)
            except Exception as exc:
                log.debug("get_model_state: %s", exc)
                raw = {}

            sim_time = raw.get("sim_time", elapsed_wall)
            sim_times.append(sim_time)

            if scenario.record.end_effector_pose:
                ee = raw.get("end_effector", {})
                pos = ee.get("position", {})
                telemetry.ee_poses.append(EEPoseSample(
                    sim_time=sim_time,
                    x=float(pos.get("x", 0.0)),
                    y=float(pos.get("y", 0.0)),
                    z=float(pos.get("z", 0.0)),
                ))

            if scenario.record.joint_states:
                js_list = raw.get("joint_states", [])
                names     = [j.get("name", "")     for j in js_list]
                positions = [j.get("position", 0.0) for j in js_list]
                efforts   = [j.get("effort", 0.0)   for j in js_list]
                if names:
                    telemetry.joint_states.append(JointStateSample(
                        sim_time=sim_time,
                        names=names,
                        positions=positions,
                        efforts=efforts,
                    ))

            if scenario.record.contacts:
                for c in raw.get("contacts", []):
                    telemetry.contacts.append(ContactEvent(
                        sim_time=sim_time,
                        link_a=c.get("link_a", ""),
                        link_b=c.get("link_b", ""),
                    ))

            rtf = raw.get("rtf", 0.0)
            if rtf > 0:
                rtf_samples.append(rtf)

            if sim_time >= scenario.duration:
                break

            time.sleep(self._poll_interval)

        # --- Step 4: pause simulation ---
        try:
            bridge.pause_simulation()
        except Exception as exc:
            log.debug("pause_simulation: %s", exc)

        # --- Finalise telemetry ---
        telemetry.sim_duration = sim_times[-1] if sim_times else 0.0
        telemetry.avg_rtf = (
            sum(rtf_samples) / len(rtf_samples) if rtf_samples else 0.0
        )

        return telemetry, "ok"

    # ------------------------------------------------------------------
    # Helpers
    # ------------------------------------------------------------------

    def _urdf_path(self) -> str:
        """Resolve the URDF path for the scenario's robot."""
        from pathlib import Path

        robot = self._scenario.robot
        try:
            from bridge.project import load_project

            cfg = load_project()
            gen = cfg.paths.generated / robot / f"{robot}.urdf"
            if gen.is_file():
                return str(gen)
            return str(cfg.paths.robots / f"{robot}.urdf")
        except Exception:
            gen = Path("generated") / robot / f"{robot}.urdf"
            if gen.is_file():
                return str(gen)
            return f"robots/{robot}.urdf"

    @staticmethod
    def _load_bridge():
        env = os.environ.get("E2E_BRIDGE_MODULE", "").strip().lower()
        if env in ("gz_cli", "1", "yes", "true"):
            try:
                from bridge import gz_cli_bridge
                return gz_cli_bridge
            except Exception as exc:
                log.debug("Cannot load bridge.gz_cli_bridge: %s", exc)
                return None
        return None
