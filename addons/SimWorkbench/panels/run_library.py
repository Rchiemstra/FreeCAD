"""
run_library.py — Run Library dock panel.

Lists previous simulation runs from ``sim_runs/`` so the user can browse
results without leaving FreeCAD.

Layout::

    ┌─ Run Library ───────────────────────────────┐
    │  ┌─────────────────────────────────────────┐ │
    │  │ 20250615_143000_reach_top_shelf  ✓      │ │
    │  │ 20250614_110022_reach_top_shelf  ✗      │ │
    │  └─────────────────────────────────────────┘ │
    │  [  Open Result YAML   ]                      │
    └──────────────────────────────────────────────┘
"""
from __future__ import annotations

import os
from pathlib import Path
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from sim_workbench import SimWorkbenchCoordinator

try:
    from PySide2.QtWidgets import (
        QWidget, QVBoxLayout, QListWidget, QPushButton, QLabel,
    )
    _QT = True
except ImportError:
    _QT = False
    QWidget = object  # type: ignore


class RunLibraryPanel(QWidget if _QT else object):  # type: ignore
    """Browses sim_runs/ and shows pass/fail for each run."""

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

        self._label = QLabel("Simulation Runs")
        layout.addWidget(self._label)

        self._list = QListWidget()
        layout.addWidget(self._list)

        self._open_btn = QPushButton("Open Result YAML")
        self._open_btn.clicked.connect(self._on_open)
        layout.addWidget(self._open_btn)

        self._refresh()

    def _refresh(self):
        """Repopulate the list from sim_runs/."""
        self._list.clear()
        try:
            from bridge.project import load_project
            cfg = load_project()
            runs_dir = Path(cfg.root) / "sim_runs"
        except Exception:
            runs_dir = Path.cwd() / "sim_runs"

        if not runs_dir.exists():
            self._list.addItem("(no sim_runs/ directory yet)")
            return

        runs = sorted(runs_dir.iterdir(), reverse=True)
        if not runs:
            self._list.addItem("(no runs yet)")
            return

        for run_dir in runs:
            result_file = run_dir / "result.yaml"
            if result_file.exists():
                status = self._read_status(result_file)
                icon   = "✓" if status == "pass" else "✗" if status == "fail" else "?"
            else:
                icon = "·"
            self._list.addItem(f"{run_dir.name}  {icon}")

    @staticmethod
    def _read_status(path: Path) -> str:
        try:
            import yaml
            data = yaml.safe_load(path.read_text(encoding="utf-8"))
            return str(data.get("status", "unknown"))
        except Exception:
            return "unknown"

    def _on_open(self):
        item = self._list.currentItem()
        if item is None:
            return
        run_name = item.text().split("  ")[0].strip()
        try:
            from bridge.project import load_project
            cfg = load_project()
            runs_dir = Path(cfg.root) / "sim_runs"
        except Exception:
            runs_dir = Path.cwd() / "sim_runs"

        result_file = runs_dir / run_name / "result.yaml"
        if result_file.exists():
            import subprocess
            subprocess.Popen(["notepad.exe", str(result_file)])
