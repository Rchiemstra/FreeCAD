#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Verify MCP stdio servers start inside the E2E image (no LLM host required)."""
from __future__ import annotations

import os
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

from test_all_mcp import MCPClientStdio, tool_names_from_response  # noqa: E402


def _venv() -> Path:
    return Path(os.environ.get("MCP_VENV", "/opt/mcp-venv"))


def _check_gazebo() -> None:
    gz_root = ROOT / "tools" / "mcp" / "gazebo-mcp"
    cmd = [str(_venv() / "bin" / "gazebo-mcp-server")]
    timeout = float(os.environ.get("E2E_MCP_TIMEOUT", "45"))
    with MCPClientStdio(cmd, cwd=gz_root, timeout=timeout) as cli:
        assert cli.is_alive(), "gazebo-mcp-server failed to start"
        resp = cli.initialize()
        assert resp and "result" in resp, "gazebo MCP initialize failed"
        lt = cli.list_tools()
        assert lt and "result" in lt, "gazebo tools/list failed"
        cli.call_tool("gazebo_list_models", {"response_format": "summary"})
        cli.call_tool("gazebo_get_simulation_status", {})


def _check_freecad() -> None:
    fc_root = ROOT / "tools" / "mcp" / "freecad-mcp"
    cmd = [str(_venv() / "bin" / "freecad-mcp")]
    timeout = float(os.environ.get("E2E_MCP_TIMEOUT", "45"))
    with MCPClientStdio(cmd, cwd=fc_root, timeout=timeout) as cli:
        assert cli.is_alive(), "freecad-mcp failed to start"
        resp = cli.initialize()
        assert resp and "result" in resp, "freecad MCP initialize failed"
        lt = cli.list_tools()
        names = tool_names_from_response(lt or {})
        assert len(names) >= 5, f"unexpected tool count: {names}"


def _check_ros() -> None:
    ros_root = ROOT / "tools" / "mcp" / "ros-mcp-server"
    cmd = [str(_venv() / "bin" / "ros-mcp")]
    timeout = float(os.environ.get("E2E_MCP_TIMEOUT", "60"))
    with MCPClientStdio(cmd, cwd=ros_root, timeout=timeout) as cli:
        assert cli.is_alive(), "ros-mcp failed to start"
        resp = cli.initialize()
        assert resp and "result" in resp, "ros MCP initialize failed"
        lt = cli.list_tools()
        assert lt and "result" in lt, "ros tools/list failed"
        cli.call_tool("ping_robots", {"targets": [{"ip": "127.0.0.1", "port": 9090}]})


def main() -> int:
    print("[mcp_smoke] gazebo-mcp …")
    _check_gazebo()
    print("[mcp_smoke] freecad-mcp …")
    _check_freecad()
    print("[mcp_smoke] ros-mcp …")
    _check_ros()
    print("[mcp_smoke] OK")
    return 0


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except Exception as exc:
        print("[mcp_smoke] FAIL:", exc, file=sys.stderr)
        raise SystemExit(1)
