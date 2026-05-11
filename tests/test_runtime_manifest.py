# SPDX-License-Identifier: LGPL-2.1-or-later

"""Validate config/runtime_manifest.yaml structure and CI pins."""

from __future__ import annotations

import tomllib
from pathlib import Path

import pytest
import yaml

REPO_ROOT = Path(__file__).resolve().parents[1]
MANIFEST = REPO_ROOT / "config" / "runtime_manifest.yaml"


def _load() -> dict:
    assert MANIFEST.is_file(), f"missing {MANIFEST}"
    data = yaml.safe_load(MANIFEST.read_text(encoding="utf-8"))
    assert isinstance(data, dict)
    return data


def test_runtime_manifest_exists_and_schema():
    data = _load()
    assert data.get("schema_version") == 1
    ci = data["canonical_ci"]
    assert ci["ros2"]["distro"] == "jazzy"
    assert ci["gazebo"]["family"] == "harmonic"
    assert ci["python"]["pytest_image"].startswith("3.12")
    assert ci["docker"]["e2e"]["base_image"] == "ros:jazzy-ros-base-noble"
    assert ci["docker"]["pytest"]["base_image"] == "python:3.12-slim-bookworm"
    env = data["environment_variables"]
    assert "GAZEBO_MCP_SENSOR_MODE" in env
    assert env["GAZEBO_MCP_SENSOR_MODE"]["default"] == "auto"
    assert "BRIDGE_MCP_DENY_MUTATING" in env
    assert "BRIDGE_STRUCTLOG_PATH" in env
    assert "SIMWORKBENCH_STRUCTLOG_PATH" in env
    assert data.get("mcp_write_permissions", {}).get("policy_file") == "config/mcp_permissions.yaml"
    assert data.get("structured_logging", {}).get("module") == "bridge/structured_log.py"
    pkgs = ("gazebo_mcp", "freecad_mcp", "ros_mcp")
    for key in pkgs:
        assert key in ci
        assert "package_version" in ci[key]
        assert "source_path" in ci[key]


@pytest.mark.parametrize(
    "pkg,version",
    [
        ("gazebo_mcp", "0.2.0"),
        ("freecad_mcp", "0.1.17"),
        ("ros_mcp", "3.0.1"),
    ],
)
def test_runtime_manifest_mcp_versions_match_pyproject(pkg: str, version: str):
    data = _load()
    rel = data["canonical_ci"][pkg]["pyproject"]
    pyproject = REPO_ROOT / rel
    assert pyproject.is_file(), f"missing {pyproject}"
    meta = tomllib.loads(pyproject.read_text(encoding="utf-8"))["project"]
    assert meta["version"] == version
