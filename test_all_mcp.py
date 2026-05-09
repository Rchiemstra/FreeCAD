#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""
test_all_mcp.py  –  Comprehensive MCP server test script

Tests all three MCP servers in this repository:
  1. FreeCAD MCP  – connects to FreeCAD via XML-RPC (runs on Windows)
  2. Gazebo MCP   – connects to Gazebo via ROS2/WSL
  3. ROS MCP      – connects to ROS via rosbridge WebSocket/WSL

The batch files  Start-FreeCAD.bat / Start-gz-sim.bat / Start-ros2.bat
are used (optionally) to boot the backing applications before testing.

Usage
-----
    python test_all_mcp.py [options]

Options
-------
    --start-apps         Launch FreeCAD, Gazebo and ROS2 before testing
    --startup-wait N     Seconds to wait after launching apps (default 20)
    --no-freecad         Skip FreeCAD MCP tests
    --no-gazebo          Skip Gazebo  MCP tests
    --no-ros             Skip ROS     MCP tests
    --timeout  N         MCP server response timeout in seconds (default 15)
    --verbose            Print full JSON-RPC messages
    --cmd-agent          After basic tests, start an AI coding-agent session
                         (prompts the user to interact with the MCP server)
"""

from __future__ import annotations

import argparse
import json
import os
import queue
import subprocess
import sys
import textwrap
import threading
import time
import xmlrpc.client
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional, Tuple

# Ensure UTF-8 output on Windows consoles so emoji / box-drawing chars work.
if sys.platform == "win32" and hasattr(sys.stdout, "reconfigure"):
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    sys.stderr.reconfigure(encoding="utf-8", errors="replace")

# ─── Paths ────────────────────────────────────────────────────────────────────
REPO_ROOT    = Path(__file__).parent.resolve()
FREECAD_MCP  = REPO_ROOT / "tools" / "mcp" / "freecad-mcp"
GAZEBO_MCP   = REPO_ROOT / "tools" / "mcp" / "gazebo-mcp"
ROS_MCP      = REPO_ROOT / "tools" / "mcp" / "ros-mcp-server"

# ─── Ports ────────────────────────────────────────────────────────────────────
FREECAD_RPC_PORT  = 9875   # XML-RPC port that the FreeCAD addon listens on
ROSBRIDGE_PORT    = 9090   # rosbridge WebSocket port
ROS_MCP_HTTP_PORT = 9000   # ROS MCP HTTP transport port (alternative)

# ─── Visual markers ───────────────────────────────────────────────────────────
PASS = "✅ PASS"
FAIL = "❌ FAIL"
SKIP = "⏭  SKIP"
WARN = "⚠️  WARN"
INFO = "ℹ️  INFO"


# ══════════════════════════════════════════════════════════════════════════════
#  Result tracking
# ══════════════════════════════════════════════════════════════════════════════

@dataclass
class TestResult:
    name:    str
    status:  str          # PASS / FAIL / SKIP / WARN
    detail:  str = ""

@dataclass
class Suite:
    name:    str
    results: List[TestResult] = field(default_factory=list)

    def add(self, name: str, status: str, detail: str = "") -> None:
        r = TestResult(name, status, detail)
        self.results.append(r)
        icon  = r.status
        extra = f"  → {r.detail}" if r.detail else ""
        print(f"  {icon}  {r.name}{extra}")

    @property
    def passed(self)  -> int: return sum(r.status == PASS for r in self.results)
    @property
    def failed(self)  -> int: return sum(r.status == FAIL for r in self.results)
    @property
    def skipped(self) -> int: return sum(r.status == SKIP for r in self.results)


# ══════════════════════════════════════════════════════════════════════════════
#  Low-level stdio MCP client  (JSON-RPC 2.0 over stdin / stdout)
# ══════════════════════════════════════════════════════════════════════════════

class MCPClientStdio:
    """
    Thin synchronous wrapper around an MCP server subprocess.

    MCP over stdio uses newline-delimited JSON-RPC 2.0 messages.
    Some SDKs (mcp >= 1.x / FastMCP 2.x) also support a
    Content-Length framed variant.  This client tries the
    newline-delimited variant first and falls back to Content-Length.
    """

    def __init__(
        self,
        cmd:     List[str],
        cwd:     Optional[Path] = None,
        env:     Optional[Dict[str, str]] = None,
        timeout: float = 15.0,
        verbose: bool  = False,
    ) -> None:
        self._cmd     = cmd
        self._cwd     = cwd
        self._env     = env
        self._timeout = timeout
        self._verbose = verbose
        self._proc:   Optional[subprocess.Popen] = None
        self._rq:     queue.Queue = queue.Queue()
        self._reader: Optional[threading.Thread] = None
        self._next_id = 1
        self._initialized = False
        self.stderr_lines: List[str] = []          # captured from server stderr

    # ── lifecycle ─────────────────────────────────────────────────────────────
    def start(self) -> None:
        env = os.environ.copy()
        if self._env:
            env.update(self._env)
        self._proc = subprocess.Popen(
            self._cmd,
            stdin  = subprocess.PIPE,
            stdout = subprocess.PIPE,
            stderr = subprocess.PIPE,
            text   = False,   # binary — _read_loop handles framing + decode
            cwd    = str(self._cwd) if self._cwd else None,
            env    = env,
        )
        self._reader = threading.Thread(target=self._read_loop, daemon=True)
        self._reader.start()
        self._stderr_reader = threading.Thread(target=self._stderr_loop, daemon=True)
        self._stderr_reader.start()
        # Give the server a moment to print any startup messages
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

    # ── I/O ───────────────────────────────────────────────────────────────────
    def _read_loop(self) -> None:
        """
        Background thread: read messages from server stdout (binary pipe).

        MCP SDK 1.x uses newline-delimited JSON (one JSON object per line).
        Also handles Content-Length framing for compatibility.
        """
        assert self._proc and self._proc.stdout
        stdout = self._proc.stdout  # raw binary pipe

        partial = b""
        while True:
            line = stdout.readline()
            if not line:
                break  # EOF
            partial += line

            # ── Content-Length framing ────────────────────────────────────
            if b"\r\n\r\n" in partial:
                header_raw, body_start = partial.split(b"\r\n\r\n", 1)
                content_length = None
                for hdr in header_raw.split(b"\r\n"):
                    if hdr.lower().startswith(b"content-length:"):
                        try:
                            content_length = int(hdr.split(b":", 1)[1].strip())
                        except ValueError:
                            pass
                if content_length is not None:
                    # Read remaining body bytes if needed
                    while len(body_start) < content_length:
                        chunk = stdout.read(content_length - len(body_start))
                        if not chunk:
                            break
                        body_start += chunk
                    body = body_start[:content_length]
                    partial = body_start[content_length:]
                    try:
                        msg = json.loads(body.decode("utf-8", errors="replace"))
                        if self._verbose:
                            print(f"    <- {json.dumps(msg, indent=2)}")
                        self._rq.put(msg)
                    except json.JSONDecodeError:
                        pass
                    continue
                partial = b""  # malformed header, reset

            # ── Newline-delimited JSON ────────────────────────────────────
            stripped = partial.strip()
            if stripped and not stripped.lower().startswith(b"content-length:"):
                try:
                    msg = json.loads(stripped.decode("utf-8", errors="replace"))
                    if self._verbose:
                        print(f"    <- {json.dumps(msg, indent=2)}")
                    self._rq.put(msg)
                except json.JSONDecodeError:
                    pass  # Not JSON — startup log line, etc.
            partial = b""

    def _stderr_loop(self) -> None:
        """Background thread: capture stderr lines for diagnostics."""
        assert self._proc and self._proc.stderr
        for raw_line in self._proc.stderr:
            line = raw_line.rstrip(b"\r\n").decode("utf-8", errors="replace")
            self.stderr_lines.append(line)
            if self._verbose:
                print(f"    [stderr] {line}")

    def _send(self, msg: Dict[str, Any]) -> None:
        """Send a JSON-RPC message using newline-delimited format (works with MCP SDK 1.x)."""
        assert self._proc and self._proc.stdin
        payload = (json.dumps(msg) + "\n").encode("utf-8")
        if self._verbose:
            print(f"    → {json.dumps(msg, indent=2)}")
        self._proc.stdin.write(payload)
        self._proc.stdin.flush()

    def _recv(self, req_id: Optional[int] = None) -> Optional[Dict[str, Any]]:
        """Wait for a response matching *req_id* (or any message if None)."""
        deadline = time.monotonic() + self._timeout
        while time.monotonic() < deadline:
            remaining = deadline - time.monotonic()
            try:
                msg = self._rq.get(timeout=min(remaining, 1.0))
                if req_id is None:
                    return msg
                if msg.get("id") == req_id:
                    return msg
                # Re-queue messages that don't match
                self._rq.put(msg)
            except queue.Empty:
                if not self.is_alive():
                    return None
        return None

    # ── MCP protocol helpers ──────────────────────────────────────────────────
    def _next(self) -> int:
        i = self._next_id
        self._next_id += 1
        return i

    def initialize(self) -> Optional[Dict[str, Any]]:
        req_id = self._next()
        self._send({
            "jsonrpc": "2.0",
            "id": req_id,
            "method": "initialize",
            "params": {
                "protocolVersion": "2024-11-05",
                "capabilities": {},
                "clientInfo": {"name": "mcp-test-client", "version": "1.0.0"},
            },
        })
        resp = self._recv(req_id)
        if resp and "result" in resp:
            # Send the required initialized notification
            self._send({"jsonrpc": "2.0", "method": "notifications/initialized", "params": {}})
            self._initialized = True
        return resp

    def list_tools(self) -> Optional[Dict[str, Any]]:
        req_id = self._next()
        self._send({"jsonrpc": "2.0", "id": req_id, "method": "tools/list", "params": {}})
        return self._recv(req_id)

    def call_tool(self, name: str, arguments: Dict[str, Any] = None) -> Optional[Dict[str, Any]]:
        req_id = self._next()
        self._send({
            "jsonrpc": "2.0",
            "id": req_id,
            "method": "tools/call",
            "params": {"name": name, "arguments": arguments or {}},
        })
        return self._recv(req_id)

    # Context-manager support
    def __enter__(self):
        self.start()
        return self

    def __exit__(self, *_):
        self.stop()


# ══════════════════════════════════════════════════════════════════════════════
#  Utility helpers
# ══════════════════════════════════════════════════════════════════════════════

def get_wsl_path(windows_path: Path) -> Optional[str]:
    """Convert a Windows path to a WSL (Linux) path."""
    try:
        # Use bash -c so the path string is passed through the shell quoted,
        # preventing PowerShell/cmd from stripping backslashes.
        win_str = str(windows_path).replace("\\", "/")
        result = subprocess.run(
            ["wsl", "--", "bash", "-c", f"wslpath -a '{win_str}'"],
            capture_output=True, text=True, timeout=10
        )
        if result.returncode == 0:
            return result.stdout.strip()
    except Exception:
        pass
    return None


def wsl_available() -> bool:
    """Return True if wsl.exe is on PATH."""
    try:
        subprocess.run(["wsl", "--status"], capture_output=True, timeout=10)
        return True
    except Exception:
        return False


def ros2_available_in_wsl() -> bool:
    """Return True if a ROS2 installation is found in WSL (/opt/ros or ros2 command)."""
    try:
        r = subprocess.run(
            ["wsl", "bash", "-c",
             "ls /opt/ros/ 2>/dev/null | grep -q . || command -v ros2 >/dev/null 2>&1 && echo ok"],
            capture_output=True, text=True, timeout=10
        )
        return r.stdout.strip() == "ok"
    except Exception:
        return False


def freecad_rpc_alive(host: str = "localhost", port: int = FREECAD_RPC_PORT) -> bool:
    """Return True if the FreeCAD XML-RPC server responds to ping()."""
    try:
        client = xmlrpc.client.ServerProxy(f"http://{host}:{port}", allow_none=True)
        return bool(client.ping())
    except Exception:
        return False


def tool_names_from_response(resp: Dict[str, Any]) -> List[str]:
    """Extract tool names from a tools/list JSON-RPC response."""
    try:
        return [t["name"] for t in resp["result"]["tools"]]
    except (KeyError, TypeError):
        return []


def find_cmd(*candidates: str) -> Optional[str]:
    """Return the first executable found on PATH."""
    import shutil
    for c in candidates:
        found = shutil.which(c)
        if found:
            return found
    return None


def start_app(bat: Path, label: str) -> None:
    """Launch a batch file in its own window (non-blocking)."""
    print(f"  {INFO}  Starting {label} via {bat.name} …")
    subprocess.Popen(
        ["cmd", "/c", str(bat)],
        creationflags=subprocess.CREATE_NEW_CONSOLE,
        cwd=str(REPO_ROOT),
    )


def ensure_wsl_venv(wsl_pkg_dir: str, label: str, install_extras: str = "") -> Optional[str]:
    """
    Ensure a Python venv exists at {wsl_pkg_dir}/.venv with the package installed.

    Returns the path to the python executable inside the venv (WSL path),
    or None if setup failed.

    Works around missing python3-venv/ensurepip on Ubuntu by using
    --without-pip and bootstrapping pip via get-pip.py.
    pip install -e . is always run (it's idempotent: fast when already installed).
    """
    venv_python = f"{wsl_pkg_dir}/.venv/bin/python3"
    venv_pip    = f"{wsl_pkg_dir}/.venv/bin/pip"

    # Step 1: create venv if python binary is missing
    has_py = subprocess.run(
        ["wsl", "bash", "-c", f"test -x '{venv_python}' && echo ok"],
        capture_output=True, text=True, timeout=10
    )
    if has_py.stdout.strip() != "ok":
        print(f"  {INFO}  Creating WSL venv for {label} …")
        r = subprocess.run(
            ["wsl", "bash", "-c",
             f"cd '{wsl_pkg_dir}' && "
             f"(python3 -m venv .venv 2>/dev/null || python3 -m venv --without-pip .venv)"],
            capture_output=True, text=True, timeout=60
        )
        if r.returncode != 0:
            print(f"  {FAIL}  venv creation failed: {r.stderr[-300:]}")
            return None

    # Step 2: bootstrap pip if pip binary is missing
    has_pip = subprocess.run(
        ["wsl", "bash", "-c", f"test -x '{venv_pip}' && echo ok"],
        capture_output=True, text=True, timeout=10
    )
    if has_pip.stdout.strip() != "ok":
        print(f"  {INFO}  Bootstrapping pip via get-pip.py …")
        r = subprocess.run(
            ["wsl", "bash", "-c",
             f"curl -sS https://bootstrap.pypa.io/get-pip.py | '{venv_python}'"],
            capture_output=True, text=True, timeout=120
        )
        if r.returncode != 0:
            print(f"  {FAIL}  pip bootstrap failed: {r.stderr[-300:]}")
            return None
        print(f"  {PASS}  pip bootstrapped")

    # Step 3: install the package (idempotent: fast if already installed)
    print(f"  {INFO}  Ensuring {label} is installed (pip install -e .) …")
    pip_cmd = f"'{venv_pip}' install -e . {install_extras} -q"
    r = subprocess.run(
        ["wsl", "bash", "-c", f"cd '{wsl_pkg_dir}' && {pip_cmd}"],
        capture_output=True, text=True, timeout=300
    )
    if r.returncode != 0:
        print(f"  {FAIL}  pip install failed: {r.stderr[-500:]}")
        return None

    print(f"  {PASS}  {label} ready in WSL venv")
    return venv_python



# ══════════════════════════════════════════════════════════════════════════════
#  FreeCAD MCP tests
# ══════════════════════════════════════════════════════════════════════════════

# Tools that the FreeCAD MCP server must expose
FREECAD_EXPECTED_TOOLS = {
    "create_document", "create_object", "edit_object", "delete_object",
    "execute_code", "get_view", "insert_part_from_library",
    "get_objects", "get_object", "get_parts_list", "list_documents",
}


def _freecad_server_cmd(wsl_root: Optional[str] = None) -> Optional[List[str]]:
    """
    Build the command to start the FreeCAD MCP server.

    Tries (in order):
      1. Installed entry-point 'freecad-mcp' on Windows PATH
      2. uv run (Windows)
      3. WSL venv (Python 3.12 in WSL)
      4. Windows python -m (if Python >= 3.12 is available)
    """
    # 1. Installed entry-point on Windows
    ep = find_cmd("freecad-mcp")
    if ep:
        return [ep]

    # 2. uv run on Windows
    uv = find_cmd("uv")
    if uv:
        return [uv, "run", "--directory", str(FREECAD_MCP), "freecad-mcp"]

    # 3. WSL venv (Python 3.12)
    if wsl_root and wsl_available():
        freecad_wsl = f"{wsl_root}/tools/mcp/freecad-mcp"
        venv_python = ensure_wsl_venv(freecad_wsl, "freecad-mcp")
        if venv_python:
            # Use the entry-point script, not python -m (server.py has no __main__ guard)
            venv_bin = venv_python.rsplit("/", 1)[0]  # e.g. .../.venv/bin
            freecad_ep = f"{venv_bin}/freecad-mcp"
            return [
                "wsl", "--", "bash", "-c",
                f"PYTHONUNBUFFERED=1 '{freecad_ep}'",
            ]

    # 4. Windows python -m (check version)
    python = find_cmd("python", "python3")
    if python:
        try:
            r = subprocess.run(
                [python, "-c", "import sys; exit(0 if sys.version_info >= (3,12) else 1)"],
                capture_output=True, timeout=5
            )
            if r.returncode == 0:
                return [python, "-m", "freecad_mcp.server"]
        except Exception:
            pass

    return None


def _try_install_freecad_mcp_deps(wsl_root: Optional[str] = None) -> bool:
    """Attempt to install freecad-mcp. Prefers WSL (Python 3.12). Returns True on success."""
    # Try WSL first (has Python 3.12)
    if wsl_root and wsl_available():
        freecad_wsl = f"{wsl_root}/tools/mcp/freecad-mcp"
        venv_python = ensure_wsl_venv(freecad_wsl, "freecad-mcp")
        return venv_python is not None

    # Try Windows pip (may fail due to Python version)
    python = find_cmd("python", "python3")
    if python:
        print(f"  {INFO}  Attempting 'pip install -e .' for freecad-mcp …")
        result = subprocess.run(
            [python, "-m", "pip", "install", "-e", "."],
            cwd=str(FREECAD_MCP),
            capture_output=True, text=True
        )
        if result.returncode == 0:
            print(f"  {PASS}  pip install succeeded")
            return True
        print(f"  {FAIL}  pip install failed: {result.stderr[-300:]}")

    return False


def test_freecad_mcp(suite: Suite, timeout: float, verbose: bool,
                    wsl_root: Optional[str] = None) -> None:
    """Run all FreeCAD MCP tests."""
    cmd = _freecad_server_cmd(wsl_root)
    if cmd is None:
        suite.add(
            "FreeCAD MCP server command", SKIP,
            "No freecad-mcp found. Install via: "
            "cd tools/mcp/freecad-mcp && pip install -e .  "
            "(requires Python >=3.12)"
        )
        return

    # For WSL-launched server, set a slightly longer startup time
    startup_wait = 3.0 if "wsl" in cmd[0].lower() else 1.5

    try:
        with MCPClientStdio(cmd, timeout=timeout, verbose=verbose) as client:
            # ── 1. Server starts ────────────────────────────────────────────
            time.sleep(startup_wait)
            if not client.is_alive():
                stderr = "\n".join(client.stderr_lines[-10:])
                if "ModuleNotFoundError" in stderr or "No module named" in stderr:
                    print(f"  {WARN}  Missing Python dependencies detected.")
                    if _try_install_freecad_mcp_deps(wsl_root):
                        suite.add("FreeCAD MCP deps installed", PASS,
                                  "Re-run the test to use the installed server")
                        suite.add("FreeCAD MCP server starts", SKIP,
                                  "Restart the test after install")
                    else:
                        suite.add(
                            "FreeCAD MCP server starts", FAIL,
                            "Missing deps, install failed. "
                            "Run: cd tools/mcp/freecad-mcp && pip install -e . "
                            "(Python >=3.12 required)"
                        )
                else:
                    suite.add(
                        "FreeCAD MCP server starts", FAIL,
                        f"Process exited – stderr: {stderr[:200] or '(empty)'}"
                    )
                return
            suite.add("FreeCAD MCP server starts", PASS)

            # ── 2. MCP initialize ───────────────────────────────────────────
            resp = client.initialize()
            if resp and "result" in resp:
                version = resp["result"].get("serverInfo", {}).get("version", "?")
                suite.add("MCP initialize", PASS, f"serverInfo version={version}")
            else:
                suite.add("MCP initialize", FAIL, f"Got: {resp}")
                return

            # ── 3. tools/list ───────────────────────────────────────────────
            resp = client.list_tools()
            if resp and "result" in resp:
                names = set(tool_names_from_response(resp))
                missing = FREECAD_EXPECTED_TOOLS - names
                extra   = names - FREECAD_EXPECTED_TOOLS
                if not missing:
                    suite.add(
                        "tools/list – expected tools present", PASS,
                        f"{len(names)} tools registered"
                    )
                else:
                    suite.add(
                        "tools/list – expected tools present", WARN,
                        f"Missing: {sorted(missing)}"
                    )
                if extra:
                    suite.add("tools/list – extra tools", INFO, f"{sorted(extra)}")
            else:
                suite.add("tools/list", FAIL, f"Got: {resp}")
                return

            # ── 4. Integration: FreeCAD RPC reachable? ──────────────────────
            rpc_up = freecad_rpc_alive()
            if not rpc_up:
                suite.add(
                    "FreeCAD RPC server reachable",
                    WARN,
                    f"Port {FREECAD_RPC_PORT} not answering – start FreeCAD + addon first"
                )
                # Still test the tool call – expect a connection-error response
                resp = client.call_tool("list_documents")
                if resp:
                    if "error" in resp or (
                        "result" in resp
                        and any(
                            "error" in str(c).lower() or "fail" in str(c).lower()
                            for c in resp["result"].get("content", [])
                        )
                    ):
                        suite.add(
                            "list_documents (no FreeCAD)", PASS,
                            "Server returned expected error/failure response"
                        )
                    else:
                        suite.add("list_documents (no FreeCAD)", WARN, f"Unexpected: {resp}")
                else:
                    suite.add("list_documents (no FreeCAD)", FAIL, "No response from server")
                return

            # FreeCAD is running – run integration tests
            suite.add("FreeCAD RPC server reachable", PASS)

            resp = client.call_tool("list_documents")
            _assert_tool_ok(suite, resp, "list_documents")

            resp = client.call_tool("create_document", {"name": "MCPTest"})
            _assert_tool_ok(suite, resp, "create_document MCPTest")

            resp = client.call_tool("create_object", {
                "doc_name": "MCPTest",
                "obj_type": "Part::Box",
                "obj_name": "TestBox",
                "obj_properties": {"Length": 20, "Width": 15, "Height": 10},
            })
            _assert_tool_ok(suite, resp, "create_object Part::Box")

            resp = client.call_tool("execute_code", {
                "code": "import FreeCAD; print(FreeCAD.Version())"
            })
            _assert_tool_ok(suite, resp, "execute_code")

            resp = client.call_tool("delete_object", {
                "doc_name": "MCPTest",
                "obj_name": "TestBox"
            })
            _assert_tool_ok(suite, resp, "delete_object")

    except Exception as exc:
        suite.add("FreeCAD MCP test session", FAIL, str(exc))


# ══════════════════════════════════════════════════════════════════════════════
#  Gazebo MCP tests
# ══════════════════════════════════════════════════════════════════════════════

GAZEBO_EXPECTED_TOOLS = {
    "gazebo_list_models", "gazebo_spawn_model", "gazebo_delete_model",
    "gazebo_get_model_state", "gazebo_set_model_state", "gazebo_apply_force",
    "gazebo_pause_simulation", "gazebo_unpause_simulation", "gazebo_reset_simulation",
}


def _install_geometry_msgs_stub(wsl_venv_python: str) -> bool:
    """
    Install a minimal stub geometry_msgs package into the WSL venv.

    This lets the Gazebo MCP server import successfully without a full ROS2
    installation.  Protocol-level tests (initialize, tools/list) work; tool
    calls that actually drive Gazebo need ROS2 at runtime and will return errors.
    """
    stub_script = textwrap.dedent("""\
        import os, sys
        for p in sys.path:
            if 'site-packages' in p and '.venv' in p:
                gm     = os.path.join(p, 'geometry_msgs')
                gm_msg = os.path.join(gm, 'msg')
                os.makedirs(gm_msg, exist_ok=True)
                open(os.path.join(gm, '__init__.py'), 'w').close()
                with open(os.path.join(gm_msg, '__init__.py'), 'w') as fh:
                    fh.write(
                        'class _M:\\n'
                        '    def __init__(self, **k):\\n'
                        '        for a, b in k.items(): setattr(self, a, b)\\n'
                        'class Pose(_M): pass\\n'
                        'class Twist(_M): pass\\n'
                        'class Vector3(_M): pass\\n'
                        'class Quaternion(_M): pass\\n'
                        'class Point(_M): pass\\n'
                        'class Wrench(_M): pass\\n'
                        'class Transform(_M): pass\\n'
                        'class PoseStamped(_M): pass\\n'
                        'class TwistStamped(_M): pass\\n'
                    )
                print('stub installed at', p)
                raise SystemExit(0)
        raise SystemExit(1)
    """)
    r = subprocess.run(
        ["wsl", "--", wsl_venv_python, "-"],
        input=stub_script,
        capture_output=True, text=True, timeout=15,
    )
    if r.returncode == 0:
        print(f"  {PASS}  geometry_msgs stub: {r.stdout.strip()}")
        return True
    print(f"  {WARN}  geometry_msgs stub install failed: {r.stderr.strip()[:120]}")
    return False


def _gazebo_server_cmd(venv_python: str, gazebo_wsl: str) -> List[str]:
    """Build the WSL command to start the Gazebo MCP server via venv entry-point."""
    venv_bin  = venv_python.rsplit("/", 1)[0]
    gazebo_ep = f"{venv_bin}/gazebo-mcp-server"
    # cd into the package root so that relative path resolution inside server.py works
    return [
        "wsl", "--", "bash", "-c",
        f"cd '{gazebo_wsl}' && PYTHONUNBUFFERED=1 '{gazebo_ep}'",
    ]


def test_gazebo_mcp(suite: Suite, timeout: float, verbose: bool) -> None:
    """Run all Gazebo MCP tests."""
    if not wsl_available():
        suite.add("WSL available", SKIP, "wsl.exe not found")
        return

    wsl_root = get_wsl_path(REPO_ROOT)
    if not wsl_root:
        suite.add("Repo path accessible from WSL", SKIP, "wslpath conversion failed")
        return

    # Auto-setup venv if needed (geometry_msgs is not pip-installable; we stub it below)
    gazebo_wsl = f"{wsl_root}/tools/mcp/gazebo-mcp"
    venv_python = ensure_wsl_venv(gazebo_wsl, "gazebo-mcp")
    if venv_python is None:
        suite.add(
            "Gazebo MCP deps", SKIP,
            f"Could not install gazebo-mcp in WSL venv. "
            f"Run manually: wsl; cd {gazebo_wsl}; "
            f"python3 -m venv .venv && .venv/bin/pip install -e ."
        )
        return

    # Install a geometry_msgs stub so the server can import without ROS2.
    # Actual tool calls to Gazebo/ROS will return runtime errors (expected).
    _install_geometry_msgs_stub(venv_python)

    cmd = _gazebo_server_cmd(venv_python, gazebo_wsl)

    try:
        with MCPClientStdio(cmd, timeout=timeout, verbose=verbose) as client:
            time.sleep(2)  # Give the server time to start
            if not client.is_alive():
                stderr = "\n".join(client.stderr_lines[-15:])
                suite.add(
                    "Gazebo MCP server starts", FAIL,
                    f"Process exited. stderr: {stderr[:300] or '(empty)'}"
                )
                return
            suite.add("Gazebo MCP server starts", PASS)

            resp = client.initialize()
            if resp and "result" in resp:
                suite.add("MCP initialize", PASS)
            else:
                suite.add("MCP initialize", FAIL, f"Got: {resp}")
                return

            resp = client.list_tools()
            if resp and "result" in resp:
                names = set(tool_names_from_response(resp))
                missing = GAZEBO_EXPECTED_TOOLS - names
                if not missing:
                    suite.add("tools/list – core tools present", PASS, f"{len(names)} tools")
                else:
                    suite.add("tools/list – core tools present", WARN, f"Missing: {sorted(missing)}")
            else:
                suite.add("tools/list", FAIL, f"Got: {resp}")
                return

            # list_models works in mock mode when Gazebo is not running
            resp = client.call_tool("gazebo_list_models", {"response_format": "summary"})
            if resp:
                if "error" in resp:
                    suite.add(
                        "gazebo_list_models", WARN,
                        "ROS2/Gazebo not running – mock/error response returned"
                    )
                else:
                    _assert_tool_ok(suite, resp, "gazebo_list_models")
            else:
                suite.add("gazebo_list_models", FAIL, "No response")

            # Spawn a test box (will fail gracefully if Gazebo not running)
            resp = client.call_tool("gazebo_spawn_model", {
                "model_name": "mcp_test_box",
                "geometry":   "box",
                "pose":       {"position": {"x": 0.0, "y": 0.0, "z": 0.5}},
                "size":       {"x": 0.5, "y": 0.5, "z": 0.5},
            })
            _assert_tool_ok_or_warn(suite, resp, "gazebo_spawn_model")

            resp = client.call_tool("gazebo_delete_model", {"model_name": "mcp_test_box"})
            _assert_tool_ok_or_warn(suite, resp, "gazebo_delete_model")

    except Exception as exc:
        suite.add("Gazebo MCP test session", FAIL, str(exc))


# ══════════════════════════════════════════════════════════════════════════════
#  ROS MCP tests
# ══════════════════════════════════════════════════════════════════════════════

ROS_EXPECTED_TOOLS = {
    "get_topics", "get_nodes", "connect_to_robot", "ping_robots",
}


def _ros_server_cmd(venv_python: str) -> List[str]:
    """Build the WSL command to start the ROS MCP server via venv entry-point."""
    venv_bin = venv_python.rsplit("/", 1)[0]
    ros_ep   = f"{venv_bin}/ros-mcp"
    return [
        "wsl", "--", "bash", "-c",
        f"PYTHONUNBUFFERED=1 '{ros_ep}'",
    ]


def test_ros_mcp(suite: Suite, timeout: float, verbose: bool) -> None:
    """Run all ROS MCP tests."""
    if not wsl_available():
        suite.add("WSL available", SKIP, "wsl.exe not found")
        return

    wsl_root = get_wsl_path(REPO_ROOT)
    if not wsl_root:
        suite.add("Repo path accessible from WSL", SKIP, "wslpath conversion failed")
        return

    # Auto-setup venv if needed (ros-mcp-server is pure Python – no ROS2 required)
    ros_wsl = f"{wsl_root}/tools/mcp/ros-mcp-server"
    venv_python = ensure_wsl_venv(ros_wsl, "ros-mcp-server")
    if venv_python is None:
        suite.add(
            "ROS MCP deps", SKIP,
            f"Could not install ros-mcp-server in WSL venv. "
            f"(opencv-python may need: sudo apt install libgl1) "
            f"Run manually: wsl; cd {ros_wsl}; "
            f"python3 -m venv .venv && .venv/bin/pip install -e ."
        )
        return

    cmd = _ros_server_cmd(venv_python)

    try:
        with MCPClientStdio(cmd, timeout=timeout, verbose=verbose) as client:
            time.sleep(3)
            if not client.is_alive():
                stderr = "\n".join(client.stderr_lines[-15:])
                suite.add(
                    "ROS MCP server starts", FAIL,
                    f"Process exited – opencv-python may need libGL: "
                    f"sudo apt install libgl1 libglib2.0-0. "
                    f"stderr: {stderr[:300] or '(empty)'}"
                )
                return
            suite.add("ROS MCP server starts", PASS)

            resp = client.initialize()
            if resp and "result" in resp:
                suite.add("MCP initialize", PASS)
            else:
                suite.add("MCP initialize", FAIL, f"Got: {resp}")
                return

            resp = client.list_tools()
            if resp and "result" in resp:
                names = set(tool_names_from_response(resp))
                missing = ROS_EXPECTED_TOOLS - names
                if not missing:
                    suite.add("tools/list – core tools present", PASS, f"{len(names)} tools")
                else:
                    suite.add("tools/list – core tools present", WARN, f"Missing: {sorted(missing)}")
            else:
                suite.add("tools/list", FAIL, f"Got: {resp}")
                return

            # ping_robots – works even without ROS running
            resp = client.call_tool("ping_robots", {
                "targets": [{"ip": "127.0.0.1", "port": ROSBRIDGE_PORT}]
            })
            if resp and "result" in resp:
                suite.add("ping_robots (localhost rosbridge)", PASS,
                          "Tool responded (port may be closed)")
            else:
                suite.add("ping_robots", FAIL, f"Got: {resp}")

            # connect_to_robot – configures the rosbridge endpoint
            resp = client.call_tool("connect_to_robot", {
                "ip":   "127.0.0.1",
                "port": ROSBRIDGE_PORT,
            })
            _assert_tool_ok_or_warn(suite, resp, "connect_to_robot")

            # get_topics – requires rosbridge running
            resp = client.call_tool("get_topics")
            if resp:
                if "error" in resp:
                    suite.add(
                        "get_topics", WARN,
                        "rosbridge not running – expected connection error"
                    )
                else:
                    _assert_tool_ok(suite, resp, "get_topics")
            else:
                suite.add("get_topics", FAIL, "No response")

            # get_nodes – requires rosbridge running
            resp = client.call_tool("get_nodes")
            if resp:
                if "error" in resp:
                    suite.add("get_nodes", WARN, "rosbridge not running")
                else:
                    _assert_tool_ok(suite, resp, "get_nodes")
            else:
                suite.add("get_nodes", FAIL, "No response")

    except Exception as exc:
        suite.add("ROS MCP test session", FAIL, str(exc))


# ══════════════════════════════════════════════════════════════════════════════
#  Assertion helpers
# ══════════════════════════════════════════════════════════════════════════════

def _assert_tool_ok(suite: Suite, resp: Optional[Dict], label: str) -> bool:
    """Pass if the tool response contains a non-error result."""
    if resp is None:
        suite.add(label, FAIL, "No response (timeout?)")
        return False
    if "error" in resp:
        suite.add(label, FAIL, f"JSON-RPC error: {resp['error']}")
        return False
    content = resp.get("result", {}).get("content", [])
    # Check for application-level error inside content
    for item in content:
        text = item.get("text", "")
        if any(kw in text.lower() for kw in ("error:", "exception:", "traceback")):
            suite.add(label, WARN, f"App-level error in response: {text[:120]}")
            return False
    suite.add(label, PASS)
    return True


def _assert_tool_ok_or_warn(suite: Suite, resp: Optional[Dict], label: str) -> None:
    """Same as _assert_tool_ok but downgrades FAIL to WARN for integration issues."""
    if resp is None:
        suite.add(label, WARN, "No response – backing service may not be running")
        return
    if "error" in resp:
        suite.add(label, WARN, f"JSON-RPC error (service unavailable?): {resp['error']}")
        return
    suite.add(label, PASS)


# ══════════════════════════════════════════════════════════════════════════════
#  Optional cmd-agent session
# ══════════════════════════════════════════════════════════════════════════════

def run_cmd_agent_session(timeout: float, verbose: bool,
                          wsl_root: Optional[str] = None) -> None:
    """
    Interactive cmd-agent session: start the FreeCAD MCP server and let the
    user send raw JSON-RPC calls (or type 'exit' to quit).  This mirrors what
    a Copilot coding-agent would do when connected to the MCP server.
    """
    print("\n" + "─" * 60)
    print("CMD AGENT SESSION  –  FreeCAD MCP")
    print("─" * 60)
    cmd = _freecad_server_cmd(wsl_root)
    if cmd is None:
        print(f"{FAIL}  freecad-mcp not found.")
        return

    with MCPClientStdio(cmd, timeout=timeout, verbose=True) as client:
        time.sleep(3.0 if "wsl" in cmd[0].lower() else 1.5)
        if not client.is_alive():
            stderr = "\n".join(client.stderr_lines[-8:])
            print(f"{FAIL}  Server failed to start. stderr: {stderr[:300]}")
            return
        resp = client.initialize()
        if not resp or "result" not in resp:
            print(f"{FAIL}  Could not initialize MCP session.")
            return
        tools_resp = client.list_tools()
        names = tool_names_from_response(tools_resp or {})
        print(f"\n{PASS}  Server ready.  Available tools: {names}\n")
        print("Enter a tool call as JSON  {'tool': 'name', 'args': {...}}")
        print("or type 'exit' to quit.\n")

        while True:
            try:
                raw = input("agent> ").strip()
            except (EOFError, KeyboardInterrupt):
                break
            if not raw or raw.lower() in ("exit", "quit", "q"):
                break
            try:
                data  = json.loads(raw)
                tool  = data.get("tool") or data.get("name") or ""
                args  = data.get("args") or data.get("arguments") or {}
                resp  = client.call_tool(tool, args)
                print(json.dumps(resp, indent=2))
            except json.JSONDecodeError as exc:
                print(f"Invalid JSON: {exc}")
            except Exception as exc:
                print(f"Error: {exc}")

    print("\nCmd-agent session ended.")


# ══════════════════════════════════════════════════════════════════════════════
#  Summary printer
# ══════════════════════════════════════════════════════════════════════════════

def print_summary(suites: List[Suite]) -> int:
    """Print a summary table and return exit code (0 = all passed / warned)."""
    print("\n" + "═" * 60)
    print("SUMMARY")
    print("═" * 60)
    total_pass = total_fail = total_skip = 0
    for suite in suites:
        status = PASS if suite.failed == 0 else FAIL
        print(f"  {status}  {suite.name}  "
              f"[passed={suite.passed} failed={suite.failed} skipped={suite.skipped}]")
        total_pass  += suite.passed
        total_fail  += suite.failed
        total_skip  += suite.skipped
    print("─" * 60)
    print(f"  Total: {total_pass} passed  {total_fail} failed  {total_skip} skipped")
    print("═" * 60)
    return 0 if total_fail == 0 else 1


# ══════════════════════════════════════════════════════════════════════════════
#  Main
# ══════════════════════════════════════════════════════════════════════════════

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(
        description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter
    )
    p.add_argument("--start-apps",    action="store_true",
                   help="Launch FreeCAD, Gazebo, ROS2 before testing")
    p.add_argument("--startup-wait",  type=float, default=20.0,  metavar="N",
                   help="Seconds to wait after launching apps (default 20)")
    p.add_argument("--no-freecad",    action="store_true",  help="Skip FreeCAD MCP tests")
    p.add_argument("--no-gazebo",     action="store_true",  help="Skip Gazebo  MCP tests")
    p.add_argument("--no-ros",        action="store_true",  help="Skip ROS     MCP tests")
    p.add_argument("--timeout",       type=float, default=15.0,  metavar="N",
                   help="MCP server response timeout in seconds (default 15)")
    p.add_argument("--verbose",       action="store_true",  help="Show full JSON-RPC messages")
    p.add_argument("--cmd-agent",     action="store_true",
                   help="After tests, start an interactive coding-agent session")
    return p.parse_args()


def main() -> int:
    args = parse_args()
    print("=" * 60)
    print("MCP Server Test Suite")
    print(f"Repo: {REPO_ROOT}")
    print("=" * 60)

    # Compute wsl_root once for all tests
    wsl_root: Optional[str] = None
    if wsl_available():
        wsl_root = get_wsl_path(REPO_ROOT)
        if wsl_root:
            print(f"  WSL root: {wsl_root}")
        else:
            print(f"  {WARN}  WSL available but wslpath conversion failed")
    else:
        print(f"  {WARN}  WSL not found – Gazebo/ROS MCP tests will be skipped")

    # ── Optional app startup ───────────────────────────────────────────────────
    if args.start_apps:
        print("\n[Starting applications]")
        if not args.no_freecad:
            start_app(REPO_ROOT / "Start-FreeCAD.bat", "FreeCAD")
        if not args.no_gazebo:
            start_app(REPO_ROOT / "Start-gz-sim.bat",  "Gazebo")
        if not args.no_ros:
            start_app(REPO_ROOT / "Start-ros2.bat",    "ROS2")
        print(f"  Waiting {args.startup_wait:.0f}s for apps to boot …")
        time.sleep(args.startup_wait)

    suites: List[Suite] = []

    # ── FreeCAD MCP ────────────────────────────────────────────────────────────
    if not args.no_freecad:
        print("\n[FreeCAD MCP]")
        suite = Suite("FreeCAD MCP")
        test_freecad_mcp(suite, timeout=args.timeout, verbose=args.verbose,
                         wsl_root=wsl_root)
        suites.append(suite)

    # ── Gazebo MCP ─────────────────────────────────────────────────────────────
    if not args.no_gazebo:
        print("\n[Gazebo MCP]")
        suite = Suite("Gazebo MCP")
        test_gazebo_mcp(suite, timeout=args.timeout, verbose=args.verbose)
        suites.append(suite)

    # ── ROS MCP ────────────────────────────────────────────────────────────────
    if not args.no_ros:
        print("\n[ROS MCP]")
        suite = Suite("ROS MCP")
        test_ros_mcp(suite, timeout=args.timeout, verbose=args.verbose)
        suites.append(suite)

    # ── Summary ────────────────────────────────────────────────────────────────
    rc = print_summary(suites)

    # ── Optional cmd-agent interactive session ─────────────────────────────────
    if args.cmd_agent:
        run_cmd_agent_session(timeout=args.timeout, verbose=args.verbose,
                               wsl_root=wsl_root)

    return rc


if __name__ == "__main__":
    sys.exit(main())
