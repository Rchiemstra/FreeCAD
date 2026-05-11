"""
gazebo_status_logic.py — formatting helpers for the Gazebo status panel.

Kept free of Qt imports so unit tests can run without FreeCAD GUI binaries.
"""
from __future__ import annotations

import json
from typing import Any, Optional


def format_simulation_status_detail(data: Optional[dict[str, Any]]) -> str:
    """Pretty-print ``gazebo_get_simulation_status`` payload for the panel."""
    if data is None:
        return "(no status payload)"
    try:
        return json.dumps(data, indent=2, sort_keys=True, default=str)
    except Exception:
        return str(data)


def combined_status_heading(
    transport_label: str,
    mcp_ok: Optional[bool],
    mcp_message: str,
) -> str:
    """Single-line summary for the status heading label."""
    if mcp_ok is None:
        mcp_part = "MCP: not queried yet"
    elif mcp_ok:
        mcp_part = "MCP: ok"
    else:
        mcp_part = f"MCP: error — {mcp_message[:120]}"
    return f"Transport: {transport_label}  |  {mcp_part}"
