"""Tests for per-run structured logging and result metadata."""

from __future__ import annotations

import logging
import os
from pathlib import Path

import pytest
import yaml

from bridge.permissions import effective_write_policy
from bridge.run_context import begin_run, current_run, finalize_run, record_lifecycle
from runner.result import RunResult, load_result, write_result
from runner.scenario import Assertion, Scenario


@pytest.fixture(autouse=True)
def _policy_allow(monkeypatch):
    monkeypatch.setenv("BRIDGE_WRITE_POLICY", "allow")
    monkeypatch.delenv("CI", raising=False)


def test_begin_run_creates_log_and_events(tmp_path):
    ctx = begin_run("unit_test", tmp_path / "sim_runs")
    try:
        assert ctx.run_dir.is_dir()
        logging.getLogger("tests.run_logging").info("hello from test")
        record_lifecycle("test_phase", detail="ok")
        assert (ctx.run_dir / "run.log").is_file()
    finally:
        finalize_run()

    events_path = ctx.run_dir / "run_events.yaml"
    assert events_path.is_file()
    data = yaml.safe_load(events_path.read_text(encoding="utf-8"))
    assert data["run_id"] == ctx.run_id
    assert any(e.get("message") == "run_started" for e in data["events"])
    assert current_run() is None


def test_write_result_includes_metadata(tmp_path):
    ctx = begin_run("meta_test", tmp_path / "sim_runs")
    record_lifecycle("spawn_ready")
    try:
        scenario = Scenario(
            name="meta_test",
            robot="arm_2dof",
            world="empty_world",
            assertions=[Assertion(type="sim_time_under", params={"seconds": 1})],
        )
        urdf = tmp_path / "arm.urdf"
        urdf.write_text("<robot/>", encoding="utf-8")
        ctx.set_path("spawn_urdf", urdf)

        result = RunResult(scenario=scenario, status="pass", run_id=ctx.run_id)
        result.metadata = ctx.build_metadata()
        path = write_result(result, sim_runs_dir=tmp_path / "sim_runs")
    finally:
        finalize_run()

    data = load_result(path)
    assert data["metadata"]["write_policy"] == effective_write_policy()
    assert "spawn_urdf" in data["metadata"]["paths"]
    assert "spawn_urdf" in data["input_hashes"]
    assert data["metadata"]["event_count"] >= 1


def test_hash_robot_world_includes_staged_paths_when_present():
    from runner.result import _hash_robot_world

    repo = Path(__file__).resolve().parents[1]
    gen_urdf = (
        repo
        / "generated/arm_2dof/arm_2dof_description/arm_2dof_description/urdf/arm_2dof.urdf"
    )
    if not gen_urdf.is_file():
        pytest.skip("generated arm_2dof URDF not present")

    scenario = Scenario(name="h", robot="arm_2dof", world="empty_world")
    hashes = _hash_robot_world(scenario)
    assert "export_urdf" in hashes
