"""
gazebo_status.py — Gazebo Status / Screenshot dock panel.

Shows Gazebo simulation status, MCP reachability, and a placeholder for
headless camera snapshots (no embedded Gazebo GUI viewport).

Layout::

    ┌─ Gazebo Status ────────────────────────┐
    │  [Refresh]  [Probe MCP servers]       │
    │  ── Simulation ──                     │
    │  Transport: connected                 │
    │  Gazebo: connected                    │
    │  ...                                  │
    │  ── MCP services ──                   │
    │  FreeCAD XML-RPC: online              │
    │  ...                                  │
    │  ┌─────────────────────────────────┐  │
    │  │  (screenshot placeholder)       │  │
    │  └─────────────────────────────────┘  │
    └───────────────────────────────────────┘
"""
from __future__ import annotations

import sys
from pathlib import Path
from typing import TYPE_CHECKING, List, Optional

if TYPE_CHECKING:
    from sim_workbench import SimWorkbenchCoordinator

try:
    from PySide2.QtWidgets import (
        QWidget, QVBoxLayout, QHBoxLayout, QLabel, QPushButton,
        QTextEdit, QFrame,
    )
    from PySide2.QtCore import Qt, QTimer
    _QT = True
except ImportError:
    _QT = False
    QWidget = object  # type: ignore

# Repo root for bridge / mcp_status imports when running inside FreeCAD
_REPO_ROOT = Path(__file__).resolve().parents[3]
if str(_REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(_REPO_ROOT))

from gazebo_status_format import (  # noqa: E402
    format_mcp_services,
    format_simulation_status,
    screenshot_unavailable_message,
)


class GazeboStatusPanel(QWidget if _QT else object):  # type: ignore
    """Gazebo status readout plus snapshot placeholder."""

    _REFRESH_MS = 3000

    def __init__(self, coordinator: "SimWorkbenchCoordinator", parent=None):
        if not _QT:
            self._coord = coordinator
            return
        super().__init__(parent)
        self._coord = coordinator
        self._repo = _REPO_ROOT
        self._model_names: List[str] = []
        self._transport_connected = False
        self._setup_ui()
        self._wire_transport()

        self._timer = QTimer(self)
        self._timer.timeout.connect(self.refresh_status)
        self._timer.start(self._REFRESH_MS)

        self.refresh_status()

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setSpacing(4)
        layout.setContentsMargins(8, 8, 8, 8)

        btn_row = QHBoxLayout()
        self._btn_refresh = QPushButton("Refresh")
        self._btn_refresh.clicked.connect(self.refresh_status)
        self._btn_probe = QPushButton("Probe MCP servers")
        self._btn_probe.clicked.connect(self._probe_mcp_servers)
        btn_row.addWidget(self._btn_refresh)
        btn_row.addWidget(self._btn_probe)
        btn_row.addStretch()
        layout.addLayout(btn_row)

        self._sim_label = QLabel("Simulation")
        self._sim_label.setStyleSheet("font-weight: bold;")
        layout.addWidget(self._sim_label)

        self._status_text = QTextEdit()
        self._status_text.setReadOnly(True)
        self._status_text.setMaximumHeight(120)
        layout.addWidget(self._status_text)

        sep = QFrame()
        sep.setFrameShape(QFrame.HLine)
        layout.addWidget(sep)

        self._mcp_label = QLabel("MCP services")
        self._mcp_label.setStyleSheet("font-weight: bold;")
        layout.addWidget(self._mcp_label)

        self._mcp_text = QTextEdit()
        self._mcp_text.setReadOnly(True)
        self._mcp_text.setMaximumHeight(100)
        layout.addWidget(self._mcp_text)

        self._shot_label = QLabel(screenshot_unavailable_message())
        self._shot_label.setAlignment(Qt.AlignCenter)
        self._shot_label.setWordWrap(True)
        self._shot_label.setMinimumHeight(140)
        self._shot_label.setStyleSheet(
            "color: #666; background: #f0f0f0; border: 1px solid #ccc; padding: 8px;"
        )
        layout.addWidget(self._shot_label)

        layout.addStretch()

    def _wire_transport(self):
        transport = self._coord.transport
        if transport is None:
            return
        from transport import ConnectionStatus

        def _on_status(status):
            self._update_transport_line(status == ConnectionStatus.CONNECTED)

        transport.on_status_change(_on_status)
        from transport import ConnectionStatus
        self._update_transport_line(transport.status == ConnectionStatus.CONNECTED)

    def _update_transport_line(self, connected: bool) -> None:
        self._transport_connected = connected

    def refresh_status(self) -> None:
        """Poll Gazebo status via bridge and refresh MCP service lines."""
        if not _QT:
            return

        connected = getattr(self, "_transport_connected", False)
        status_data: Optional[dict] = None
        try:
            from bridge import gazebo_bridge

            result = gazebo_bridge.get_simulation_status(timeout=10.0)
            if result.ok:
                status_data = result.data
            try:
                self._model_names = gazebo_bridge.list_models(timeout=10.0)
            except Exception:
                self._model_names = []
        except Exception as exc:
            status_data = {"note": str(exc)[:200], "gazebo_connected": False}

        text = format_simulation_status(
            status_data,
            transport_connected=connected,
            model_names=self._model_names,
        )
        self._status_text.setPlainText(text)
        self._refresh_mcp_lines(probe_stdio=False)

    def _refresh_mcp_lines(self, *, probe_stdio: bool) -> None:
        try:
            from bridge.mcp_status import (
                check_freecad_stack,
                check_freecad_mcp,
                check_gazebo_mcp,
                check_ros_mcp,
            )

            services = [
                check_freecad_stack(),
                check_freecad_mcp(self._repo, probe_stdio=probe_stdio),
                check_gazebo_mcp(self._repo, probe_stdio=probe_stdio),
                check_ros_mcp(self._repo, probe_stdio=probe_stdio),
            ]
        except Exception as exc:
            services = [f"MCP check error: {exc}"]
        self._mcp_text.setPlainText(format_mcp_services(services))

    def _probe_mcp_servers(self) -> None:
        self._btn_probe.setEnabled(False)
        try:
            self._refresh_mcp_lines(probe_stdio=True)
        finally:
            self._btn_probe.setEnabled(True)

    def show(self):
        if hasattr(self, "_dock"):
            self._dock.show()

    def hide(self):
        if hasattr(self, "_dock"):
            self._dock.hide()

    def close(self):
        if hasattr(self, "_timer"):
            self._timer.stop()
        if hasattr(self, "_dock"):
            self._dock.close()
