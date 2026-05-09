"""
sim_controls.py — Sim Controls dock panel.

Provides Play / Pause / Resume / Step / Reset buttons plus a live readout
of sim time and real-time factor (RTF).

Layout::

    ┌─ Sim Controls ─────────────────┐
    │  Status: ● Disconnected         │
    │  Sim time:  0.000 s             │
    │  RTF:       0.00                │
    ├─────────────────────────────────┤
    │  [▶ Start] [⏸ Pause] [▶▶ Step] │
    │  [↺ Reset]                      │
    └─────────────────────────────────┘
"""
from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from sim_workbench import SimWorkbenchCoordinator

try:
    from PySide2.QtWidgets import (
        QWidget, QVBoxLayout, QHBoxLayout,
        QPushButton, QLabel, QFrame,
    )
    from PySide2.QtCore import Qt
    from PySide2.QtGui import QColor
    _QT = True
except ImportError:
    _QT = False
    QWidget = object   # type: ignore


class SimControlsPanel(QWidget if _QT else object):  # type: ignore
    """Qt dock widget panel for simulation lifecycle controls."""

    def __init__(self, coordinator: "SimWorkbenchCoordinator", parent=None):
        if not _QT:
            self._coord = coordinator
            return
        super().__init__(parent)
        self._coord = coordinator
        self._setup_ui()

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setSpacing(4)
        layout.setContentsMargins(8, 8, 8, 8)

        # --- Status row ---
        status_row = QHBoxLayout()
        self._status_dot = QLabel("●")
        self._status_dot.setStyleSheet("color: #888;")
        self._status_label = QLabel("Disconnected")
        status_row.addWidget(self._status_dot)
        status_row.addWidget(self._status_label)
        status_row.addStretch()
        layout.addLayout(status_row)

        # --- Sim time / RTF ---
        self._time_label = QLabel("Sim time:  0.000 s")
        self._rtf_label  = QLabel("RTF:       ---")
        layout.addWidget(self._time_label)
        layout.addWidget(self._rtf_label)

        # --- Separator ---
        sep = QFrame()
        sep.setFrameShape(QFrame.HLine)
        layout.addWidget(sep)

        # --- Control buttons ---
        btn_row1 = QHBoxLayout()
        self._btn_start  = QPushButton("▶ Start")
        self._btn_pause  = QPushButton("⏸ Pause")
        self._btn_resume = QPushButton("▷ Resume")
        self._btn_step   = QPushButton("▶▶ Step")
        for btn in (self._btn_start, self._btn_pause,
                    self._btn_resume, self._btn_step):
            btn_row1.addWidget(btn)
        layout.addLayout(btn_row1)

        btn_row2 = QHBoxLayout()
        self._btn_reset = QPushButton("↺ Reset")
        btn_row2.addWidget(self._btn_reset)
        btn_row2.addStretch()
        layout.addLayout(btn_row2)

        layout.addStretch()

        # Wire up buttons
        self._btn_start.clicked.connect(self._on_start)
        self._btn_pause.clicked.connect(self._on_pause)
        self._btn_resume.clicked.connect(self._on_resume)
        self._btn_step.clicked.connect(self._on_step)
        self._btn_reset.clicked.connect(self._on_reset)

    # ------------------------------------------------------------------
    # Button handlers
    # ------------------------------------------------------------------

    def _on_start(self):
        self._coord.start_sim()

    def _on_pause(self):
        self._coord.pause_sim()

    def _on_resume(self):
        self._coord.resume_sim()

    def _on_step(self):
        self._coord.step_sim(1)

    def _on_reset(self):
        self._coord.reset_sim()

    # ------------------------------------------------------------------
    # External updates (called from transport callbacks)
    # ------------------------------------------------------------------

    def update_connection_status(self, status) -> None:
        """Update the status indicator (called on transport status change)."""
        if not _QT:
            return
        from transport import ConnectionStatus
        color_map = {
            ConnectionStatus.DISCONNECTED: "#888",
            ConnectionStatus.CONNECTING:   "#e9a227",
            ConnectionStatus.CONNECTED:    "#4caf50",
            ConnectionStatus.ERROR:        "#f44336",
        }
        label_map = {
            ConnectionStatus.DISCONNECTED: "Disconnected",
            ConnectionStatus.CONNECTING:   "Connecting…",
            ConnectionStatus.CONNECTED:    "Connected",
            ConnectionStatus.ERROR:        "Error",
        }
        color = color_map.get(status, "#888")
        text  = label_map.get(status, str(status))
        self._status_dot.setStyleSheet(f"color: {color};")
        self._status_label.setText(text)

    def update_sim_time(self, sim_time: float, rtf: float) -> None:
        """Update sim time and RTF readouts."""
        if not _QT:
            return
        self._time_label.setText(f"Sim time:  {sim_time:9.3f} s")
        self._rtf_label.setText( f"RTF:       {rtf:.2f}")
