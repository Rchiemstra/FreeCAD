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

All functions return a GazeboResult(ok, data, messages) so callers
never need to catch MCP protocol exceptions.

BLOCKER (Phase 1): Requires Gazebo Docker to be running:
  Run Start-gz-sim.bat and wait for the container to be ready.
  Then these tools will connect via the gazebo-mcp MCP server.
"""

from __future__ import annotations

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
from typing import Any, Dict, List, Optional

logger = logging.getLogger(__name__)

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
            path = result.stdout.strip()
            # Prefer /mnt/<drive>/... over docker-desktop bind-mount hashes (stable in docker -v).
            if path and "docker-desktop-bind-mounts" not in path:
                return path
    except Exception:
        pass
    try:
        resolved = windows_path.resolve()
        drive = resolved.drive.rstrip(":").lower()
        if drive:
            rest = str(resolved)[len(resolved.drive) :].replace("\\", "/")
            return f"/mnt/{drive}{rest}"
    except Exception:
        pass
    return None


def _docker_container_running(name: str) -> bool:
    try:
        result = subprocess.run(
            [
                "wsl", "--", "bash", "-lc",
                f"docker inspect -f '{{{{.State.Running}}}}' {name} 2>/dev/null",
            ],
            capture_output=True,
            text=True,
            timeout=8,
        )
        return result.returncode == 0 and result.stdout.strip().lower() == "true"
    except Exception:
        return False


def _use_docker_mcp() -> bool:
    """Run gazebo-mcp inside ROS Docker on the ros_gz_bridge network namespace."""
    flag = os.environ.get("GAZEBO_MCP_DOCKER", "").strip().lower()
    if flag in ("0", "false", "no"):
        return False
    if flag in ("1", "true", "yes"):
        return True
    bridge = os.environ.get("ROS_GZ_BRIDGE_CONTAINER", "ros-gz-bridge")
    live = os.environ.get("RUN_GAZEBO_LIVE", "").strip().lower() in ("1", "yes", "true")
    return live and _docker_container_running(bridge)


def _build_gazebo_server_cmd() -> List[str]:
    """Start gazebo-mcp-server (stdio MCP)."""
    manual = os.environ.get("GAZEBO_MCP_CMD", "").strip()
    if manual:
        return manual.split()

    def _wsl_venv_shell(wsl_repo_path: str) -> str:
        from bridge.gazebo_lifecycle import resolve_world_name

        world = resolve_world_name()
        return (
            f"cd '{wsl_repo_path}' && "
            f"export GAZEBO_WORLD_NAME={world} && "
            f"if [ -f /opt/ros/humble/setup.bash ]; then "
            f"source /opt/ros/humble/setup.bash; fi && "
            f"exec .venv/bin/python3 -m gazebo_mcp.server"
        )

    def _wsl_docker_shell(wsl_repo_path: str, bridge_container: str) -> str:
        from bridge.gazebo_lifecycle import resolve_world_name

        world = resolve_world_name()
        gz_timeout = os.environ.get("GAZEBO_TIMEOUT", "60")
        return (
            f"docker run -i --rm --network container:{bridge_container} "
            f"-v '{wsl_repo_path}:/ws' -e PYTHONPATH=/ws/src:/ws "
            f"-e GAZEBO_WORLD_NAME={world} -e GAZEBO_TIMEOUT={gz_timeout} "
            f"osrf/ros:humble-desktop "
            f"bash -lc 'set -e; source /opt/ros/humble/setup.bash; "
            f"apt-get update -qq && apt-get install -y -qq python3-pip ros-humble-ros-gz-interfaces >/dev/null; "
            f"pip install -q mcp pydantic pydantic-settings pyyaml 2>/dev/null || true; "
            f"exec python3 -m gazebo_mcp.server'"
        )

    if sys.platform == "win32":
        wsl_path = _get_wsl_path(_GAZEBO_MCP) or str(_GAZEBO_MCP).replace("\\", "/")
        if _use_docker_mcp():
            bridge = os.environ.get("ROS_GZ_BRIDGE_CONTAINER", "ros-gz-bridge")
            return ["wsl", "--", "bash", "-c", _wsl_docker_shell(wsl_path, bridge)]
        return ["wsl", "--", "bash", "-c", _wsl_venv_shell(wsl_path)]

    # Linux / macOS
    venv_python = _GAZEBO_MCP / ".venv" / "bin" / "python3"
    if venv_python.is_file():
        return [
            "bash", "-lc",
            f"cd '{_GAZEBO_MCP}' && exec '{venv_python}' -m gazebo_mcp.server",
        ]
    return ["bash", "-lc", f"cd '{_GAZEBO_MCP}' && exec python3 -m gazebo_mcp.server"]


def get_gazebo_server_cmd() -> List[str]:
    """Resolve MCP server launch command (may depend on live Docker sidecars)."""
    return _build_gazebo_server_cmd()


# Back-compat for tests that import the constant name.
_GAZEBO_SERVER_CMD = get_gazebo_server_cmd()


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


# ── Context manager for a single Gazebo MCP session ───────────────────────────

class GazeboSession:
    """
    Context manager that starts the gazebo-mcp server subprocess and yields
    a call_tool callable.

    Usage:
        with GazeboSession() as gz:
            result = gz("spawn_model", {"model_name": "arm", ...})
    """

    def __init__(self, timeout: float = 15.0) -> None:
        self._timeout = timeout
        self._client: Optional[_MCPClient] = None

    def __enter__(self):
        cmd = get_gazebo_server_cmd()
        self._client = _MCPClient(cmd, timeout=self._timeout)
        self._client.start()
        if not self._client.is_alive():
            raise RuntimeError(
                "gazebo-mcp server process failed to start. "
                f"Command: {' '.join(cmd)}"
            )
        if not self._client.initialize():
            self._client.stop()
            raise RuntimeError("gazebo-mcp MCP initialization handshake failed")
        return self._call

    def __exit__(self, *_):
        if self._client:
            self._client.stop()

    def _call(self, tool: str, args: Dict[str, Any]) -> Dict[str, Any]:
        assert self._client
        return self._client.call_tool(tool, args)


# ── Helper: parse tool result content ─────────────────────────────────────────

def _unwrap_data(result: "GazeboResult | Dict[str, Any]") -> Dict[str, Any]:
    """Return a plain dict from GazeboResult or pass through dict-like data."""
    if isinstance(result, GazeboResult):
        if not result.ok:
            raise RuntimeError("; ".join(result.messages) or "Gazebo bridge call failed")
        return result.data or {}
    if isinstance(result, dict):
        return result
    return {}


def _model_names_from_list_data(data: Optional[Dict[str, Any]]) -> List[str]:
    """Extract model names from gazebo_list_models response data."""
    if not data:
        return []
    models = data.get("models")
    if isinstance(models, list):
        return [str(m.get("name", "")) for m in models if m.get("name")]
    return []


def _status_payload(envelope: Optional[Dict[str, Any]]) -> Optional[Dict[str, Any]]:
    """Return the inner simulation status dict from a gazebo-mcp tool envelope."""
    if not envelope:
        return None
    payload = envelope.get("data")
    return payload if isinstance(payload, dict) else None


def _gazebo_connected(envelope: Optional[Dict[str, Any]]) -> Optional[bool]:
    """True/False when status reports connection; None if field absent."""
    payload = _status_payload(envelope)
    if payload is None or "gazebo_connected" not in payload:
        return None
    return bool(payload.get("gazebo_connected"))


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
        msg  = data.get("message", data.get("error", text[:200]))
        return ok, data, msg
    except (json.JSONDecodeError, AttributeError):
        # Not JSON — treat as a message string
        lower = text.lower()
        ok = "error" not in lower and "fail" not in lower
        return ok, None, text[:500]


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
                connected = _gazebo_connected(data)
                if ok and connected is not False:
                    return GazeboResult(ok=True, data=data, messages=[msg])
                if ok and connected is False:
                    note = (_status_payload(data) or {}).get("note", "Gazebo not connected")
                    last_msgs = [
                        f"Attempt {attempt + 1}/{retries}: MCP up but {note}. "
                        "Run scripts/ensure_ros_gz_bridge.sh after Start-gz-sim.bat."
                    ]
                else:
                    last_msgs = [f"Attempt {attempt + 1}/{retries}: {msg}"]
        except Exception as exc:
            last_msgs = [f"Attempt {attempt + 1}/{retries}: {exc}"]
            logger.debug("wait_for_ready attempt %d failed: %s", attempt + 1, exc)

        if attempt < retries - 1:
            time.sleep(delay)

    return GazeboResult(
        ok=False,
        messages=last_msgs + [
            "GAZEBO_NOT_RUNNING: Gazebo not ready after all retries. "
            "Run Start-gz-sim.bat and wait 2–5 s for the container to start. "
            "Live pytest: set RUN_GAZEBO_LIVE=1."
        ],
    )


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
    from bridge.permissions import PermissionDenied, WriteOperation, assert_write_allowed

    try:
        assert_write_allowed(WriteOperation.GAZEBO_SPAWN)
    except PermissionDenied as exc:
        return GazeboResult(ok=False, messages=[str(exc)])

    if urdf_path is None and sdf_content is None:
        return GazeboResult(
            ok=False,
            messages=["spawn_model: provide urdf_path or sdf_content"],
        )

    path: Optional[Path] = None
    if urdf_path is not None:
        path = Path(urdf_path)
        if not path.exists():
            return GazeboResult(ok=False, messages=[f"URDF/SDF file not found: {path}"])
        from bridge.urdf_for_gazebo import prepare_urdf_for_gazebo

        raw_xml = path.read_text(encoding="utf-8")
        xml_content = prepare_urdf_for_gazebo(raw_xml, path)
    else:
        xml_content = sdf_content or ""

    from bridge.gazebo_gz_docker import spawn_prepared_xml, use_gz_docker_spawn

    if use_gz_docker_spawn() and urdf_path is not None:
        return spawn_prepared_xml(
            model_name,
            xml_content,
            host_urdf_path=path,
            pose=pose,
            timeout=timeout,
        )

    # Unpause physics before spawn (headless gz sim starts paused without -r on some builds).
    resume = resume_simulation(timeout=min(timeout, 15.0))
    if not resume.ok:
        logger.warning("resume_simulation before spawn: %s", resume.messages)

    args: Dict[str, Any] = {
        "entity_name": model_name,
        "sdf_xml": xml_content,
        "x": 0.0,
        "y": 0.0,
        "z": 0.0,
        "roll": 0.0,
        "pitch": 0.0,
        "yaw": 0.0,
    }

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
            raw = gz("gazebo_spawn_sdf", args)
            ok, data, msg = _parse_tool_result(raw)
            if ok:
                from bridge.run_context import record_event, record_path

                if urdf_path is not None:
                    record_path("spawn_urdf", urdf_path)
                record_event(
                    "gazebo",
                    "spawn_model",
                    model=model_name,
                    via="gazebo_mcp",
                )
            return GazeboResult(ok=ok, data=data, messages=[msg])
    except Exception as exc:
        return GazeboResult(ok=False, messages=[str(exc)])


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
            raw = gz("gazebo_get_model_state", {"model_name": model_name})
            ok, data, msg = _parse_tool_result(raw)
            return GazeboResult(ok=ok, data=data, messages=[msg])
    except Exception as exc:
        return GazeboResult(ok=False, messages=[str(exc)])


def get_simulation_status(timeout: float = 15.0) -> GazeboResult:
    """Return overall Gazebo simulation status from gazebo-mcp."""
    try:
        with GazeboSession(timeout=timeout) as gz:
            raw = gz("gazebo_get_simulation_status", {})
            ok, data, msg = _parse_tool_result(raw)
            return GazeboResult(ok=ok, data=data, messages=[msg])
    except Exception as exc:
        return GazeboResult(ok=False, messages=[str(exc)])


def list_models(timeout: float = 15.0) -> List[str]:
    """
    Return model names in the running simulation.

    Raises RuntimeError when the MCP call fails. Returns [] when the world is empty.
    """
    try:
        with GazeboSession(timeout=timeout) as gz:
            raw = gz("gazebo_list_models", {"response_format": "concise"})
            ok, data, msg = _parse_tool_result(raw)
            if not ok:
                raise RuntimeError(msg)
            return _model_names_from_list_data(data)
    except Exception as exc:
        raise RuntimeError(str(exc)) from exc


def pause_simulation(timeout: float = 10.0) -> GazeboResult:
    """Pause Gazebo physics."""
    from bridge.permissions import PermissionDenied, WriteOperation, assert_write_allowed

    try:
        assert_write_allowed(WriteOperation.GAZEBO_SIM_CONTROL)
    except PermissionDenied as exc:
        return GazeboResult(ok=False, messages=[str(exc)])

    try:
        with GazeboSession(timeout=timeout) as gz:
            raw = gz("gazebo_pause_simulation", {})
            ok, data, msg = _parse_tool_result(raw)
            return GazeboResult(ok=ok, data=data, messages=[msg])
    except Exception as exc:
        return GazeboResult(ok=False, messages=[str(exc)])


def resume_simulation(timeout: float = 10.0) -> GazeboResult:
    """Resume Gazebo physics."""
    from bridge.permissions import PermissionDenied, WriteOperation, assert_write_allowed

    try:
        assert_write_allowed(WriteOperation.GAZEBO_SIM_CONTROL)
    except PermissionDenied as exc:
        return GazeboResult(ok=False, messages=[str(exc)])

    try:
        with GazeboSession(timeout=timeout) as gz:
            raw = gz("gazebo_unpause_simulation", {})
            ok, data, msg = _parse_tool_result(raw)
            return GazeboResult(ok=ok, data=data, messages=[msg])
    except Exception as exc:
        return GazeboResult(ok=False, messages=[str(exc)])


def reset_simulation(timeout: float = 15.0) -> GazeboResult:
    """Reset Gazebo to its initial state."""
    from bridge.permissions import PermissionDenied, WriteOperation, assert_write_allowed

    try:
        assert_write_allowed(WriteOperation.GAZEBO_SIM_CONTROL)
    except PermissionDenied as exc:
        return GazeboResult(ok=False, messages=[str(exc)])

    try:
        with GazeboSession(timeout=timeout) as gz:
            raw = gz("gazebo_reset_simulation", {})
            ok, data, msg = _parse_tool_result(raw)
            return GazeboResult(ok=ok, data=data, messages=[msg])
    except Exception as exc:
        return GazeboResult(ok=False, messages=[str(exc)])
