# SPDX-License-Identifier: LGPL-2.1-or-later

"""Tests for bridge.mcp_write_policy (MCP write / mutation bounds)."""

from __future__ import annotations

import os
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from bridge.mcp_write_policy import (  # noqa: E402
    MCPFilesystemWriteDenied,
    MCPMutatingToolDenied,
    MCPRepoReadDenied,
    enforce_gazebo_mcp_call,
    ensure_allowed_write_path,
    ensure_repo_read_path,
    enforce_spawn_model_urdf_read,
    is_gazebo_mcp_read_only,
    load_mcp_write_policy,
    reload_mcp_write_policy_for_tests,
)


@pytest.fixture(autouse=True)
def _clear_policy_cache():
    yield
    reload_mcp_write_policy_for_tests()


def test_allowed_write_under_sim_runs():
    p = REPO_ROOT / "sim_runs" / "pytest_mcp_policy" / "nested" / "shot.png"
    got = ensure_allowed_write_path(p)
    assert got.is_absolute()
    assert "sim_runs" in got.parts


def test_allowed_write_under_generated():
    p = REPO_ROOT / "generated" / "pytest_mcp_policy" / "out.sdf"
    got = ensure_allowed_write_path(p)
    assert got.is_absolute()


def test_denied_write_outside_roots():
    with pytest.raises(MCPFilesystemWriteDenied):
        ensure_allowed_write_path(REPO_ROOT / "project.yaml")


def test_extra_write_roots_env(tmp_path, monkeypatch):
    extra = tmp_path / "extra_out"
    extra.mkdir()
    monkeypatch.setenv("BRIDGE_MCP_EXTRA_WRITE_ROOTS", str(extra))
    reload_mcp_write_policy_for_tests()
    try:
        target = extra / "deep" / "file.txt"
        got = ensure_allowed_write_path(target)
        assert _under(extra, got)
    finally:
        monkeypatch.delenv("BRIDGE_MCP_EXTRA_WRITE_ROOTS", raising=False)
        reload_mcp_write_policy_for_tests()


def _under(parent: Path, child: Path) -> bool:
    try:
        child.relative_to(parent.resolve())
        return True
    except ValueError:
        return False


def test_repo_read_allowed_robots_urdf():
    p = REPO_ROOT / "robots" / "arm_2dof.urdf"
    got = ensure_repo_read_path(p)
    assert got.is_file()


def test_repo_read_denied_outside_repo(tmp_path):
    outside = tmp_path / "secret.urdf"
    outside.write_text("<robot/>")
    with pytest.raises(MCPRepoReadDenied):
        ensure_repo_read_path(outside)


def test_spawn_model_read_enforcement():
    enforce_spawn_model_urdf_read(REPO_ROOT / "robots" / "arm_2dof.urdf")


def test_read_only_tool_classification():
    pol = load_mcp_write_policy(REPO_ROOT)
    assert is_gazebo_mcp_read_only("gazebo_get_simulation_status", pol)
    assert is_gazebo_mcp_read_only("get_simulation_status", pol)
    assert not is_gazebo_mcp_read_only("gazebo_spawn_model", pol)
    assert not is_gazebo_mcp_read_only("spawn_model", pol)


def test_deny_mutating_mcp_tools(monkeypatch):
    monkeypatch.setenv("BRIDGE_MCP_DENY_MUTATING", "1")
    reload_mcp_write_policy_for_tests()
    try:
        with pytest.raises(MCPMutatingToolDenied):
            enforce_gazebo_mcp_call("gazebo_spawn_model", {})
        with pytest.raises(MCPMutatingToolDenied):
            enforce_gazebo_mcp_call("spawn_model", {})
        enforce_gazebo_mcp_call("gazebo_get_simulation_status", {})
        enforce_gazebo_mcp_call("gazebo_list_sensors", {"response_format": "filtered"})
    finally:
        monkeypatch.delenv("BRIDGE_MCP_DENY_MUTATING", raising=False)
        reload_mcp_write_policy_for_tests()


def test_save_world_output_path_must_be_under_allowed_roots(monkeypatch):
    monkeypatch.delenv("BRIDGE_MCP_DENY_MUTATING", raising=False)
    reload_mcp_write_policy_for_tests()
    bad = str(Path(os.environ.get("TEMP", "/tmp")) / "escape_world.sdf")
    with pytest.raises(MCPFilesystemWriteDenied):
        enforce_gazebo_mcp_call("gazebo_save_world", {"output_path": bad})

    good = str(REPO_ROOT / "sim_runs" / "pytest_mcp_save" / "world.sdf")
    enforce_gazebo_mcp_call("gazebo_save_world", {"output_path": good})


def test_load_world_path_must_be_in_repo(tmp_path):
    good = str(REPO_ROOT / "worlds" / "empty_world.sdf")
    enforce_gazebo_mcp_call("gazebo_load_world", {"world_file_path": good})
    bad = str(tmp_path / "outside.sdf")
    with pytest.raises(MCPRepoReadDenied):
        enforce_gazebo_mcp_call("gazebo_load_world", {"world_file_path": bad})


def test_spawn_model_model_file_arg_must_be_in_repo(tmp_path):
    good = str(REPO_ROOT / "robots" / "arm_2dof.urdf")
    enforce_gazebo_mcp_call("gazebo_spawn_model", {"model_file": good, "model_name": "x"})
    bad = str(tmp_path / "evil.urdf")
    Path(bad).write_text("<robot/>")
    with pytest.raises(MCPRepoReadDenied):
        enforce_gazebo_mcp_call("gazebo_spawn_model", {"model_file": bad, "model_name": "x"})
