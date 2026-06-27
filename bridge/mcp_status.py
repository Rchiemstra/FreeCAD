# SPDX-License-Identifier: LGPL-2.1-or-later
"""
bridge.mcp_status — lightweight reachability checks for MCP-related services.

Used by the Simulation Workbench Gazebo Status panel so operators can see
whether FreeCAD XML-RPC and the stdio MCP servers respond without reading logs.
"""
from __future__ import annotations

import socket
import xmlrpc.client
from dataclasses import dataclass
from pathlib import Path
from typing import List, Optional

FREECAD_RPC_PORT = 9875


@dataclass
class ServiceStatus:
    name: str
    online: bool
    detail: str = ""

    def label(self) -> str:
        mark = "online" if self.online else "offline"
        extra = f" — {self.detail}" if self.detail else ""
        return f"{self.name}: {mark}{extra}"


def freecad_rpc_alive(host: str = "localhost", port: int = FREECAD_RPC_PORT, timeout: float = 2.0) -> bool:
    """Return True when the FreeCAD MCP addon XML-RPC server answers ping()."""
    try:
        proxy = xmlrpc.client.ServerProxy(f"http://{host}:{port}", allow_none=True)
        return bool(proxy.ping())
    except Exception:
        return False


def tcp_port_open(host: str, port: int, timeout: float = 1.0) -> bool:
    """Return True when *host*:*port* accepts a TCP connection."""
    try:
        with socket.create_connection((host, port), timeout=timeout):
            return True
    except OSError:
        return False


def probe_mcp_stdio(
    cmd: List[str],
    cwd: Optional[Path] = None,
    timeout: float = 12.0,
) -> ServiceStatus:
    """
    Start an MCP server subprocess, run initialize + tools/list, then exit.

    This is heavier than a port check but confirms the server package starts.
    """
    label = Path(cmd[0]).stem if cmd else "mcp"
    try:
        from test_all_mcp import MCPClientStdio, tool_names_from_response
    except ImportError as exc:
        return ServiceStatus(label, False, f"test_all_mcp unavailable ({exc})")

    try:
        with MCPClientStdio(cmd, cwd=cwd, timeout=timeout) as cli:
            if not cli.is_alive():
                return ServiceStatus(label, False, "process exited")
            resp = cli.initialize()
            if not resp or "result" not in resp:
                return ServiceStatus(label, False, "initialize failed")
            lt = cli.list_tools()
            names = tool_names_from_response(lt or {})
            return ServiceStatus(label, True, f"{len(names)} tools")
    except Exception as exc:
        return ServiceStatus(label, False, str(exc)[:120])


def check_freecad_stack(host: str = "localhost", port: int = FREECAD_RPC_PORT) -> ServiceStatus:
    """FreeCAD addon RPC (port 9875) — required for in-process automation."""
    if freecad_rpc_alive(host, port):
        return ServiceStatus("FreeCAD XML-RPC", True, f"{host}:{port}")
    return ServiceStatus("FreeCAD XML-RPC", False, f"no response on {host}:{port}")


def check_gazebo_mcp(repo_root: Path, *, probe_stdio: bool = False, timeout: float = 12.0) -> ServiceStatus:
    """gazebo-mcp stdio server (spawns on demand from bridge calls)."""
    if not probe_stdio:
        return ServiceStatus(
            "gazebo-mcp",
            True,
            "stdio (starts per bridge call)",
        )
    venv = repo_root / "tools" / "mcp" / "gazebo-mcp" / ".venv" / "bin" / "gazebo-mcp-server"
    if not venv.is_file():
        return ServiceStatus("gazebo-mcp", False, "venv server not found")
    return probe_mcp_stdio([str(venv)], cwd=repo_root / "tools" / "mcp" / "gazebo-mcp", timeout=timeout)


def check_ros_mcp(repo_root: Path, *, probe_stdio: bool = False, timeout: float = 15.0) -> ServiceStatus:
    """ros-mcp stdio server."""
    if not probe_stdio:
        return ServiceStatus("ros-mcp", True, "stdio (starts per bridge call)")
    venv = repo_root / "tools" / "mcp" / "ros-mcp-server" / ".venv" / "bin" / "ros-mcp"
    if not venv.is_file():
        return ServiceStatus("ros-mcp", False, "venv server not found")
    return probe_mcp_stdio([str(venv)], cwd=repo_root / "tools" / "mcp" / "ros-mcp-server", timeout=timeout)


def check_freecad_mcp(repo_root: Path, *, probe_stdio: bool = False, timeout: float = 12.0) -> ServiceStatus:
    """freecad-mcp stdio server (separate from the in-FreeCAD XML-RPC addon)."""
    if not probe_stdio:
        return ServiceStatus("freecad-mcp", True, "stdio (starts per tool call)")
    venv = repo_root / "tools" / "mcp" / "freecad-mcp" / ".venv" / "bin" / "freecad-mcp"
    if not venv.is_file():
        return ServiceStatus("freecad-mcp", False, "venv server not found")
    return probe_mcp_stdio([str(venv)], cwd=repo_root / "tools" / "mcp" / "freecad-mcp", timeout=timeout)
