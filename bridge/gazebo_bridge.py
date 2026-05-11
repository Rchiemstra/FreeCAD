"""
bridge.gazebo_bridge — Gazebo spawn/control tools (MCP stdio).

Communicates with the gazebo-mcp server running in WSL2 via the same
stdio MCP protocol used by the test suite. This keeps the bridge on
Windows (no WSL Python dependency) while all Gazebo interaction goes
through the MCP server subprocess in WSL.

Tools exposed:
  - wait_for_ready()     Wait until Gazebo is ready to accept commands
  - spawn_model()        Spawn a URDF or SDF robot model in Gazebo
  - spawn_world()        Load a world SDF into a running Gazebo instance
  - get_model_state()    Return pose + twist for a named model
  - pause_simulation()   Pause Gazebo physics
  - resume_simulation()  Resume Gazebo physics
  - reset_simulation()   Reset Gazebo to initial state
  - step_simulation()    Advance Gazebo by N physics steps
  - get_simulation_status()  One-shot status for UI panels
  - list_gazebo_sensors()    Sensor listing (gazebo-mcp)
  - capture_camera_snapshot() Save a camera frame via gazebo_get_sensor_data

All functions return a GazeboResult(ok, data, messages) so callers
never need to catch MCP protocol exceptions.

BLOCKER (Phase 1): Requires Gazebo Docker to be running:
  Run Start-gz-sim.bat and wait for the container to be ready.
  Then these tools will connect via the gazebo-mcp MCP server.

MCP write policy (Phase 6): ``GazeboSession`` routes tools/call through
``bridge.mcp_write_policy.enforce_gazebo_mcp_call`` (path-arg validation + optional
``BRIDGE_MCP_DENY_MUTATING``). Local screenshot/export paths use ``sim_runs/`` and
``generated/`` (see ``config/mcp_permissions.yaml``).

Transport hardening (Phase 6): bounded **session start** retries and **read-only**
MCP ``tools/call`` reconnect on transport failures (timeouts / broken pipe); **mutating**
tools are never auto-retried. Lifecycle events go to structured JSONL when configured
(``session_start``, ``session_ready``, ``session_error``, ``reconnect_*``).
"""

from __future__ import annotations

import base64
import errno
import json
import logging
import os
import queue
import subprocess
import sys
import threading
import time
from dataclasses import dataclass, field
from pathlib import Path
from datetime import datetime, timezone
from typing import Any, Dict, List, Optional, Tuple

from bridge.mcp_write_policy import (
    MCPRepoReadDenied,
    enforce_gazebo_mcp_call,
    ensure_allowed_write_path,
    enforce_spawn_model_urdf_read,
)

logger = logging.getLogger(__name__)

# Test hook: optional ``(cmd, timeout) -> client`` with same surface as ``_MCPClient``.
_mcp_client_factory: Optional[Any] = None


def _int_env(name: str, default: int, *, min_v: int = 1, max_v: int = 10) -> int:
    try:
        v = int(os.environ.get(name, str(default)).strip())
    except ValueError:
        return default
    return max(min_v, min(max_v, v))


def _session_start_attempts() -> int:
    """How many times to try subprocess start + MCP initialize (default 3)."""
    return _int_env("BRIDGE_GAZEBO_MCP_SESSION_START_ATTEMPTS", 3, min_v=1, max_v=8)


def _readonly_transport_retries() -> int:
    """
    Extra MCP tool attempts after a transport-level failure for **read-only** tools only.

    ``1`` means at most one full stdio reconnect then a second ``tools/call`` attempt.
    Mutating tools never use this path.
    """
    return _int_env("BRIDGE_GAZEBO_MCP_READONLY_TRANSPORT_RETRIES", 1, min_v=0, max_v=3)


def _recoverable_transport_error(exc: BaseException) -> bool:
    """True when a new stdio MCP subprocess may fix the failure (timeouts, broken pipes)."""
    if isinstance(exc, (TimeoutError, BrokenPipeError, ConnectionAbortedError, ConnectionResetError)):
        return True
    if isinstance(exc, OSError):
        err = getattr(exc, "errno", None) or getattr(exc, "winerror", None) or 0
        codes: set[int] = {232}  # Windows: pipe closing / no data
        for nm in ("EPIPE", "ECONNRESET", "ECONNABORTED"):
            c = getattr(errno, nm, None)
            if isinstance(c, int) and c != 0:
                codes.add(c)
        if err in codes:
            return True
    if isinstance(exc, RuntimeError):
        sl = str(exc).lower()
        if "no response" in sl or "broken pipe" in sl:
            return True
    return False


def user_hint_for_gazebo_mcp_failure(exc: BaseException) -> str:
    """Short, UI-friendly message for FreeCAD panels and ``GazeboResult.messages``."""
    if isinstance(exc, TimeoutError):
        return (
            "Gazebo MCP timed out — headless sim or gazebo-mcp may still be starting. "
            "Wait a few seconds and retry."
        )
    if isinstance(exc, (BrokenPipeError, ConnectionResetError, ConnectionAbortedError)):
        return "Gazebo MCP connection dropped — retry after the sim or MCP server is stable."
    if isinstance(exc, OSError):
        err = getattr(exc, "errno", None) or getattr(exc, "winerror", None) or 0
        codes: set[int] = {232}
        for nm in ("EPIPE", "ECONNRESET", "ECONNABORTED"):
            c = getattr(errno, nm, None)
            if isinstance(c, int) and c != 0:
                codes.add(c)
        if err in codes:
            return "Gazebo MCP pipe closed — the MCP subprocess may have exited; retry or restart the sim stack."
    if isinstance(exc, RuntimeError):
        sl = str(exc).lower()
        if "no response" in sl:
            return (
                "No response from gazebo-mcp — check that the Gazebo Docker/WSL stack is running, "
                "then retry."
            )
        if "reconnect failed" in sl or "initialization handshake failed" in sl:
            return str(exc)[:400]
    return str(exc)[:400]


# ── Paths ──────────────────────────────────────────────────────────────────────
_REPO_ROOT  = Path(__file__).parent.parent.resolve()
_GAZEBO_MCP = _REPO_ROOT / "tools" / "mcp" / "gazebo-mcp"


def _get_wsl_path(windows_path: Path) -> Optional[str]:
    """Convert a Windows path to a WSL Linux path using wslpath."""
    try:
        win_str = str(windows_path).replace("\\", "/")
        result = subprocess.run(
            ["wsl", "--", "bash", "-c", f"wslpath -a '{win_str}'"],
            capture_output=True, text=True, timeout=10,
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except Exception:
        pass
    return None


def _build_gazebo_server_cmd() -> List[str]:
    """Start gazebo-mcp-server (stdio MCP)."""
    manual = os.environ.get("GAZEBO_MCP_CMD", "").strip()
    if manual:
        return manual.split()

    # Docker E2E / CI: unified venv at MCP_VENV (no per-submodule .venv)
    mcp_venv = os.environ.get("MCP_VENV", "").strip()
    if mcp_venv:
        shared = Path(mcp_venv) / "bin" / "gazebo-mcp-server"
        if shared.is_file():
            return [
                "bash", "-lc",
                f"cd '{_GAZEBO_MCP}' && exec '{shared}'",
            ]

    venv_server = _GAZEBO_MCP / ".venv" / "bin" / "gazebo-mcp-server"

    if sys.platform == "win32":
        wsl_path = _get_wsl_path(_GAZEBO_MCP)
        if wsl_path:
            return [
                "wsl", "--", "bash", "-c",
                f"cd '{wsl_path}' && .venv/bin/gazebo-mcp-server",
            ]
        fallback = str(_GAZEBO_MCP).replace("\\", "/")
        logger.warning("wslpath conversion failed; using fallback path: %s", fallback)
        return [
            "wsl", "--", "bash", "-c",
            f"cd '{fallback}' && .venv/bin/gazebo-mcp-server",
        ]

    # Linux / macOS — local venv entry-point with package cwd for relative assets
    if venv_server.is_file():
        return [
            "bash", "-lc",
            f"cd '{_GAZEBO_MCP}' && exec '{venv_server}'",
        ]
    return ["bash", "-lc", f"cd '{_GAZEBO_MCP}' && exec gazebo-mcp-server"]


def gazebo_mcp_server_cmd() -> List[str]:
    """Argv to spawn ``gazebo-mcp-server`` (respects ``MCP_VENV``, ``GAZEBO_MCP_CMD``)."""
    return _build_gazebo_server_cmd()


# ── Result type ────────────────────────────────────────────────────────────────

@dataclass
class GazeboResult:
    ok:       bool
    data:     Optional[Dict[str, Any]] = None
    messages: List[str]                = field(default_factory=list)

    def __bool__(self) -> bool:
        return self.ok


# ── Minimal stdio MCP client ───────────────────────────────────────────────────
# Adapted from test_all_mcp.py's MCPClientStdio — kept as a lightweight
# library version without the test scaffolding.

class _MCPClient:
    """Subprocess-based MCP client (newline-delimited JSON-RPC 2.0)."""

    def __init__(self, cmd: List[str], timeout: float = 15.0) -> None:
        self._cmd     = cmd
        self._timeout = timeout
        self._proc:   Optional[subprocess.Popen] = None
        self._rq:     queue.Queue = queue.Queue()
        self._next_id = 1
        self._initialized = False

    def start(self) -> None:
        env = os.environ.copy()
        self._proc = subprocess.Popen(
            self._cmd,
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=False,
            env=env,
        )
        threading.Thread(target=self._read_loop, daemon=True).start()
        time.sleep(0.5)

    def stop(self) -> None:
        if self._proc:
            try:
                self._proc.terminate()
                self._proc.wait(timeout=5)
            except Exception:
                self._proc.kill()
            self._proc = None

    def is_alive(self) -> bool:
        return self._proc is not None and self._proc.poll() is None

    def _read_loop(self) -> None:
        assert self._proc and self._proc.stdout
        for line in self._proc.stdout:
            stripped = line.strip()
            if not stripped:
                continue
            try:
                msg = json.loads(stripped.decode("utf-8", errors="replace"))
                self._rq.put(msg)
            except json.JSONDecodeError:
                pass

    def _send(self, msg: Dict[str, Any]) -> None:
        assert self._proc and self._proc.stdin
        payload = (json.dumps(msg) + "\n").encode("utf-8")
        self._proc.stdin.write(payload)
        self._proc.stdin.flush()

    def _recv(self, req_id: Optional[int] = None) -> Optional[Dict[str, Any]]:
        deadline = time.monotonic() + self._timeout
        while time.monotonic() < deadline:
            remaining = max(0.1, deadline - time.monotonic())
            try:
                msg = self._rq.get(timeout=min(remaining, 1.0))
                if req_id is None or msg.get("id") == req_id:
                    return msg
                self._rq.put(msg)  # put back — not ours
            except queue.Empty:
                pass
        return None

    def initialize(self) -> bool:
        if self._initialized:
            return True
        req_id = self._next_id; self._next_id += 1
        self._send({
            "jsonrpc": "2.0", "id": req_id, "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "bridge", "version": "0.1"},
            },
        })
        resp = self._recv(req_id)
        if resp and "result" in resp:
            self._send({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})
            self._initialized = True
            return True
        return False

    def call_tool(self, tool_name: str, arguments: Dict[str, Any]) -> Dict[str, Any]:
        if not self._initialized:
            raise RuntimeError("MCP client not initialized — call initialize() first")
        req_id = self._next_id; self._next_id += 1
        self._send({
            "jsonrpc": "2.0", "id": req_id, "method": "tools/call",
            "params": {"name": tool_name, "arguments": arguments},
        })
        resp = self._recv(req_id)
        if resp is None:
            raise TimeoutError(f"No response from gazebo-mcp for tool '{tool_name}'")
        if "error" in resp:
            raise RuntimeError(f"MCP error: {resp['error']}")
        return resp.get("result", {})


def _create_mcp_client(cmd: List[str], timeout: float) -> _MCPClient:
    fac = _mcp_client_factory
    if fac is not None:
        return fac(cmd, timeout)
    return _MCPClient(cmd, timeout)


# ── Context manager for a single Gazebo MCP session ───────────────────────────

class GazeboSession:
    """
    Context manager that starts the gazebo-mcp server subprocess and yields
    a ``tools/call`` wrapper.

    **Lifecycle (stdio MCP subprocess)**

    - **Session creation:** ``__enter__`` builds the server argv, then tries
      subprocess ``Popen`` + JSON-RPC ``initialize`` up to
      ``BRIDGE_GAZEBO_MCP_SESSION_START_ATTEMPTS`` times (default **3**), with
      short backoff between failures.
    - **First MCP call:** each public bridge helper typically opens one
      ``with GazeboSession()`` block and issues ``tools/call`` once or a few times.
    - **Tool failure:** JSON-RPC / MCP *application* errors (tool ran, returned
      ``isError`` or ``error``) are **not** retried at the transport layer.
    - **Process exit / broken pipe / timeout:** for tools classified **read-only**
      in ``config/mcp_permissions.yaml``, one bounded reconnect may run
      (default **1** extra attempt after reconnect via
      ``BRIDGE_GAZEBO_MCP_READONLY_TRANSPORT_RETRIES``). **Mutating** tools are
      **never** auto-retried (the first attempt may have executed server-side).

    Structured JSONL (when ``BRIDGE_STRUCTLOG_PATH`` or peers are set) may emit:
    ``session_start``, ``session_ready``, ``session_error``,
    ``reconnect_attempt``, ``reconnect_success``, ``reconnect_failed`` — see
    :func:`bridge.structured_log.log_gazebo_mcp_session_event`.

    Usage::

        with GazeboSession() as gz:
            result = gz("spawn_model", {"model_name": "arm", ...})
    """

    _LOG_COMPONENT = "bridge.gazebo_bridge"

    def __init__(self, timeout: float = 15.0) -> None:
        self._timeout = timeout
        self._client: Optional[_MCPClient] = None
        self._server_cmd: Optional[List[str]] = None

    def __enter__(self):
        from bridge import structured_log

        self._server_cmd = _build_gazebo_server_cmd()
        attempts = _session_start_attempts()
        last_err: Optional[BaseException] = None
        for i in range(attempts):
            structured_log.log_gazebo_mcp_session_event(
                component=self._LOG_COMPONENT,
                event="session_start",
                attempt=i + 1,
                max_attempts=attempts,
                message="starting gazebo-mcp stdio subprocess",
            )
            self._client = _create_mcp_client(self._server_cmd, self._timeout)
            self._client.start()
            if not self._client.is_alive():
                msg = "gazebo-mcp subprocess exited during startup"
                structured_log.log_gazebo_mcp_session_event(
                    component=self._LOG_COMPONENT,
                    event="session_error",
                    attempt=i + 1,
                    max_attempts=attempts,
                    message=msg,
                    exc_type="ProcessExited",
                )
                try:
                    self._client.stop()
                except Exception:
                    pass
                self._client = None
                last_err = RuntimeError(
                    "gazebo-mcp server process failed to start. "
                    f"Command: {' '.join(self._server_cmd)}"
                )
                if i < attempts - 1:
                    time.sleep(min(0.35 * (2**i), 2.0))
                continue
            if self._client.initialize():
                structured_log.log_gazebo_mcp_session_event(
                    component=self._LOG_COMPONENT,
                    event="session_ready",
                    attempt=i + 1,
                    max_attempts=attempts,
                    message="MCP initialize handshake ok",
                )
                return self._call
            structured_log.log_gazebo_mcp_session_event(
                component=self._LOG_COMPONENT,
                event="session_error",
                attempt=i + 1,
                max_attempts=attempts,
                message="MCP initialize handshake failed",
                exc_type="HandshakeFailed",
            )
            try:
                self._client.stop()
            except Exception:
                pass
            self._client = None
            last_err = RuntimeError("gazebo-mcp MCP initialization handshake failed")
            if i < attempts - 1:
                time.sleep(min(0.35 * (2**i), 2.0))

        assert last_err is not None
        raise last_err

    def __exit__(self, *_):
        if self._client:
            try:
                self._client.stop()
            except Exception:
                pass
            self._client = None

    def _restart_mcp_transport(self, *, tool: str, read_only: bool) -> None:
        """Stop and recreate the stdio client (same argv). Used for read-only retry only."""
        from bridge import structured_log

        assert self._server_cmd is not None
        structured_log.log_gazebo_mcp_session_event(
            component=self._LOG_COMPONENT,
            event="reconnect_attempt",
            tool=tool,
            read_only=read_only,
            message="recreating gazebo-mcp stdio client after transport failure",
        )
        if self._client:
            try:
                self._client.stop()
            except Exception:
                pass
            self._client = None
        try:
            self._client = _create_mcp_client(self._server_cmd, self._timeout)
            self._client.start()
            if not self._client.is_alive():
                raise RuntimeError("gazebo-mcp subprocess exited immediately after reconnect start")
            if not self._client.initialize():
                raise RuntimeError("gazebo-mcp MCP initialization handshake failed after reconnect")
        except Exception as exc:
            structured_log.log_gazebo_mcp_session_event(
                component=self._LOG_COMPONENT,
                event="reconnect_failed",
                tool=tool,
                read_only=read_only,
                message=str(exc),
                exc_type=type(exc).__name__,
            )
            raise RuntimeError(
                f"gazebo-mcp reconnect failed: {exc}. "
                "Check that the Gazebo stack and gazebo-mcp-server can start."
            ) from exc

        structured_log.log_gazebo_mcp_session_event(
            component=self._LOG_COMPONENT,
            event="reconnect_success",
            tool=tool,
            read_only=read_only,
            message="stdio MCP session re-established",
        )

    def _call(self, tool: str, args: Dict[str, Any]) -> Dict[str, Any]:
        enforce_gazebo_mcp_call(tool, args)
        from bridge.mcp_write_policy import is_gazebo_mcp_read_only, load_mcp_write_policy

        pol = load_mcp_write_policy()
        read_only = is_gazebo_mcp_read_only(tool, pol)
        extra = _readonly_transport_retries()
        max_rounds = 1 + (extra if read_only else 0)
        last_exc: Optional[BaseException] = None
        for round_idx in range(max_rounds):
            try:
                return self._invoke_tool_once(tool, args, read_only)
            except Exception as exc:
                last_exc = exc
                recoverable = (
                    read_only
                    and _recoverable_transport_error(exc)
                    and round_idx < max_rounds - 1
                )
                if not recoverable:
                    raise
                self._restart_mcp_transport(tool=tool, read_only=read_only)
        assert last_exc is not None
        raise last_exc

    def _invoke_tool_once(self, tool: str, args: Dict[str, Any], read_only: bool) -> Dict[str, Any]:
        assert self._client
        from bridge import structured_log

        t0 = time.monotonic()
        try:
            raw = self._client.call_tool(tool, args)
        except Exception as exc:
            dt_ms = (time.monotonic() - t0) * 1000.0
            structured_log.log_mcp_tool_result(
                component=self._LOG_COMPONENT,
                tool=tool,
                ok=False,
                duration_ms=dt_ms,
                message=str(exc),
                is_error=None,
                permission_gate="read_only" if read_only else "mutating",
            )
            structured_log.append_event(
                {
                    "event": "mcp_tool_exception",
                    "component": self._LOG_COMPONENT,
                    "tool": tool,
                    "exc_type": type(exc).__name__,
                    "exc_message": str(exc),
                }
            )
            raise

        dt_ms = (time.monotonic() - t0) * 1000.0
        is_err = bool(raw.get("isError")) if isinstance(raw, dict) else False
        ok = not is_err
        msg = None
        if isinstance(raw, dict):
            content = raw.get("content") or []
            if content and isinstance(content[0], dict) and content[0].get("type") == "text":
                txt = str(content[0].get("text", ""))
                if "image_base64" in txt or "rgb_base64" in txt:
                    txt = "[redacted: image/base64 payload omitted from structured logs]"
                else:
                    txt = txt[:300]
                msg = txt if txt else None
        structured_log.log_mcp_tool_result(
            component=self._LOG_COMPONENT,
            tool=tool,
            ok=ok,
            duration_ms=dt_ms,
            message=msg,
            is_error=is_err if isinstance(raw, dict) else None,
            permission_gate="read_only" if read_only else "mutating",
        )
        return raw


# ── Helper: parse tool result content ─────────────────────────────────────────

def _parse_tool_result(raw: Dict[str, Any]) -> tuple[bool, Optional[Dict], str]:
    """
    Extract (ok, data_dict, message) from a tools/call response.

    MCP tool results have a 'content' list of TextContent / ImageContent items.
    gazebo-mcp returns JSON-encoded text.
    """
    content = raw.get("content", [])
    if not content:
        return False, None, "Empty tool response"

    text = ""
    for item in content:
        if item.get("type") == "text":
            text += item.get("text", "")

    try:
        data = json.loads(text)
        ok   = data.get("success", True)  # gazebo-mcp uses "success" key
        raw_msg = data.get("message", data.get("error", text[:200]))
        msg  = "" if raw_msg is None else str(raw_msg)
        return ok, data, msg
    except (json.JSONDecodeError, AttributeError):
        # Not JSON — treat as a message string
        lower = text.lower()
        ok = "error" not in lower and "fail" not in lower
        return ok, None, text[:500]


def _parse_mcp_tool_response_media(
    raw: Dict[str, Any],
) -> Tuple[bool, Optional[Dict[str, Any]], Optional[bytes], str]:
    """
    Like ``_parse_tool_result`` but also extracts optional image bytes from
    MCP ``ImageContent`` items or common JSON keys (``image_base64``, etc.).
    """
    content = raw.get("content", [])
    if not content:
        return False, None, None, "Empty tool response"

    text_chunks: List[str] = []
    image_bytes: Optional[bytes] = None

    for item in content:
        if not isinstance(item, dict):
            continue
        itype = item.get("type")
        if itype == "text":
            text_chunks.append(str(item.get("text", "")))
        elif itype == "image":
            b64 = item.get("data")
            if not b64 and isinstance(item.get("source"), dict):
                b64 = item["source"].get("data")
            if b64:
                try:
                    image_bytes = base64.b64decode(b64)
                except Exception:
                    pass

    text = "".join(text_chunks).strip()
    data: Optional[Dict[str, Any]] = None
    msg = text[:500] if text else ""
    ok = True

    if text:
        try:
            data = json.loads(text)
            ok = bool(data.get("success", True))
            raw_msg = data.get("message", data.get("error", msg))
            msg = "" if raw_msg is None else str(raw_msg)
            if image_bytes is None and isinstance(data, dict):
                layers: List[Dict[str, Any]] = [data]
                inner = data.get("data")
                if isinstance(inner, dict):
                    layers.append(inner)
                for layer in layers:
                    for key in ("image_base64", "image_data", "rgb_base64"):
                        val = layer.get(key)
                        if isinstance(val, str) and val.strip():
                            try:
                                image_bytes = base64.b64decode(val)
                                if image_bytes:
                                    break
                            except Exception:
                                pass
                    if image_bytes:
                        break
        except json.JSONDecodeError:
            lower = text.lower()
            ok = "error" not in lower and "fail" not in lower

    return ok, data, image_bytes, msg


def _call_gazebo_tool(
    gz,
    names: Tuple[str, ...],
    args: Dict[str, Any],
) -> Dict[str, Any]:
    """Try tool names in order (short vs ``gazebo_``-prefixed registrations)."""
    last_exc: Optional[Exception] = None
    for name in names:
        try:
            return gz(name, args)
        except Exception as exc:
            last_exc = exc
            err = str(exc).lower()
            if any(s in err for s in ("unknown", "not found", "invalid tool", "no such tool")):
                continue
            raise
    if last_exc:
        raise last_exc
    raise RuntimeError("gazebo MCP tool call failed")


def pick_camera_sensor_from_mcp_list(
    data: Optional[Dict[str, Any]],
) -> Optional[str]:
    """
    Choose a sensor name for :func:`capture_camera_snapshot` when the user
    did not specify one.

    Prefers names containing ``camera``, ``rgb``, or ``image`` (case-insensitive).
    """
    names: List[str] = []

    def walk(obj: Any) -> None:
        if isinstance(obj, dict):
            n = obj.get("sensor_name") or obj.get("name")
            if isinstance(n, str) and n.strip():
                names.append(n.strip())
            for key in ("sensors", "data", "items", "results", "models", "children"):
                if key in obj:
                    walk(obj[key])
        elif isinstance(obj, list):
            for x in obj:
                walk(x)

    if data:
        walk(data)

    seen: set[str] = set()
    ordered: List[str] = []
    for n in names:
        if n not in seen:
            seen.add(n)
            ordered.append(n)

    for n in ordered:
        low = n.lower()
        if any(k in low for k in ("camera", "rgb", "image")):
            return n
    return ordered[0] if ordered else None


# ── Public API ─────────────────────────────────────────────────────────────────

def wait_for_ready(
    retries: int = 10,
    delay: float = 2.0,
    timeout: float = 15.0,
) -> GazeboResult:
    """
    Poll the gazebo-mcp server until it reports Gazebo is ready.

    Addresses friction point #7: Gazebo starts 2–5 s after the Docker container.
    This function retries get_simulation_status until it succeeds.

    Args:
        retries: Number of attempts.
        delay:   Seconds between attempts.
        timeout: Per-call MCP timeout.

    Returns:
        GazeboResult with ok=True once Gazebo is reachable.
    """
    last_msgs: List[str] = []
    for attempt in range(retries):
        try:
            with GazeboSession(timeout=timeout) as gz:
                raw = gz("gazebo_get_simulation_status", {})
                ok, data, msg = _parse_tool_result(raw)
                if ok:
                    return GazeboResult(ok=True, data=data, messages=[msg])
                last_msgs = [f"Attempt {attempt + 1}/{retries}: {msg}"]
        except Exception as exc:
            last_msgs = [f"Attempt {attempt + 1}/{retries}: {user_hint_for_gazebo_mcp_failure(exc)}"]
            logger.debug("wait_for_ready attempt %d failed: %s", attempt + 1, exc)

        if attempt < retries - 1:
            time.sleep(delay)

    return GazeboResult(
        ok=False,
        messages=last_msgs + [
            "Gazebo not ready after all retries. "
            "Run Start-gz-sim.bat and wait for the container to start."
        ],
    )


def get_simulation_status(timeout: float = 15.0) -> GazeboResult:
    """
    Single-shot simulation status from gazebo-mcp (``gazebo_get_simulation_status``).

    Used by the Simulation Workbench status panel; does not retry like
    :func:`wait_for_ready`.
    """
    try:
        with GazeboSession(timeout=timeout) as gz:
            raw = _call_gazebo_tool(
                gz,
                ("gazebo_get_simulation_status", "get_simulation_status"),
                {},
            )
            ok, data, msg = _parse_tool_result(raw)
            from bridge import structured_log

            structured_log.append_event(
                {
                    "event": "gazebo_simulation_status",
                    "component": "bridge.gazebo_bridge",
                    "ok": ok,
                    "message": (msg or "")[:500],
                }
            )
            return GazeboResult(ok=ok, data=data, messages=[msg])
    except Exception as exc:
        from bridge import structured_log

        structured_log.append_event(
            {
                "event": "gazebo_simulation_status",
                "component": "bridge.gazebo_bridge",
                "ok": False,
                "message": str(exc),
            }
        )
        return GazeboResult(ok=False, messages=[user_hint_for_gazebo_mcp_failure(exc)])


def list_gazebo_sensors(
    model_name: Optional[str] = None,
    timeout: float = 20.0,
) -> GazeboResult:
    """List sensors via gazebo-mcp (``filtered`` includes sensor *names* for auto-pick)."""
    args: Dict[str, Any] = {"response_format": "filtered"}
    if model_name:
        args["model_name"] = model_name
    try:
        with GazeboSession(timeout=timeout) as gz:
            raw = _call_gazebo_tool(
                gz,
                ("gazebo_list_sensors", "list_sensors"),
                args,
            )
            ok, data, msg = _parse_tool_result(raw)
            from bridge import structured_log

            structured_log.append_event(
                {
                    "event": "sensor_discovery",
                    "component": "bridge.gazebo_bridge",
                    "ok": ok,
                    "message": (msg or "")[:500],
                    "model_name": model_name,
                }
            )
            return GazeboResult(ok=ok, data=data, messages=[msg])
    except Exception as exc:
        from bridge import structured_log

        structured_log.append_event(
            {
                "event": "sensor_discovery",
                "component": "bridge.gazebo_bridge",
                "ok": False,
                "message": str(exc),
                "model_name": model_name,
            }
        )
        return GazeboResult(ok=False, messages=[user_hint_for_gazebo_mcp_failure(exc)])


def capture_camera_snapshot(
    sensor_name: Optional[str] = None,
    output_dir: Optional[Path] = None,
    timeout: float = 45.0,
) -> GazeboResult:
    """
    Save one camera frame from Gazebo via ``gazebo_get_sensor_data``.

    If *sensor_name* is omitted, picks the first name containing ``camera``
    (case-insensitive) from :func:`list_gazebo_sensors`, or the environment
    variable ``SIMWORKBENCH_GAZEBO_CAMERA_SENSOR``.

    Writes PNG/JPEG bytes under *output_dir* (default: ``sim_runs/screenshots/``).
    """
    out_root = output_dir or (_REPO_ROOT / "sim_runs" / "screenshots")
    out_root = ensure_allowed_write_path(out_root)
    out_root.mkdir(parents=True, exist_ok=True)

    env_sensor = os.environ.get("SIMWORKBENCH_GAZEBO_CAMERA_SENSOR", "").strip()
    chosen = (sensor_name or env_sensor or "").strip() or None

    from bridge import structured_log

    try:
        with GazeboSession(timeout=timeout) as gz:
            if not chosen:
                ls = _call_gazebo_tool(
                    gz,
                    ("gazebo_list_sensors", "list_sensors"),
                    {"response_format": "filtered"},
                )
                _ok_ls, ls_data, _msg_ls = _parse_tool_result(ls)
                chosen = pick_camera_sensor_from_mcp_list(ls_data)
                if not chosen:
                    structured_log.log_gazebo_capture(
                        component="bridge.gazebo_bridge",
                        ok=False,
                        sensor_name=None,
                        artifact_path=None,
                        message="No camera sensor name resolved",
                    )
                    return GazeboResult(
                        ok=False,
                        messages=[
                            "No camera sensor name resolved. "
                            "List sensors in your world, or set "
                            "SIMWORKBENCH_GAZEBO_CAMERA_SENSOR.",
                        ],
                    )

            raw = _call_gazebo_tool(
                gz,
                ("gazebo_get_sensor_data", "get_sensor_data"),
                {"sensor_name": chosen, "timeout": min(timeout, 60.0)},
            )
            ok, data, img_bytes, msg = _parse_mcp_tool_response_media(raw)

            def _payload_dict(d: Optional[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
                if not isinstance(d, dict):
                    return None
                inner = d.get("data")
                if isinstance(inner, dict):
                    return inner
                return d

            payload = _payload_dict(data)

            file_path: Optional[Path] = None
            if isinstance(data, dict):
                p = data.get("file_path") or data.get("path") or data.get("saved_path")
                if p:
                    fp = Path(str(p))
                    if fp.is_file():
                        file_path = fp.resolve()

            if img_bytes:
                ts = datetime.now(timezone.utc).strftime("%Y%m%d_%H%M%S")
                file_path = out_root / f"gazebo_cam_{ts}.png"
                file_path.write_bytes(img_bytes)

            if file_path and file_path.is_file():
                out_data: Dict[str, Any] = {"path": str(file_path), "sensor_name": chosen}
                if payload:
                    mode = payload.get("camera_source_mode") or payload.get("source_mode")
                    if mode:
                        out_data["camera_source_mode"] = mode
                    w = payload.get("image_width")
                    if w is None:
                        w = payload.get("width")
                    h = payload.get("image_height")
                    if h is None:
                        h = payload.get("height")
                    if w is not None:
                        out_data["image_width"] = w
                    if h is not None:
                        out_data["image_height"] = h
                    if payload.get("gz_image_topic"):
                        out_data["gz_image_topic"] = payload["gz_image_topic"]
                    if payload.get("pixel_format"):
                        out_data["pixel_format"] = payload["pixel_format"]
                structured_log.log_gazebo_capture(
                    component="bridge.gazebo_bridge",
                    ok=True,
                    sensor_name=chosen,
                    artifact_path=str(file_path),
                    camera_source_mode=out_data.get("camera_source_mode"),
                    image_width=out_data.get("image_width"),
                    image_height=out_data.get("image_height"),
                    gz_image_topic=out_data.get("gz_image_topic"),
                    message=msg or "Screenshot saved",
                )
                return GazeboResult(
                    ok=True,
                    data=out_data,
                    messages=[msg or "Screenshot saved"],
                )

            structured_log.log_gazebo_capture(
                component="bridge.gazebo_bridge",
                ok=False,
                sensor_name=chosen,
                artifact_path=None,
                message=msg or "Sensor returned no image payload",
            )
            return GazeboResult(
                ok=False,
                data=data,
                messages=[
                    msg or "Sensor returned no image payload",
                ],
            )
    except Exception as exc:
        structured_log.log_gazebo_capture(
            component="bridge.gazebo_bridge",
            ok=False,
            sensor_name=chosen,
            artifact_path=None,
            exc_type=type(exc).__name__,
            exc_message=str(exc),
        )
        return GazeboResult(ok=False, messages=[user_hint_for_gazebo_mcp_failure(exc)])


def spawn_model(
    model_name: str,
    urdf_path: Optional[Path] = None,
    sdf_content: Optional[str] = None,
    pose: Optional[Dict[str, Any]] = None,
    timeout: float = 30.0,
) -> GazeboResult:
    """
    Spawn a robot model in Gazebo.

    Accepts either a URDF file path or raw SDF/URDF XML string.
    Addresses friction point #9: gazebo-mcp's spawn_model accepts both.

    Args:
        model_name:  Gazebo model name (unique identifier in the world).
        urdf_path:   Path to a .urdf or .sdf file.
        sdf_content: Raw SDF/URDF XML string (alternative to urdf_path).
        pose:        Spawn pose dict {"position": {"x": 0, "y": 0, "z": 0},
                                      "orientation": {"roll": 0, ...}}.

    Returns:
        GazeboResult with ok=True on success.
    """
    if urdf_path is None and sdf_content is None:
        return GazeboResult(
            ok=False,
            messages=["spawn_model: provide urdf_path or sdf_content"],
        )

    args: Dict[str, Any] = {"model_name": model_name}

    if urdf_path is not None:
        # Read and pass the content — gazebo-mcp's spawn_model takes XML content
        path = Path(urdf_path)
        if not path.exists():
            return GazeboResult(ok=False, messages=[f"URDF/SDF file not found: {path}"])
        try:
            enforce_spawn_model_urdf_read(path)
        except MCPRepoReadDenied as exc:
            return GazeboResult(ok=False, messages=[str(exc)])
        args["model_xml"] = path.read_text(encoding="utf-8")
    else:
        args["model_xml"] = sdf_content

    if pose:
        pos = pose.get("position", {})
        args["x"] = float(pos.get("x", 0))
        args["y"] = float(pos.get("y", 0))
        args["z"] = float(pos.get("z", 0))
        ori = pose.get("orientation", {})
        args["roll"]  = float(ori.get("roll", 0))
        args["pitch"] = float(ori.get("pitch", 0))
        args["yaw"]   = float(ori.get("yaw", 0))

    try:
        with GazeboSession(timeout=timeout) as gz:
            raw = gz("spawn_model", args)
            ok, data, msg = _parse_tool_result(raw)
            return GazeboResult(ok=ok, data=data, messages=[msg])
    except Exception as exc:
        return GazeboResult(ok=False, messages=[user_hint_for_gazebo_mcp_failure(exc)])


def get_model_state(
    model_name: str,
    timeout: float = 15.0,
) -> GazeboResult:
    """
    Return pose + twist for a model in Gazebo.

    Args:
        model_name: Name of the model as spawned.

    Returns:
        GazeboResult with data containing 'pose' and 'twist' dicts.
    """
    try:
        with GazeboSession(timeout=timeout) as gz:
            raw = gz("get_model_state", {"model_name": model_name})
            ok, data, msg = _parse_tool_result(raw)
            return GazeboResult(ok=ok, data=data, messages=[msg])
    except Exception as exc:
        return GazeboResult(ok=False, messages=[user_hint_for_gazebo_mcp_failure(exc)])


def pause_simulation(timeout: float = 10.0) -> GazeboResult:
    """Pause Gazebo physics."""
    try:
        with GazeboSession(timeout=timeout) as gz:
            raw = gz("pause_simulation", {})
            ok, data, msg = _parse_tool_result(raw)
            return GazeboResult(ok=ok, data=data, messages=[msg])
    except Exception as exc:
        return GazeboResult(ok=False, messages=[user_hint_for_gazebo_mcp_failure(exc)])


def resume_simulation(timeout: float = 10.0) -> GazeboResult:
    """Resume Gazebo physics."""
    try:
        with GazeboSession(timeout=timeout) as gz:
            raw = gz("unpause_simulation", {})
            ok, data, msg = _parse_tool_result(raw)
            return GazeboResult(ok=ok, data=data, messages=[msg])
    except Exception as exc:
        return GazeboResult(ok=False, messages=[user_hint_for_gazebo_mcp_failure(exc)])


def reset_simulation(timeout: float = 15.0) -> GazeboResult:
    """Reset Gazebo to its initial state."""
    try:
        with GazeboSession(timeout=timeout) as gz:
            raw = gz("reset_simulation", {})
            ok, data, msg = _parse_tool_result(raw)
            return GazeboResult(ok=ok, data=data, messages=[msg])
    except Exception as exc:
        return GazeboResult(ok=False, messages=[user_hint_for_gazebo_mcp_failure(exc)])
