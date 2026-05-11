"""
gazebo_status_panel.py — Gazebo / gazebo-mcp status and camera snapshot panel.

Shows transport connection state (from :class:`GazeboTransport`), runs
``bridge.gazebo_bridge.get_simulation_status`` on refresh, and saves a camera
frame via ``capture_camera_snapshot`` (requires a camera sensor in the world
or ``SIMWORKBENCH_GAZEBO_CAMERA_SENSOR``).

This is not an embedded Gazebo GUI viewport — only status text and optional
sensor image preview / file path.
"""
from __future__ import annotations

from pathlib import Path
from typing import TYPE_CHECKING, Optional

if TYPE_CHECKING:
    from sim_workbench import SimWorkbenchCoordinator

try:
    from PySide2.QtWidgets import (
        QWidget,
        QVBoxLayout,
        QHBoxLayout,
        QPushButton,
        QLabel,
        QFrame,
        QTextEdit,
        QLineEdit,
        QScrollArea,
    )
    from PySide2.QtCore import Qt
    from PySide2.QtGui import QImage, QPixmap

    _QT = True
except ImportError:
    _QT = False
    QWidget = object  # type: ignore


class GazeboStatusPanel(QWidget if _QT else object):  # type: ignore
    """Dock panel: connection + MCP status refresh + camera screenshot."""

    def __init__(self, coordinator: "SimWorkbenchCoordinator", parent=None):
        if not _QT:
            self._coord = coordinator
            return
        super().__init__(parent)
        self._coord = coordinator
        self._transport_label_text = "Disconnected"
        self._last_mcp_ok: Optional[bool] = None
        self._last_mcp_msg: str = ""
        self._last_image_path: Optional[Path] = None
        self._setup_ui()

    def _setup_ui(self) -> None:
        layout = QVBoxLayout(self)
        layout.setSpacing(4)
        layout.setContentsMargins(8, 8, 8, 8)

        title = QLabel("<b>Gazebo status &amp; screenshot</b>")
        layout.addWidget(title)

        self._heading = QLabel("Transport: …  |  MCP: not queried yet")
        self._heading.setWordWrap(True)
        layout.addWidget(self._heading)

        sep1 = QFrame()
        sep1.setFrameShape(QFrame.HLine)
        layout.addWidget(sep1)

        detail_label = QLabel("Simulation status (JSON)")
        layout.addWidget(detail_label)

        self._detail = QTextEdit()
        self._detail.setReadOnly(True)
        self._detail.setMinimumHeight(120)
        self._detail.setPlaceholderText(
            "Click “Refresh status” to query Gazebo via gazebo-mcp "
            "(headless sim must be running)."
        )
        layout.addWidget(self._detail)

        btn_row = QHBoxLayout()
        self._btn_refresh = QPushButton("Refresh status")
        self._btn_shot = QPushButton("Capture screenshot")
        btn_row.addWidget(self._btn_refresh)
        btn_row.addWidget(self._btn_shot)
        layout.addLayout(btn_row)

        sensor_row = QHBoxLayout()
        sensor_row.addWidget(QLabel("Sensor:"))
        self._sensor_edit = QLineEdit()
        self._sensor_edit.setPlaceholderText(
            "optional — auto-pick camera or set SIMWORKBENCH_GAZEBO_CAMERA_SENSOR"
        )
        sensor_row.addWidget(self._sensor_edit)
        layout.addLayout(sensor_row)

        self._path_label = QLabel("")
        self._path_label.setWordWrap(True)
        self._path_label.setTextInteractionFlags(Qt.TextSelectableByMouse)
        layout.addWidget(self._path_label)

        self._preview = QLabel("(no screenshot yet)")
        self._preview.setAlignment(Qt.AlignCenter)
        self._preview.setMinimumHeight(160)
        self._preview.setStyleSheet("background: #2b2b2b; color: #ccc;")
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setWidget(self._preview)
        scroll.setMaximumHeight(220)
        layout.addWidget(scroll)

        layout.addStretch()

        self._btn_refresh.clicked.connect(self._on_refresh_status)
        self._btn_shot.clicked.connect(self._on_capture)
        self._render_heading()

    # ------------------------------------------------------------------
    # Transport updates (from GazeboTransport)
    # ------------------------------------------------------------------

    def update_transport_status(self, status) -> None:
        """Sync with :class:`transport.ConnectionStatus` from the coordinator."""
        if not _QT:
            return
        from transport import ConnectionStatus

        label_map = {
            ConnectionStatus.DISCONNECTED: "Disconnected",
            ConnectionStatus.CONNECTING: "Connecting…",
            ConnectionStatus.CONNECTED: "Connected",
            ConnectionStatus.ERROR: "Error",
        }
        self._transport_label_text = label_map.get(status, str(status))
        self._render_heading()

    def _render_heading(self) -> None:
        from panels.gazebo_status_logic import combined_status_heading

        self._heading.setText(
            combined_status_heading(
                self._transport_label_text,
                self._last_mcp_ok,
                self._last_mcp_msg,
            )
        )
        if self._last_mcp_ok is False:
            self._heading.setStyleSheet("color: #f44336;")
        elif self._last_mcp_ok is True:
            self._heading.setStyleSheet("color: #4caf50;")
        else:
            self._heading.setStyleSheet("")

    # ------------------------------------------------------------------
    # Actions
    # ------------------------------------------------------------------

    def _log_panel(
        self,
        action: str,
        ok: bool,
        message: str = "",
        exc: Optional[BaseException] = None,
    ) -> None:
        try:
            from bridge import structured_log

            line = structured_log.log_panel_mcp_status(
                component="addons.SimWorkbench.gazebo_status_panel",
                action=action,
                ok=ok,
                message=message,
                exc_type=type(exc).__name__ if exc else None,
                exc_message=str(exc) if exc else None,
            )
            import FreeCAD  # type: ignore

            FreeCAD.Console.PrintMessage(line + "\n")
        except Exception:
            pass

    def _set_busy(self, busy: bool, message: str = "") -> None:
        if not _QT:
            return
        self._btn_refresh.setEnabled(not busy)
        self._btn_shot.setEnabled(not busy)
        if busy:
            self._detail.setPlaceholderText(message or "Loading…")
        else:
            self._detail.setPlaceholderText(
                "Click “Refresh status” to query Gazebo via gazebo-mcp "
                "(headless sim must be running)."
            )

    def _on_refresh_status(self) -> None:
        if not _QT:
            return
        self._set_busy(True, "Querying gazebo-mcp…")
        try:
            from bridge import gazebo_bridge
            from panels.gazebo_status_logic import format_simulation_status_detail

            res = gazebo_bridge.get_simulation_status(timeout=12.0)
            ok = bool(res.ok)
            msg = "; ".join(res.messages) if res.messages else ""
            self._last_mcp_ok = ok
            self._last_mcp_msg = msg
            self._render_heading()
            self._detail.setPlainText(format_simulation_status_detail(res.data))
            if not ok:
                self._path_label.setText("")
            self._log_panel("refresh_status", ok, msg)
        except Exception as exc:
            self._last_mcp_ok = False
            self._last_mcp_msg = str(exc)
            self._render_heading()
            self._detail.setPlainText(str(exc))
            self._log_panel("refresh_status", False, str(exc), exc)
        finally:
            self._set_busy(False)

    def _on_capture(self) -> None:
        if not _QT:
            return
        self._set_busy(True, "Requesting camera frame…")
        self._preview.setText("Capturing…")
        try:
            from bridge import gazebo_bridge

            name = self._sensor_edit.text().strip() or None
            res = gazebo_bridge.capture_camera_snapshot(sensor_name=name, timeout=40.0)
            msg = "; ".join(res.messages) if res.messages else ""
            self._last_mcp_ok = bool(res.ok)
            self._last_mcp_msg = msg
            self._render_heading()
            if res.ok and res.data and res.data.get("path"):
                p = Path(res.data["path"])
                self._last_image_path = p
                d = res.data
                bits = [f"Saved: {p}"]
                mode = d.get("camera_source_mode")
                if mode:
                    bits.append(f"source: {mode}")
                iw, ih = d.get("image_width"), d.get("image_height")
                if iw is not None and ih is not None:
                    bits.append(f"size: {iw}×{ih}")
                if d.get("gz_image_topic"):
                    bits.append(f"topic: {d['gz_image_topic']}")
                self._path_label.setText("  |  ".join(bits))
                self._load_preview(p)
                self._log_panel("capture_screenshot", True, "  |  ".join(bits))
            else:
                self._path_label.setText(msg or "Screenshot failed")
                self._preview.setText("(no image)")
                self._log_panel("capture_screenshot", False, msg or "Screenshot failed")
        except Exception as exc:
            self._last_mcp_ok = False
            self._last_mcp_msg = str(exc)
            self._render_heading()
            self._path_label.setText(str(exc))
            self._preview.setText("(error)")
            self._log_panel("capture_screenshot", False, str(exc), exc)
        finally:
            self._set_busy(False)

    def _load_preview(self, path: Path) -> None:
        if not _QT:
            return
        img = QImage(str(path))
        if img.isNull():
            self._preview.setText("Could not decode image")
            return
        pix = QPixmap.fromImage(img).scaled(
            360,
            220,
            Qt.KeepAspectRatio,
            Qt.SmoothTransformation,
        )
        self._preview.setPixmap(pix)
