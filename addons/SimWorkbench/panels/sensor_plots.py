"""
sensor_plots.py — Sensor Plots dock panel.

Displays a live rolling chart of joint positions and the real-time factor.
Uses a simple QTableWidget as a fallback when matplotlib is not available.

Layout::

    ┌─ Sensor Plots ─────────────────────────────┐
    │  RTF: 0.98                                  │
    │  ┌───────────┬──────────┬──────────┐        │
    │  │ Joint     │ Pos (°)  │ Vel (°/s)│        │
    │  ├───────────┼──────────┼──────────┤        │
    │  │ joint_1   │   12.3   │   0.5    │        │
    │  │ joint_2   │  -45.0   │  -1.2    │        │
    │  └───────────┴──────────┴──────────┘        │
    └────────────────────────────────────────────-┘
"""
from __future__ import annotations

import math
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from sim_workbench import SimWorkbenchCoordinator
    from transport import ModelState

try:
    from PySide2.QtWidgets import (
        QWidget, QVBoxLayout, QLabel, QTableWidget,
        QTableWidgetItem, QHeaderView,
    )
    from PySide2.QtCore import Qt
    _QT = True
except ImportError:
    _QT = False
    QWidget = object  # type: ignore


class SensorPlotsPanel(QWidget if _QT else object):  # type: ignore
    """Live joint-state table panel."""

    def __init__(self, coordinator: "SimWorkbenchCoordinator", parent=None):
        if not _QT:
            self._coord = coordinator
            return
        super().__init__(parent)
        self._coord = coordinator
        self._setup_ui()
        if coordinator.transport is not None:
            coordinator.transport.on_state_update(self._on_state_update)

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setSpacing(4)
        layout.setContentsMargins(8, 8, 8, 8)

        self._rtf_label = QLabel("RTF: ---")
        layout.addWidget(self._rtf_label)

        self._table = QTableWidget(0, 4)
        self._table.setHorizontalHeaderLabels(
            ["Joint", "Pos (°)", "Vel (°/s)", "Effort (N·m)"]
        )
        self._table.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
        self._table.verticalHeader().setVisible(False)
        self._table.setEditTriggers(QTableWidget.NoEditTriggers)
        layout.addWidget(self._table)

    def _on_state_update(self, states: "list[ModelState]") -> None:
        if not states or not _QT:
            return
        # Use the first model's state
        state = states[0]
        self._rtf_label.setText(f"RTF: {state.rtf:.2f}  |  Sim: {state.sim_time:.3f} s")

        joints = state.joint_states
        self._table.setRowCount(len(joints))
        for row, js in enumerate(joints):
            pos_deg = math.degrees(js.position)
            vel_deg = math.degrees(js.velocity)
            self._table.setItem(row, 0, QTableWidgetItem(js.name))
            self._table.setItem(row, 1, QTableWidgetItem(f"{pos_deg:8.2f}"))
            self._table.setItem(row, 2, QTableWidgetItem(f"{vel_deg:8.2f}"))
            self._table.setItem(row, 3, QTableWidgetItem(f"{js.effort:8.3f}"))
