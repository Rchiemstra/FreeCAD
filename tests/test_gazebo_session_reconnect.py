# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline tests for Gazebo MCP session start retries and read-only transport reconnect."""

from __future__ import annotations

import json
import sys
from pathlib import Path

import pytest

REPO_ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO_ROOT))


class _FakeMCPClient:
    """Minimal stdio MCP stand-in for :class:`bridge.gazebo_bridge.GazeboSession`."""

    _call_failures: dict[str, int] = {}
    init_failures_remaining: int = 0
    _init_seq: int = 0
    fail_init_on_seq: int | None = None

    def __init__(self, cmd, timeout: float = 15.0) -> None:
        self._cmd = cmd
        self._timeout = timeout
        self._initialized = False

    def start(self) -> None:
        return

    def stop(self) -> None:
        self._initialized = False

    def is_alive(self) -> bool:
        return True

    def initialize(self) -> bool:
        _FakeMCPClient._init_seq += 1
        if _FakeMCPClient.fail_init_on_seq == _FakeMCPClient._init_seq:
            return False
        if _FakeMCPClient.init_failures_remaining > 0:
            _FakeMCPClient.init_failures_remaining -= 1
            return False
        self._initialized = True
        return True

    def call_tool(self, tool_name: str, arguments: dict) -> dict:
        if not self._initialized:
            raise RuntimeError("MCP client not initialized — call initialize() first")
        n = _FakeMCPClient._call_failures.get(tool_name, 0)
        if n > 0:
            _FakeMCPClient._call_failures[tool_name] = n - 1
            raise TimeoutError(f"simulated transport failure for {tool_name}")
        return {"content": [{"type": "text", "text": '{"success": true, "message": "ok"}'}]}


def _reset_fake():
    _FakeMCPClient._call_failures.clear()
    _FakeMCPClient.init_failures_remaining = 0
    _FakeMCPClient._init_seq = 0
    _FakeMCPClient.fail_init_on_seq = None


@pytest.fixture
def fake_mcp_factory(monkeypatch):
    from bridge import gazebo_bridge

    def _factory(cmd, timeout):
        return _FakeMCPClient(cmd, timeout)

    monkeypatch.setattr(gazebo_bridge, "_mcp_client_factory", _factory)
    _reset_fake()
    yield
    monkeypatch.setattr(gazebo_bridge, "_mcp_client_factory", None)
    _reset_fake()


def test_session_lifecycle_success_jsonl(tmp_path, monkeypatch, fake_mcp_factory):
    from bridge import gazebo_bridge

    logf = tmp_path / "lc.jsonl"
    monkeypatch.setenv("BRIDGE_STRUCTLOG_PATH", str(logf))

    with gazebo_bridge.GazeboSession(timeout=5.0) as gz:
        raw = gz("gazebo_get_simulation_status", {})
        ok, data, msg = gazebo_bridge._parse_tool_result(raw)
        assert ok

    lines = [json.loads(x) for x in logf.read_text(encoding="utf-8").splitlines() if x.strip()]
    evs = [x.get("event") for x in lines]
    assert "session_start" in evs
    assert "session_ready" in evs
    assert "session_error" not in evs


def test_session_start_retries_then_ready(tmp_path, monkeypatch, fake_mcp_factory):
    from bridge import gazebo_bridge

    _FakeMCPClient.init_failures_remaining = 2
    monkeypatch.setenv("BRIDGE_STRUCTLOG_PATH", str(tmp_path / "a.jsonl"))
    monkeypatch.setenv("BRIDGE_GAZEBO_MCP_SESSION_START_ATTEMPTS", "4")

    with gazebo_bridge.GazeboSession(timeout=5.0) as gz:
        raw = gz("gazebo_get_simulation_status", {})
        ok, _, _ = gazebo_bridge._parse_tool_result(raw)
        assert ok

    logf = tmp_path / "a.jsonl"
    lines = [json.loads(x) for x in logf.read_text(encoding="utf-8").splitlines() if x.strip()]
    starts = [x for x in lines if x.get("event") == "session_start"]
    assert len(starts) == 3
    assert any(x.get("event") == "session_ready" for x in lines)


def test_readonly_transport_reconnect_emits_events(tmp_path, monkeypatch, fake_mcp_factory):
    from bridge import gazebo_bridge

    _FakeMCPClient._call_failures["gazebo_get_simulation_status"] = 1
    logf = tmp_path / "r.jsonl"
    monkeypatch.setenv("BRIDGE_STRUCTLOG_PATH", str(logf))
    monkeypatch.setenv("BRIDGE_GAZEBO_MCP_READONLY_TRANSPORT_RETRIES", "1")

    with gazebo_bridge.GazeboSession(timeout=5.0) as gz:
        raw = gz("gazebo_get_simulation_status", {})
        ok, _, _ = gazebo_bridge._parse_tool_result(raw)
        assert ok

    lines = [json.loads(x) for x in logf.read_text(encoding="utf-8").splitlines() if x.strip()]
    evs = [x.get("event") for x in lines]
    assert evs.count("reconnect_attempt") >= 1
    assert "reconnect_success" in evs
    assert "reconnect_failed" not in evs


def test_mutating_tool_no_transport_retry(tmp_path, monkeypatch, fake_mcp_factory):
    from bridge import gazebo_bridge

    _FakeMCPClient._call_failures["spawn_model"] = 3
    monkeypatch.setenv("BRIDGE_STRUCTLOG_PATH", str(tmp_path / "m.jsonl"))
    monkeypatch.setenv("BRIDGE_GAZEBO_MCP_READONLY_TRANSPORT_RETRIES", "2")

    with pytest.raises(TimeoutError):
        with gazebo_bridge.GazeboSession(timeout=5.0) as gz:
            gz(
                "spawn_model",
                {"model_name": "x", "model_xml": "<robot name='x'/>"},
            )

    lines = [json.loads(x) for x in (tmp_path / "m.jsonl").read_text(encoding="utf-8").splitlines() if x.strip()]
    assert not any(x.get("event") == "reconnect_attempt" for x in lines)


def test_reconnect_handshake_failure_emits_reconnect_failed(tmp_path, monkeypatch, fake_mcp_factory):
    from bridge import gazebo_bridge

    _FakeMCPClient._call_failures["gazebo_get_simulation_status"] = 1
    # Second client (after reconnect) is the next initialize() sequence — force failure there.
    _FakeMCPClient.fail_init_on_seq = 2
    monkeypatch.setenv("BRIDGE_STRUCTLOG_PATH", str(tmp_path / "f.jsonl"))
    monkeypatch.setenv("BRIDGE_GAZEBO_MCP_READONLY_TRANSPORT_RETRIES", "1")

    with pytest.raises(RuntimeError, match="reconnect failed"):
        with gazebo_bridge.GazeboSession(timeout=5.0) as gz:
            gz("gazebo_get_simulation_status", {})

    lines = [json.loads(x) for x in (tmp_path / "f.jsonl").read_text(encoding="utf-8").splitlines() if x.strip()]
    assert any(x.get("event") == "reconnect_failed" for x in lines)


def test_user_hint_for_timeout():
    from bridge.gazebo_bridge import user_hint_for_gazebo_mcp_failure

    h = user_hint_for_gazebo_mcp_failure(TimeoutError())
    assert "timed out" in h.lower()


def test_recoverable_transport_classifies_timeout():
    from bridge.gazebo_bridge import _recoverable_transport_error

    assert _recoverable_transport_error(TimeoutError()) is True
    assert _recoverable_transport_error(RuntimeError("No response from gazebo-mcp")) is True
    assert _recoverable_transport_error(RuntimeError("MCP error: {'code': -1}")) is False
