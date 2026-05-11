# SPDX-License-Identifier: LGPL-2.1-or-later
"""
bridge/structured_log.py — newline-delimited JSON (JSONL) events for bridge / E2E.

Writes one JSON object per line when a log path is resolved (see
:func:`resolve_structured_log_path`).  No secrets or full environment dumps.

Resolution order:

1. ``BRIDGE_STRUCTLOG_PATH`` — explicit file path (E2E sets this under ``sim_runs/<run>/logs/``).
2. ``SIMWORKBENCH_STRUCTLOG_PATH`` — FreeCAD panel / host override.
3. If ``E2E_RUN_DIR`` is set: ``<E2E_RUN_DIR>/logs/structured.jsonl``.

Library modules call :func:`append_event`; if no path is configured, calls are no-ops.
The runner may pass ``log_path=…`` to write ``sim_runs/<run>/logs/structured.jsonl`` next to ``result.yaml``.
"""

from __future__ import annotations

import json
import os
import re
import time
from pathlib import Path
from typing import Any, Dict, Optional

_EVENT_SCHEMA = 1
_MAX_STR = 800
_SENSITIVE_KEY = re.compile(
    r"(password|passwd|token|secret|api_?key|authorization|bearer|credential)",
    re.I,
)


def resolve_structured_log_path() -> Optional[Path]:
    for env_key in ("BRIDGE_STRUCTLOG_PATH", "SIMWORKBENCH_STRUCTLOG_PATH"):
        raw = os.environ.get(env_key, "").strip()
        if raw:
            return Path(raw).expanduser().resolve(strict=False)
    e2e = os.environ.get("E2E_RUN_DIR", "").strip()
    if e2e:
        return (Path(e2e) / "logs" / "structured.jsonl").resolve(strict=False)
    return None


def _truncate(s: str, max_len: int = _MAX_STR) -> str:
    s = str(s)
    if len(s) <= max_len:
        return s
    return s[: max_len - 3] + "..."


def sanitize_event(data: Dict[str, Any]) -> Dict[str, Any]:
    """Drop sensitive-looking keys and truncate long string values."""
    out: Dict[str, Any] = {}
    for k, v in data.items():
        if _SENSITIVE_KEY.search(str(k)):
            continue
        if isinstance(v, dict):
            out[k] = sanitize_event(v)  # type: ignore[assignment]
        elif isinstance(v, str):
            out[k] = _truncate(v)
        elif isinstance(v, (int, float, bool)) or v is None:
            out[k] = v
        else:
            out[k] = _truncate(str(v))
    return out


def append_event(event: Dict[str, Any], *, log_path: Optional[Path] = None) -> Optional[Path]:
    """
    Append one sanitized JSON line to the structured log file, if configured.

    If *log_path* is set, it overrides :func:`resolve_structured_log_path` (used by
    the runner to write next to ``result.yaml`` without touching process env).

    Adds ``schema``, ``ts_unix``, and ``ts_iso`` unless present.
    """
    path = log_path or resolve_structured_log_path()
    if path is None:
        return None

    try:
        payload = dict(event)
        payload.setdefault("schema", _EVENT_SCHEMA)
        payload.setdefault("ts_unix", time.time())
        if "ts_iso" not in payload:
            payload["ts_iso"] = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())

        line = json.dumps(sanitize_event(payload), ensure_ascii=False, default=str) + "\n"
        path.parent.mkdir(parents=True, exist_ok=True)
        with path.open("a", encoding="utf-8") as fh:
            fh.write(line)
        return path
    except OSError:
        return None


_GAZEBO_MCP_SESSION_EVENTS = frozenset(
    {
        "session_start",
        "session_ready",
        "session_error",
        "reconnect_attempt",
        "reconnect_success",
        "reconnect_failed",
    }
)


def log_gazebo_mcp_session_event(
    *,
    component: str,
    event: str,
    message: Optional[str] = None,
    tool: Optional[str] = None,
    attempt: Optional[int] = None,
    max_attempts: Optional[int] = None,
    read_only: Optional[bool] = None,
    exc_type: Optional[str] = None,
) -> None:
    """
    Append a Gazebo MCP stdio session lifecycle line to JSONL (when configured).

    *event* must be one of:
    ``session_start``, ``session_ready``, ``session_error``,
    ``reconnect_attempt``, ``reconnect_success``, ``reconnect_failed``.
    """
    if event not in _GAZEBO_MCP_SESSION_EVENTS:
        bad = event
        event = "session_error"
        message = ((message or "") + f" (invalid lifecycle event name: {bad!r})").strip()
    append_event(
        {
            "event": event,
            "component": component,
            "lifecycle": "gazebo_mcp_session",
            "message": message,
            "tool": tool,
            "attempt": attempt,
            "max_attempts": max_attempts,
            "read_only": read_only,
            "exc_type": exc_type,
        }
    )


def console_summary_line(event: Dict[str, Any]) -> str:
    """Short human-readable line for FreeCAD console (not full JSON)."""
    ev = event.get("event", "event")
    comp = event.get("component", "")
    prefix = f"[SimWorkbench:{comp}] " if comp else "[SimWorkbench] "
    if ev == "mcp_tool_call":
        ok = event.get("ok")
        tool = event.get("tool", "?")
        ms = event.get("duration_ms")
        return f"{prefix}MCP {tool} ok={ok} {ms}ms" if ms is not None else f"{prefix}MCP {tool} ok={ok}"
    if ev == "gazebo_capture":
        return (
            f"{prefix}capture ok={event.get('ok')} source={event.get('camera_source_mode')} "
            f"{event.get('image_width')}x{event.get('image_height')}"
        )
    if ev == "permission_check":
        return f"{prefix}permission {event.get('decision')} tool={event.get('tool')!r}"
    if ev == "simworkbench_panel":
        return (
            f"{prefix}{event.get('action')} ok={event.get('ok')} "
            f"{(event.get('message') or '')[:120]}"
        )
    if ev == "scenario_run_result":
        return (
            f"{prefix}scenario {event.get('scenario')} status={event.get('status')} "
            f"path={(event.get('artifact_path') or '')[:80]}"
        )
    if ev in _GAZEBO_MCP_SESSION_EVENTS and event.get("lifecycle") == "gazebo_mcp_session":
        bits = [ev]
        if event.get("attempt") is not None and event.get("max_attempts") is not None:
            bits.append(f"{event.get('attempt')}/{event.get('max_attempts')}")
        if event.get("tool"):
            bits.append(f"tool={event.get('tool')}")
        return prefix + " ".join(bits) + (f" — {(event.get('message') or '')[:100]}" if event.get("message") else "")
    return prefix + str(ev)


def log_permission_check(
    *,
    component: str,
    decision: str,
    tool: Optional[str] = None,
    path: Optional[str] = None,
    read_only: Optional[bool] = None,
    detail: Optional[str] = None,
) -> None:
    append_event(
        {
            "event": "permission_check",
            "component": component,
            "decision": decision,
            "tool": tool,
            "path": path,
            "read_only": read_only,
            "detail": detail,
        }
    )


def log_mcp_tool_result(
    *,
    component: str,
    tool: str,
    ok: bool,
    duration_ms: float,
    message: Optional[str] = None,
    is_error: Optional[bool] = None,
    permission_gate: Optional[str] = None,
) -> None:
    append_event(
        {
            "event": "mcp_tool_call",
            "component": component,
            "tool": tool,
            "ok": ok,
            "duration_ms": round(duration_ms, 2),
            "message": message,
            "mcp_is_error": is_error,
            "permission_gate": permission_gate,
        }
    )


def log_gazebo_capture(
    *,
    component: str,
    ok: bool,
    sensor_name: Optional[str],
    artifact_path: Optional[str],
    camera_source_mode: Optional[str] = None,
    image_width: Optional[Any] = None,
    image_height: Optional[Any] = None,
    gz_image_topic: Optional[str] = None,
    message: Optional[str] = None,
    exc_type: Optional[str] = None,
    exc_message: Optional[str] = None,
) -> None:
    append_event(
        {
            "event": "gazebo_capture",
            "component": component,
            "ok": ok,
            "sensor_name": sensor_name,
            "artifact_path": artifact_path,
            "camera_source_mode": camera_source_mode,
            "image_width": image_width,
            "image_height": image_height,
            "gz_image_topic": gz_image_topic,
            "message": message,
            "exc_type": exc_type,
            "exc_message": exc_message,
        }
    )


def log_panel_mcp_status(
    *,
    component: str,
    action: str,
    ok: bool,
    message: str = "",
    exc_type: Optional[str] = None,
    exc_message: Optional[str] = None,
) -> str:
    d = {
        "event": "simworkbench_panel",
        "component": component,
        "action": action,
        "ok": ok,
        "message": message,
        "exc_type": exc_type,
        "exc_message": exc_message,
    }
    append_event(d)
    return console_summary_line(d)
