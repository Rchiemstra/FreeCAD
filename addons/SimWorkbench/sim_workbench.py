"""
sim_workbench.py — SimWorkbench coordinator.

Central singleton that owns the transport, state bridge, and panel
references. Accessed via ``get_coordinator()``.
"""
from __future__ import annotations

import logging
from typing import Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from transport import GazeboTransport, ConnectionStatus
    from state_bridge import StateBridge

log = logging.getLogger(__name__)

_coordinator: Optional["SimWorkbenchCoordinator"] = None


def get_coordinator() -> "SimWorkbenchCoordinator":
    """Return the singleton coordinator, creating it on first call."""
    global _coordinator
    if _coordinator is None:
        _coordinator = SimWorkbenchCoordinator()
    return _coordinator


class SimWorkbenchCoordinator:
    """
    Owns the transport, state bridge, and all dock panels.

    Lifecycle (called from InitGui.py):
      - ``show_panels()``  — create dock panels and start transport
      - ``hide_panels()``  — hide panels (transport keeps running)
      - ``shutdown()``     — stop transport and destroy panels (addon unload)

    Simulation controls (forwarded to bridge):
      - ``start_sim()``
      - ``pause_sim()``
      - ``resume_sim()``
      - ``step_sim(n)``
      - ``reset_sim()``
    """

    def __init__(self):
        self._transport: Optional["GazeboTransport"] = None
        self._state_bridge: Optional["StateBridge"] = None
        self._panels: dict[str, object] = {}
        self._sim_running = False

    # ------------------------------------------------------------------
    # Panel management
    # ------------------------------------------------------------------

    def show_panels(self) -> None:
        """Show (or create) all dock panels."""
        self._ensure_transport()
        self._ensure_panels()
        for panel in self._panels.values():
            try:
                panel.show()
            except Exception as exc:
                log.debug("Panel show failed: %s", exc)

    def hide_panels(self) -> None:
        for panel in self._panels.values():
            try:
                panel.hide()
            except Exception:
                pass

    def shutdown(self) -> None:
        if self._transport is not None:
            self._transport.stop()
        for panel in self._panels.values():
            try:
                panel.close()
            except Exception:
                pass
        self._panels.clear()

    # ------------------------------------------------------------------
    # Transport / bridge
    # ------------------------------------------------------------------

    def _ensure_transport(self) -> None:
        if self._transport is not None:
            return
        from transport import GazeboTransport
        self._transport = GazeboTransport(poll_interval_ms=100)

        from state_bridge import StateBridge
        self._state_bridge = StateBridge(self._transport)
        self._state_bridge.start()

        self._transport.on_status_change(self._on_connection_status)
        self._transport.start()

    def _on_connection_status(self, status: "ConnectionStatus") -> None:
        controls = self._panels.get("sim_controls")
        if controls is not None:
            try:
                controls.update_connection_status(status)
            except Exception:
                pass
        gz_status = self._panels.get("gazebo_status")
        if gz_status is not None:
            try:
                gz_status.update_transport_status(status)
            except Exception:
                pass

    # ------------------------------------------------------------------
    # Simulation lifecycle controls
    # ------------------------------------------------------------------

    def start_sim(self) -> str:
        """Export robot, stage world, and spawn in Gazebo."""
        try:
            from bridge.handoff import export_and_spawn
            result = export_and_spawn()
            self._sim_running = result.success
            return result.summary()
        except Exception as exc:
            log.warning("start_sim failed: %s", exc)
            return f"Error: {exc}"

    def pause_sim(self) -> str:
        return self._gazebo_call("pause_simulation")

    def resume_sim(self) -> str:
        return self._gazebo_call("resume_simulation")

    def step_sim(self, steps: int = 1) -> str:
        return self._gazebo_call("step_simulation", steps=steps)

    def reset_sim(self) -> str:
        result = self._gazebo_call("reset_simulation")
        self._sim_running = False
        return result

    def _gazebo_call(self, tool_name: str, **kwargs) -> str:
        try:
            from bridge import gazebo_bridge
            fn = getattr(gazebo_bridge, tool_name)
            fn(**kwargs)
            return f"{tool_name}: ok"
        except Exception as exc:
            return f"{tool_name}: {exc}"

    # ------------------------------------------------------------------
    # Panel factory
    # ------------------------------------------------------------------

    def _ensure_panels(self) -> None:
        if self._panels:
            return
        try:
            self._panels = _create_panels(self)
        except Exception as exc:
            log.warning("Could not create panels: %s", exc)

    @property
    def transport(self) -> Optional["GazeboTransport"]:
        return self._transport

    @property
    def state_bridge(self) -> Optional["StateBridge"]:
        return self._state_bridge

    @property
    def sim_running(self) -> bool:
        return self._sim_running


# ---------------------------------------------------------------------------
# Panel factory (separated so imports only happen inside FreeCAD)
# ---------------------------------------------------------------------------

def _create_panels(coord: "SimWorkbenchCoordinator") -> dict:
    """Import and instantiate all dock panels."""
    panels = {}
    try:
        from panels.sim_controls import SimControlsPanel
        panels["sim_controls"] = SimControlsPanel(coord)
        _add_dock(panels["sim_controls"], "Sim Controls", "Right")
    except Exception as exc:
        log.warning("SimControlsPanel failed: %s", exc)

    try:
        from panels.gazebo_status_panel import GazeboStatusPanel
        panels["gazebo_status"] = GazeboStatusPanel(coord)
        _add_dock(panels["gazebo_status"], "Gazebo Status", "Right")
    except Exception as exc:
        log.warning("GazeboStatusPanel failed: %s", exc)

    try:
        from panels.scenario_picker import ScenarioPickerPanel
        panels["scenario_picker"] = ScenarioPickerPanel(coord)
        _add_dock(panels["scenario_picker"], "Scenario", "Left")
    except Exception as exc:
        log.warning("ScenarioPickerPanel failed: %s", exc)

    try:
        from panels.sensor_plots import SensorPlotsPanel
        panels["sensor_plots"] = SensorPlotsPanel(coord)
        _add_dock(panels["sensor_plots"], "Sensor Plots", "Bottom")
    except Exception as exc:
        log.warning("SensorPlotsPanel failed: %s", exc)

    try:
        from panels.run_library import RunLibraryPanel
        panels["run_library"] = RunLibraryPanel(coord)
        _add_dock(panels["run_library"], "Run Library", "Left")
    except Exception as exc:
        log.warning("RunLibraryPanel failed: %s", exc)

    try:
        from panels.test_runner_panel import TestRunnerPanel
        panels["test_runner"] = TestRunnerPanel(coord)
        _add_dock(panels["test_runner"], "Test Runner", "Left")
    except Exception as exc:
        log.warning("TestRunnerPanel failed: %s", exc)

    try:
        from panels.mcp_log import MCPLogPanel
        panels["mcp_log"] = MCPLogPanel(coord)
        _add_dock(panels["mcp_log"], "MCP Activity", "Bottom")
    except Exception as exc:
        log.warning("MCPLogPanel failed: %s", exc)

    return panels


def _add_dock(widget, title: str, area: str = "Right") -> None:
    """Add a Qt dock widget to FreeCAD's main window."""
    try:
        from PySide2.QtWidgets import QDockWidget
        from PySide2.QtCore import Qt
        import FreeCADGui

        mw = FreeCADGui.getMainWindow()
        dock = QDockWidget(title, mw)
        dock.setObjectName(f"SimWorkbench_{title.replace(' ', '_')}")
        dock.setWidget(widget)
        dock.setAllowedAreas(Qt.AllDockWidgetAreas)
        area_map = {
            "Left":   Qt.LeftDockWidgetArea,
            "Right":  Qt.RightDockWidgetArea,
            "Bottom": Qt.BottomDockWidgetArea,
            "Top":    Qt.TopDockWidgetArea,
        }
        mw.addDockWidget(area_map.get(area, Qt.RightDockWidgetArea), dock)
        # Store dock reference on the widget so we can show/hide it
        widget._dock = dock
    except Exception as exc:
        log.debug("_add_dock failed for %r: %s", title, exc)
