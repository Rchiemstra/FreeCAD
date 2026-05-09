"""
scenario_picker.py — Scenario Picker dock panel.

Lets the user select a robot URDF, Gazebo world SDF, and scenario YAML
before running the simulation.

Layout::

    ┌─ Scenario ──────────────────────┐
    │  Robot:    [arm_2dof         ▼] │
    │  World:    [empty_world      ▼] │
    │  Scenario: [reach_top_shelf  ▼] │
    │            [  Load Scenario   ] │
    └─────────────────────────────────┘
"""
from __future__ import annotations

import os
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from sim_workbench import SimWorkbenchCoordinator

try:
    from PySide2.QtWidgets import (
        QWidget, QVBoxLayout, QFormLayout,
        QComboBox, QPushButton, QLabel,
    )
    _QT = True
except ImportError:
    _QT = False
    QWidget = object  # type: ignore


def _find_files(directory: Path, glob: str) -> list[str]:
    """Return stem names of files matching glob in directory."""
    if not directory.exists():
        return []
    return sorted(p.stem for p in directory.glob(glob))


class ScenarioPickerPanel(QWidget if _QT else object):  # type: ignore
    """Panel for selecting robot / world / scenario before a sim run."""

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

        form = QFormLayout()
        form.setSpacing(4)

        self._robot_combo    = QComboBox()
        self._world_combo    = QComboBox()
        self._scenario_combo = QComboBox()

        form.addRow("Robot:",    self._robot_combo)
        form.addRow("World:",    self._world_combo)
        form.addRow("Scenario:", self._scenario_combo)
        layout.addLayout(form)

        self._load_btn = QPushButton("Load Scenario")
        self._load_btn.clicked.connect(self._on_load)
        layout.addWidget(self._load_btn)

        layout.addStretch()

        self._populate()

    def _populate(self):
        """Fill the combo boxes from the repo directories."""
        try:
            from bridge.project import load_project
            cfg = load_project()
            root = Path(cfg.root)
        except Exception:
            root = Path.cwd()

        for stem in _find_files(root / "robots", "*.urdf"):
            self._robot_combo.addItem(stem)
        for stem in _find_files(root / "worlds", "*.sdf"):
            self._world_combo.addItem(stem)
        for stem in _find_files(root / "tests" / "scenarios", "*.yaml"):
            self._scenario_combo.addItem(stem)

    def _on_load(self):
        """Propagate selection to the coordinator for next sim start."""
        import FreeCAD
        robot    = self._robot_combo.currentText()
        world    = self._world_combo.currentText()
        scenario = self._scenario_combo.currentText()
        FreeCAD.Console.PrintMessage(
            f"[SimWorkbench] Selection: robot={robot}, "
            f"world={world}, scenario={scenario}\n"
        )
        # Store selection on the coordinator for start_sim() to read
        self._coord._selected_robot    = robot
        self._coord._selected_world    = world
        self._coord._selected_scenario = scenario
