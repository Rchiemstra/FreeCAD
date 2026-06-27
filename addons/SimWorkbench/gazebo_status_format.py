# SPDX-License-Identifier: LGPL-2.1-or-later
"""
gazebo_status_format.py — pure helpers for the Gazebo Status panel text.

Kept separate from Qt so pytest can cover formatting without FreeCAD.
"""
from __future__ import annotations

from typing import Any, Dict, List, Optional, Sequence


def format_simulation_status(
    data: Optional[Dict[str, Any]],
    *,
    transport_connected: bool,
    model_names: Optional[Sequence[str]] = None,
) -> str:
    """Build a multi-line status summary for the workbench panel."""
    lines: List[str] = []

    if transport_connected:
        lines.append("Transport: connected")
    else:
        lines.append("Transport: disconnected")

    if not data:
        lines.append("Gazebo: no status data")
        if model_names is not None:
            lines.append(f"Models: {len(model_names)}")
        return "\n".join(lines)

    connected = data.get("gazebo_connected", data.get("running", False))
    paused = data.get("paused", False)
    sim_time = data.get("simulation_time", data.get("sim_time", 0.0))
    note = data.get("note", "")

    state = "connected" if connected else "not connected"
    if note:
        state = f"{state} ({note})"
    lines.append(f"Gazebo: {state}")
    lines.append(f"Paused: {'yes' if paused else 'no'}")
    lines.append(f"Sim time: {float(sim_time):.3f} s")

    if model_names is not None:
        if model_names:
            lines.append("Models: " + ", ".join(model_names))
        else:
            lines.append("Models: (none)")

    return "\n".join(lines)


def format_mcp_services(services: Sequence[Any]) -> str:
    """Format ServiceStatus rows (or objects with label())."""
    rows: List[str] = []
    for svc in services:
        if hasattr(svc, "label"):
            rows.append(svc.label())
        else:
            rows.append(str(svc))
    return "\n".join(rows) if rows else "MCP: (no checks run)"


def screenshot_unavailable_message() -> str:
    return (
        "No Gazebo camera snapshot yet.\n"
        "Headless sim has no default render topic in v1.\n"
        "Configure a camera sensor, then refresh."
    )
