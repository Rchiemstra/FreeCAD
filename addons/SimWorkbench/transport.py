"""
transport.py — shared Gazebo / ROS2 transport layer.

Polls Gazebo state via the bridge module and delivers updates to registered
callbacks. Runs on a QTimer inside FreeCAD (no threads needed for v1).

Usage::

    from transport import GazeboTransport, ModelState

    t = GazeboTransport()
    t.on_state_update(my_callback)   # callback(list[ModelState])
    t.on_status_change(my_cb)        # callback(ConnectionStatus)
    t.start()   # begins polling
    t.stop()

The transport wraps ``bridge.gazebo_bridge`` so the addon never imports
the bridge directly — this keeps the dependency on the bridge module
optional and makes the transport unit-testable with mocks.
"""
from __future__ import annotations

import os
import sys
import time
import logging
from dataclasses import dataclass, field
from enum import Enum, auto
from typing import Callable, Optional

log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Public data structures
# ---------------------------------------------------------------------------

@dataclass
class Pose:
    x: float = 0.0
    y: float = 0.0
    z: float = 0.0
    roll: float = 0.0
    pitch: float = 0.0
    yaw: float = 0.0


@dataclass
class JointState:
    name: str = ""
    position: float = 0.0      # radians or metres
    velocity: float = 0.0      # rad/s or m/s
    effort: float = 0.0        # N·m or N


@dataclass
class ModelState:
    model_name: str = ""
    pose: Pose = field(default_factory=Pose)
    joint_states: list[JointState] = field(default_factory=list)
    sim_time: float = 0.0      # seconds
    rtf: float = 0.0           # real-time factor (e.g. 0.95)


class ConnectionStatus(Enum):
    DISCONNECTED = auto()
    CONNECTING   = auto()
    CONNECTED    = auto()
    ERROR        = auto()


# ---------------------------------------------------------------------------
# GazeboTransport
# ---------------------------------------------------------------------------

class GazeboTransport:
    """
    Polls Gazebo for model states at a configurable interval.

    In FreeCAD context this should be driven by a QTimer::timeout signal
    connected to ``poll()``. The ``start()`` / ``stop()`` helpers manage
    that timer if FreeCAD / Qt is available; otherwise ``poll()`` can be
    called directly.

    Parameters
    ----------
    poll_interval_ms : int
        How often to query Gazebo (milliseconds). Default 100 ms → 10 Hz.
    model_names : list[str] | None
        Models to poll. If None, polls every model in the scene.
    bridge_module : module | None
        Inject a different bridge module for testing (default: bridge.gazebo_bridge).
    """

    def __init__(
        self,
        poll_interval_ms: int = 100,
        model_names: Optional[list[str]] = None,
        bridge_module=None,
    ):
        self._interval_ms  = poll_interval_ms
        self._model_names  = model_names or []
        self._bridge       = bridge_module
        self._status       = ConnectionStatus.DISCONNECTED
        self._timer        = None          # Qt timer, set in start()
        self._state_cbs:  list[Callable] = []
        self._status_cbs: list[Callable] = []
        self._running      = False
        self._last_poll    = 0.0
        self._session      = None          # GazeboSession context manager

    # ------------------------------------------------------------------
    # Callback registration
    # ------------------------------------------------------------------

    def on_state_update(self, cb: Callable[[list[ModelState]], None]) -> None:
        """Register a callback that receives a list of ModelState objects."""
        self._state_cbs.append(cb)

    def on_status_change(self, cb: Callable[[ConnectionStatus], None]) -> None:
        """Register a callback that receives ConnectionStatus changes."""
        self._status_cbs.append(cb)

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    def start(self) -> None:
        """Start polling. Creates a QTimer if running inside FreeCAD."""
        if self._running:
            return
        self._running = True
        self._bridge = self._bridge or self._load_bridge()
        self._set_status(ConnectionStatus.CONNECTING)
        try:
            from PySide2.QtCore import QTimer
            self._timer = QTimer()
            self._timer.timeout.connect(self.poll)
            self._timer.start(self._interval_ms)
        except ImportError:
            # Outside FreeCAD — caller must call poll() manually
            log.debug("PySide2 not available; timer not created")

    def stop(self) -> None:
        """Stop polling and disconnect from Gazebo."""
        self._running = False
        if self._timer is not None:
            try:
                self._timer.stop()
            except Exception:
                pass
            self._timer = None
        if self._session is not None:
            try:
                self._session.__exit__(None, None, None)
            except Exception:
                pass
            self._session = None
        # Always fire callbacks on explicit stop (even if already DISCONNECTED)
        self._status = ConnectionStatus.DISCONNECTED
        for cb in self._status_cbs:
            try:
                cb(ConnectionStatus.DISCONNECTED)
            except Exception as exc:
                log.warning("Status callback raised: %s", exc)

    # ------------------------------------------------------------------
    # Polling
    # ------------------------------------------------------------------

    def poll(self) -> list[ModelState]:
        """
        Query Gazebo once and fire state callbacks.

        Safe to call even when Gazebo is not running — returns [] and sets
        status to ERROR.
        """
        if not self._running:
            return []

        bridge = self._bridge
        if bridge is None:
            self._set_status(ConnectionStatus.ERROR)
            return []

        try:
            states = self._fetch_states(bridge)
            self._set_status(ConnectionStatus.CONNECTED)
            for cb in self._state_cbs:
                try:
                    cb(states)
                except Exception as exc:
                    log.warning("State callback raised: %s", exc)
            return states
        except Exception as exc:
            log.debug("Gazebo poll error: %s", exc)
            self._set_status(ConnectionStatus.ERROR)
            return []

    def _fetch_states(self, bridge) -> list[ModelState]:
        """Internal: call bridge and parse into ModelState objects.

        Raises ``RuntimeError`` if we had model names to query but every
        individual query failed, so that ``poll()`` can set ERROR status.
        """
        names = self._model_names
        if not names:
            # Try to get the list of models from Gazebo (best-effort)
            try:
                names = bridge.list_models()
            except Exception:
                names = []

        states: list[ModelState] = []
        errors: list[Exception] = []
        for name in names:
            try:
                raw = bridge.get_model_state(name)
                if hasattr(raw, "ok"):
                    if not raw.ok:
                        raise RuntimeError(
                            "; ".join(getattr(raw, "messages", [])) or "get_model_state failed"
                        )
                    raw = getattr(raw, "data", None) or {}
                states.append(self._parse_model_state(name, raw))
            except Exception as exc:
                log.debug("Could not get state for %r: %s", name, exc)
                errors.append(exc)

        # If we had targets but every one failed, surface an error.
        if names and not states:
            raise RuntimeError(
                f"All {len(errors)} model queries failed: {errors[0]}"
            )
        return states

    @staticmethod
    def _parse_model_state(name: str, raw: dict) -> ModelState:
        """Convert raw bridge dict → ModelState. Handles missing keys."""
        pose_raw = raw.get("pose", {})
        pos  = pose_raw.get("position", {})
        ori  = pose_raw.get("orientation", {})

        # Convert quaternion to RPY if present (simple small-angle approximation)
        qx = ori.get("x", 0.0)
        qy = ori.get("y", 0.0)
        qz = ori.get("z", 0.0)
        qw = ori.get("w", 1.0)
        roll, pitch, yaw = _quat_to_rpy(qx, qy, qz, qw)

        pose = Pose(
            x=pos.get("x", 0.0),
            y=pos.get("y", 0.0),
            z=pos.get("z", 0.0),
            roll=roll,
            pitch=pitch,
            yaw=yaw,
        )

        joint_states: list[JointState] = []
        for js in raw.get("joint_states", []):
            joint_states.append(JointState(
                name     =js.get("name", ""),
                position =js.get("position", 0.0),
                velocity =js.get("velocity", 0.0),
                effort   =js.get("effort", 0.0),
            ))

        return ModelState(
            model_name   =name,
            pose         =pose,
            joint_states =joint_states,
            sim_time     =raw.get("sim_time", 0.0),
            rtf          =raw.get("rtf", 0.0),
        )

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _set_status(self, new_status: ConnectionStatus) -> None:
        if new_status == self._status:
            return
        self._status = new_status
        for cb in self._status_cbs:
            try:
                cb(new_status)
            except Exception as exc:
                log.warning("Status callback raised: %s", exc)

    @staticmethod
    def _load_bridge():
        """Try to import bridge.gazebo_bridge; return None on failure."""
        try:
            from bridge import gazebo_bridge
            return gazebo_bridge
        except Exception as exc:
            log.debug("Could not load bridge.gazebo_bridge: %s", exc)
            return None

    @property
    def status(self) -> ConnectionStatus:
        return self._status

    @property
    def poll_interval_ms(self) -> int:
        return self._interval_ms


# ---------------------------------------------------------------------------
# Quaternion → RPY helper
# ---------------------------------------------------------------------------

import math

def _quat_to_rpy(qx: float, qy: float, qz: float, qw: float) -> tuple[float, float, float]:
    """Convert unit quaternion to roll-pitch-yaw (radians, ZYX convention)."""
    # roll (x-axis rotation)
    sinr_cosp = 2.0 * (qw * qx + qy * qz)
    cosr_cosp = 1.0 - 2.0 * (qx * qx + qy * qy)
    roll = math.atan2(sinr_cosp, cosr_cosp)

    # pitch (y-axis rotation)
    sinp = 2.0 * (qw * qy - qz * qx)
    pitch = math.copysign(math.pi / 2, sinp) if abs(sinp) >= 1.0 else math.asin(sinp)

    # yaw (z-axis rotation)
    siny_cosp = 2.0 * (qw * qz + qx * qy)
    cosy_cosp = 1.0 - 2.0 * (qy * qy + qz * qz)
    yaw = math.atan2(siny_cosp, cosy_cosp)

    return roll, pitch, yaw
