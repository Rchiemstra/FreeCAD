"""
test_runner_panel.py — Test Runner dock panel for the Simulation Workbench.

Lists available scenarios, runs them, and shows pass/fail results.

Layout::

    ┌─ Test Runner ─────────────────────────────────┐
    │  ┌───────────────────────────────────────────┐ │
    │  │ ○ reach_top_shelf                         │ │
    │  └───────────────────────────────────────────┘ │
    │  [ Run Selected ]   [ Run All ]                 │
    │  ─────────────────────────────────────────────  │
    │  Status: idle                                   │
    │  ┌───────────────────────────────────────────┐ │
    │  │  ✓  reach_target_within: Reached at t=…   │ │
    │  │  ✗  no_self_collision: 1 self-collision   │ │
    │  └───────────────────────────────────────────┘ │
    └──────────────────────────────────────────────-─┘
"""
from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from sim_workbench import SimWorkbenchCoordinator


class _NoSignal:
    def __init__(self, *_args, **_kwargs):
        pass

    def connect(self, *_args, **_kwargs):
        pass

    def emit(self, *_args, **_kwargs):
        pass


try:
    from PySide6.QtWidgets import (
        QWidget, QVBoxLayout, QHBoxLayout, QListWidget, QListWidgetItem,
        QPushButton, QLabel, QFrame, QTextEdit,
    )
    from PySide6.QtCore import Qt, QThread, Signal, QObject
    from PySide6.QtGui import QFont, QColor
    _QT = True
except ImportError:
    try:
        from PySide2.QtWidgets import (
            QWidget, QVBoxLayout, QHBoxLayout, QListWidget, QListWidgetItem,
            QPushButton, QLabel, QFrame, QTextEdit,
        )
        from PySide2.QtCore import Qt, QThread, Signal, QObject
        from PySide2.QtGui import QFont, QColor
        _QT = True
    except ImportError:
        try:
            from PySide.QtGui import (
                QWidget, QVBoxLayout, QHBoxLayout, QListWidget, QListWidgetItem,
                QPushButton, QLabel, QFrame, QTextEdit, QFont, QColor,
            )
            from PySide.QtCore import Qt, QThread, Signal, QObject
            _QT = True
        except ImportError:
            _QT = False
            QWidget = object      # type: ignore
            QThread = object      # type: ignore
            QObject = object      # type: ignore
            Signal = _NoSignal    # type: ignore


class _RunWorker(QObject if _QT else object):  # type: ignore
    """Runs a scenario in a background thread and emits the result."""
    finished = Signal(object)   # emits RunResult

    def __init__(self, name: str, parent=None):
        if not _QT:
            return
        super().__init__(parent)
        self._name = name

    def run(self):
        try:
            from runner.runner import run_test
            result = run_test(self._name)
        except Exception as exc:
            from runner.result import RunResult
            from runner.scenario import Scenario
            s = Scenario(); s.name = self._name; s.robot = "?"
            result = RunResult(scenario=s, status="error", error_message=str(exc))
        self.finished.emit(result)


class TestRunnerPanel(QWidget if _QT else object):  # type: ignore
    """Dock panel for listing and running scenario tests."""

    def __init__(self, coordinator: "SimWorkbenchCoordinator", parent=None):
        if not _QT:
            self._coord = coordinator
            return
        super().__init__(parent)
        self._coord  = coordinator
        self._thread = None
        self._setup_ui()

    def _setup_ui(self):
        layout = QVBoxLayout(self)
        layout.setSpacing(4)
        layout.setContentsMargins(8, 8, 8, 8)

        self._list = QListWidget()
        layout.addWidget(self._list)

        btn_row = QHBoxLayout()
        self._run_btn     = QPushButton("▶ Run Selected")
        self._run_all_btn = QPushButton("▶▶ Run All")
        btn_row.addWidget(self._run_btn)
        btn_row.addWidget(self._run_all_btn)
        layout.addLayout(btn_row)

        sep = QFrame()
        sep.setFrameShape(QFrame.HLine)
        layout.addWidget(sep)

        self._status_label = QLabel("Status: idle")
        layout.addWidget(self._status_label)

        self._output = QTextEdit()
        self._output.setReadOnly(True)
        mono = QFont("Courier New", 9)
        mono.setStyleHint(QFont.Monospace)
        self._output.setFont(mono)
        layout.addWidget(self._output)

        self._run_btn.clicked.connect(self._on_run_selected)
        self._run_all_btn.clicked.connect(self._on_run_all)

        self._populate()

    def _populate(self):
        self._list.clear()
        try:
            from runner.runner import list_tests
            names = list_tests()
        except Exception as exc:
            self._list.addItem(f"(error: {exc})")
            return
        for name in names:
            item = QListWidgetItem(f"○  {name}")
            item.setData(Qt.UserRole, name)
            self._list.addItem(item)
        if not names:
            self._list.addItem("(no scenarios found)")

    def _on_run_selected(self):
        item = self._list.currentItem()
        if item is None:
            return
        name = item.data(Qt.UserRole)
        if not name:
            return
        self._run_scenario(name)

    def _on_run_all(self):
        try:
            from runner.runner import list_tests
            names = list_tests()
        except Exception:
            return
        for name in names:
            self._run_scenario(name)

    def _run_scenario(self, name: str):
        if not _QT:
            return
        self._status_label.setText(f"Status: running {name}…")
        self._output.clear()

        worker = _RunWorker(name)
        thread = QThread()
        worker.moveToThread(thread)
        worker.finished.connect(self._on_result)
        worker.finished.connect(thread.quit)
        thread.started.connect(worker.run)
        thread.start()
        self._thread = thread   # keep reference

    def _on_result(self, result) -> None:
        icon = "✓" if result.status == "pass" else "✗"
        self._status_label.setText(f"Status: {result.status}")
        self._output.append(f"{icon}  {result.summary()}")
        for ar in result.assertion_results:
            tick = "✓" if ar.passed else "✗"
            self._output.append(f"  {tick}  {ar.assertion_type}: {ar.message}")
        if result.error_message:
            self._output.append(f"  ERROR: {result.error_message}")
        self._output.append("")

        # Update list icon
        for i in range(self._list.count()):
            item = self._list.item(i)
            if item.data(Qt.UserRole) == result.scenario.name:
                tick = "✓" if result.status == "pass" else "✗"
                item.setText(f"{tick}  {result.scenario.name}")
                break
