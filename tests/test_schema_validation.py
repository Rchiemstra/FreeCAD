"""Tests for JSON Schema validation (project, scenario, result)."""

from __future__ import annotations

from pathlib import Path

import pytest
import yaml

from bridge.project import load_project
from bridge.schema_validate import SchemaValidationError, validate_instance
from runner.scenario import load_scenario, ScenarioLoadError
from runner.result import RunResult, write_result
from runner.scenario import Scenario, Assertion


REPO = Path(__file__).resolve().parents[1]


def test_project_yaml_validates():
    load_project(REPO / "project.yaml")


def test_e2e_smoke_scenario_validates():
    load_scenario(REPO / "tests" / "scenarios_e2e" / "e2e_smoke.yaml")


def test_reach_top_shelf_scenario_validates():
    load_scenario(REPO / "tests" / "scenarios" / "reach_top_shelf.yaml")


def test_invalid_scenario_rejected(tmp_path):
    bad = tmp_path / "bad.yaml"
    bad.write_text(
        yaml.dump({"name": "x", "robot": "r"}),
        encoding="utf-8",
    )
    with pytest.raises(ScenarioLoadError):
        load_scenario(bad)


def test_result_schema_on_write(tmp_path, monkeypatch):
    monkeypatch.setenv("BRIDGE_WRITE_POLICY", "allow")
    scenario = Scenario(
        name="t",
        robot="arm_2dof",
        world="empty_world",
        assertions=[Assertion(type="sim_time_under", params={"seconds": 1})],
    )
    run = RunResult(scenario=scenario, status="pass")
    path = write_result(run, sim_runs_dir=tmp_path / "sim_runs")
    assert path.is_file()


def test_validate_instance_rejects_bad_result():
    with pytest.raises(SchemaValidationError):
        validate_instance({"status": "pass"}, "result")
