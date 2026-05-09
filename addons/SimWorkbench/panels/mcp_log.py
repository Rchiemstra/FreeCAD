"""
mcp_log.py — MCP Activity Log dock panel.

Shows a scrolling log of MCP tool calls made by LLM agents.  The log is
populated by hooking into FreeCAD's existing MCP RPC server logging.

Layout::

    ┌─ MCP Activity ──────────────────────────────┐
    │  [Clear]                                     │
    │  ┌─────────────────────────────────────────┐ │
    │  │ 14:03:01 → spawn_model(arm_2dof) ok     │ │
    │  │ 14:03:02 → get_model_state(arm_2dof)    │ │
    │  └─────────────────────────────────────────┘ │
    └──────────────────────────────────────────────┘
"""
from __future__ import annotations

from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from sim_workbench import SimWorkbenchCoordinator

try:
    from PySide2.QtWidgets import (
        QWidget, QVBoxLayout, QHBoxLayout, QTextEdit, QPushButton,
    )
    from PySide2.QtGui import QFont
    _QT = True
except ImportError:
    _QT = False
    QWidget = object  # type: ignore


class MCPLogPanel(QWidget if _QT else object):  # type: ignore
    """Scrolling audit log of MCP agent tool calls."""

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

        btn_row = QHBoxLayout()
        clear_btn = QPushButton("Clear")
        clear_btn.clicked.connect(self._clear)
        btn_row.addWidget(clear_btn)
        btn_row.addStretch()
        layout.addLayout(btn_row)

        self._log = QTextEdit()
        self._log.setReadOnly(True)
        mono = QFont("Courier New", 9)
        mono.setStyleHint(QFont.Monospace)
        self._log.setFont(mono)
        layout.addWidget(self._log)

    def _clear(self):
        self._log.clear()

    def append(self, message: str) -> None:
        """Append a log line (thread-safe via Qt signal)."""
        if not _QT:
            return
        import datetime
        ts = datetime.datetime.now().strftime("%H:%M:%S")
        self._log.append(f"{ts}  {message}")
        sb = self._log.verticalScrollBar()
        sb.setValue(sb.maximum())
