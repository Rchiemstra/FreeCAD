# SPDX-License-Identifier: LGPL-2.1-or-later
"""QObject dispatcher for Part 3 LocalUserDriver."""

from __future__ import annotations

import threading
from typing import Any

from PySide import QtCore

try:
    from . import actions
except ImportError:
    import actions

try:
    queued = QtCore.Qt.ConnectionType.QueuedConnection
except AttributeError:
    queued = QtCore.Qt.QueuedConnection


class LocalUserDriverCore(QtCore.QObject):
    """Execute personal-view actions on the Qt owner thread."""

    action_requested = QtCore.Signal(str, dict, str)

    def __init__(self, parent: QtCore.QObject | None = None) -> None:
        super().__init__(parent)
        self._owner_thread_ident = threading.get_ident()
        self._results: dict[str, dict[str, Any]] = {}
        self._events: dict[str, threading.Event] = {}
        self._lock = threading.Lock()
        self.action_requested.connect(self._run_action, queued)

    def submit_from_thread(
        self,
        action: str,
        params: dict[str, Any],
        operation_id: str,
    ) -> dict[str, Any]:
        event = threading.Event()
        with self._lock:
            self._events[operation_id] = event
        self.action_requested.emit(operation_id, params, action)
        event.wait()
        with self._lock:
            result = self._results.pop(operation_id, None)
            self._events.pop(operation_id, None)
        if result is None:
            raise RuntimeError(f"action {action!r} produced no acknowledgement")
        return result

    @QtCore.Slot(str, dict, str)
    def _run_action(self, operation_id: str, params: dict[str, Any], action: str) -> None:
        if threading.get_ident() != self._owner_thread_ident:
            payload = {
                "operation_id": operation_id,
                "success": False,
                "error": "action ran off the Qt owner thread",
            }
        else:
            try:
                observed = actions.execute(action, params)
                payload = {
                    "operation_id": operation_id,
                    "success": True,
                    "result": observed,
                }
            except Exception as exc:  # noqa: BLE001 - report to coordinator
                payload = {
                    "operation_id": operation_id,
                    "success": False,
                    "error": str(exc),
                }
        with self._lock:
            self._results[operation_id] = payload
            event = self._events.get(operation_id)
        if event is not None:
            event.set()
