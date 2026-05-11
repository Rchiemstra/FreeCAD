# SPDX-License-Identifier: LGPL-2.1-or-later

"""Tests for bridge.structured_log (JSONL helpers)."""

from __future__ import annotations

import json
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))

from bridge.structured_log import (  # noqa: E402
    append_event,
    console_summary_line,
    log_gazebo_mcp_session_event,
    resolve_structured_log_path,
    sanitize_event,
)


def test_sanitize_strips_sensitive_keys():
    d = sanitize_event(
        {
            "tool": "x",
            "api_key": "secret",
            "nested": {"password": "nope", "ok": True},
        }
    )
    assert "api_key" not in d
    assert "password" not in d.get("nested", {})


def test_resolve_path_prefers_bridge_env(tmp_path, monkeypatch):
    logf = tmp_path / "custom.jsonl"
    monkeypatch.setenv("BRIDGE_STRUCTLOG_PATH", str(logf))
    monkeypatch.delenv("SIMWORKBENCH_STRUCTLOG_PATH", raising=False)
    monkeypatch.delenv("E2E_RUN_DIR", raising=False)
    assert resolve_structured_log_path() == logf.resolve(strict=False)


def test_resolve_path_e2e_run_dir(tmp_path, monkeypatch):
    monkeypatch.delenv("BRIDGE_STRUCTLOG_PATH", raising=False)
    monkeypatch.delenv("SIMWORKBENCH_STRUCTLOG_PATH", raising=False)
    run = tmp_path / "e2e_test"
    run.mkdir()
    monkeypatch.setenv("E2E_RUN_DIR", str(run))
    assert resolve_structured_log_path() == (run / "logs" / "structured.jsonl").resolve(strict=False)


def test_append_event_explicit_log_path(tmp_path):
    sim = tmp_path / "sim_runs" / "pytest_struct"
    sim.mkdir(parents=True)
    logf = sim / "logs" / "structured.jsonl"
    out = append_event({"event": "test", "hello": "world"}, log_path=logf)
    assert out is not None
    assert out.name == "structured.jsonl"
    lines = logf.read_text(encoding="utf-8").strip().splitlines()
    assert len(lines) == 1
    row = json.loads(lines[0])
    assert row["event"] == "test"
    assert row["hello"] == "world"
    assert "schema" in row
    assert "ts_unix" in row


def test_console_summary_mcp_tool_call():
    line = console_summary_line(
        {
            "event": "mcp_tool_call",
            "component": "bridge.gazebo_bridge",
            "tool": "gazebo_list_sensors",
            "ok": True,
            "duration_ms": 12.3,
            "permission_gate": "read_only",
        }
    )
    assert "gazebo_list_sensors" in line
    assert "ok=True" in line


def test_bridge_mcp_log_emits_when_env_set(monkeypatch, tmp_path):
    """One bridge path: log_mcp_tool_result writes JSONL when BRIDGE_STRUCTLOG_PATH is set."""
    from bridge import structured_log

    sim = tmp_path / "sim_runs" / "pytest_mcp_log"
    sim.mkdir(parents=True)
    logf = sim / "logs" / "out.jsonl"
    monkeypatch.setenv("BRIDGE_STRUCTLOG_PATH", str(logf))
    structured_log.log_mcp_tool_result(
        component="bridge.gazebo_bridge",
        tool="gazebo_get_simulation_status",
        ok=True,
        duration_ms=5.0,
        permission_gate="read_only",
    )
    assert logf.is_file()
    row = json.loads(logf.read_text(encoding="utf-8").strip())
    assert row["event"] == "mcp_tool_call"
    assert row["tool"] == "gazebo_get_simulation_status"
    assert row["permission_gate"] == "read_only"


def test_log_gazebo_mcp_session_event_jsonl(tmp_path, monkeypatch):
    logf = tmp_path / "sess.jsonl"
    monkeypatch.setenv("BRIDGE_STRUCTLOG_PATH", str(logf))
    log_gazebo_mcp_session_event(
        component="bridge.gazebo_bridge",
        event="reconnect_attempt",
        tool="gazebo_get_simulation_status",
        read_only=True,
        message="test",
    )
    row = json.loads(logf.read_text(encoding="utf-8").strip())
    assert row["event"] == "reconnect_attempt"
    assert row["lifecycle"] == "gazebo_mcp_session"
    assert row["read_only"] is True


def test_console_summary_session_lifecycle():
    line = console_summary_line(
        {
            "event": "session_ready",
            "component": "bridge.gazebo_bridge",
            "lifecycle": "gazebo_mcp_session",
            "attempt": 2,
            "max_attempts": 3,
        }
    )
    assert "session_ready" in line
    assert "2/3" in line


def test_runner_try_write_emits_scenario_jsonl(tmp_path):
    from runner.result import RunResult
    from runner.runner import _try_write
    from runner.scenario import Scenario

    sr = tmp_path / "sim_runs"
    sr.mkdir(parents=True)
    sc = Scenario()
    sc.name = "unit_struct_log"
    r = RunResult(scenario=sc, assertion_results=[], status="pass")
    _try_write(r, sr)
    logf = sr / r.run_id / "logs" / "structured.jsonl"
    assert logf.is_file(), f"missing {logf}"
    row = json.loads(logf.read_text(encoding="utf-8").strip().splitlines()[-1])
    assert row["event"] == "scenario_run_result"
    assert row["scenario"] == "unit_struct_log"
