"""Offline tests for bridge.gazebo_lifecycle and world SDF consistency."""
from __future__ import annotations

import os
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]


class TestGazeboLifecycle:
    def test_world_sdf_declares_empty_world(self):
        sdf = REPO_ROOT / "worlds" / "empty_world.sdf"
        text = sdf.read_text(encoding="utf-8")
        assert '<world name="empty_world">' in text

    def test_resolve_world_name_defaults(self, monkeypatch):
        monkeypatch.delenv("GZ_SIM_WORLD_NAME", raising=False)
        monkeypatch.delenv("GAZEBO_WORLD_NAME", raising=False)
        from bridge.gazebo_lifecycle import resolve_world_name

        assert resolve_world_name() == "empty_world"

    def test_resolve_world_name_prefers_gz_sim(self, monkeypatch):
        monkeypatch.setenv("GZ_SIM_WORLD_NAME", "custom")
        monkeypatch.setenv("GAZEBO_WORLD_NAME", "other")
        from bridge.gazebo_lifecycle import resolve_world_name

        assert resolve_world_name() == "custom"

    def test_validate_world_env_mismatch(self, monkeypatch):
        monkeypatch.setenv("GZ_SIM_WORLD_NAME", "empty_world")
        monkeypatch.setenv("GAZEBO_WORLD_NAME", "empty")
        from bridge.gazebo_lifecycle import validate_world_env

        ok, msg = validate_world_env()
        assert not ok
        assert "mismatch" in msg.lower() or "!=" in msg

    def test_validate_world_env_ok(self, monkeypatch):
        monkeypatch.setenv("GZ_SIM_WORLD_NAME", "empty_world")
        monkeypatch.setenv("GAZEBO_WORLD_NAME", "empty_world")
        from bridge.gazebo_lifecycle import validate_world_env

        ok, _ = validate_world_env()
        assert ok

    def test_export_live_defaults_sync_worlds(self, monkeypatch):
        monkeypatch.delenv("GZ_SIM_WORLD_NAME", raising=False)
        monkeypatch.delenv("GAZEBO_WORLD_NAME", raising=False)
        from bridge.gazebo_lifecycle import export_live_defaults

        env = export_live_defaults()
        assert env["GZ_SIM_WORLD_NAME"] == env["GAZEBO_WORLD_NAME"] == "empty_world"
        assert env["GZ_SIM_CONTAINER_NAME"] == "gz-sim-sever"

    def test_gz_cli_and_gazebo_gz_docker_same_world(self, monkeypatch):
        monkeypatch.setenv("GZ_SIM_WORLD_NAME", "empty_world")
        monkeypatch.setenv("GAZEBO_WORLD_NAME", "empty_world")
        from bridge.gz_cli_bridge import _world_name
        from bridge.gazebo_gz_docker import _world_name as docker_world

        assert _world_name() == docker_world() == "empty_world"

    def test_smoke_script_exists(self):
        assert (REPO_ROOT / "scripts" / "smoke_gz_lifecycle.sh").is_file()
        assert (REPO_ROOT / "scripts" / "stop_gz_stack.sh").is_file()
