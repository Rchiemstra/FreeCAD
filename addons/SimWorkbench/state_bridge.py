"""
state_bridge.py — Live State Bridge.

Translates Gazebo model states into FreeCAD object placements so the 3D
viewer reflects the live simulation without the Gazebo GUI.

Usage (inside FreeCAD)::

    from state_bridge import StateBridge

    bridge = StateBridge(transport, doc=FreeCAD.ActiveDocument)
    bridge.start()   # begins listening to transport
    bridge.stop()

The bridge maps each Gazebo model by name to a FreeCAD object in the
active document.  The mapping is:

    Gazebo model name  →  FreeCAD object Label
    "arm_2dof"         →  FreeCAD object with Label == "arm_2dof"

When a matching FreeCAD object is found its ``Placement`` is updated
every time the transport delivers a new state.  Unknown models are silently
ignored (they may be ground planes, sensors, etc.).

Joint state mirroring:

    If the FreeCAD document contains a FreeCAD Assembly/Link with matching
    joint names they are also updated. v1: joint state display is limited
    to the sensor plot panel; CAD joint motion is a v2 feature.
"""
from __future__ import annotations

import logging
import math
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from transport import GazeboTransport, ModelState, ConnectionStatus

log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# StateBridge
# ---------------------------------------------------------------------------

class StateBridge:
    """
    Applies Gazebo ModelState objects to FreeCAD object Placements.

    Parameters
    ----------
    transport : GazeboTransport
        The transport instance that emits state updates.
    doc : FreeCAD.Document | None
        Target FreeCAD document. Defaults to ``FreeCAD.ActiveDocument`` at
        the time of the first state update (not at construction time).
    model_map : dict[str, str] | None
        Optional explicit mapping ``{gazebo_model: freecad_label}``.
        If None, model names are matched to FreeCAD labels directly.
    scale : float
        Unit scale from Gazebo (metres) to FreeCAD internal units (mm).
        FreeCAD stores positions in mm; Gazebo uses metres → scale = 1000.
    """

    # FreeCAD stores positions in mm; Gazebo outputs metres.
    _GAZEBO_TO_FC_MM = 1000.0

    def __init__(
        self,
        transport: "GazeboTransport",
        doc=None,
        model_map: dict[str, str] | None = None,
        scale: float = _GAZEBO_TO_FC_MM,
    ):
        self._transport  = transport
        self._doc        = doc
        self._model_map  = model_map or {}
        self._scale      = scale
        self._running    = False
        self._last_states: list["ModelState"] = []

    # ------------------------------------------------------------------
    # Lifecycle
    # ------------------------------------------------------------------

    def start(self) -> None:
        if self._running:
            return
        self._running = True
        self._transport.on_state_update(self._on_state)
        self._transport.on_status_change(self._on_status)

    def stop(self) -> None:
        self._running = False

    # ------------------------------------------------------------------
    # Callbacks
    # ------------------------------------------------------------------

    def _on_state(self, states: "list[ModelState]") -> None:
        if not self._running:
            return
        self._last_states = states
        doc = self._resolve_doc()
        if doc is None:
            return
        for state in states:
            self._apply_state(doc, state)

    def _on_status(self, status: "ConnectionStatus") -> None:
        from transport import ConnectionStatus
        if status == ConnectionStatus.DISCONNECTED:
            log.info("[StateBridge] Gazebo disconnected")
        elif status == ConnectionStatus.CONNECTED:
            log.info("[StateBridge] Gazebo connected")

    # ------------------------------------------------------------------
    # FreeCAD placement update
    # ------------------------------------------------------------------

    def _apply_state(self, doc, state: "ModelState") -> None:
        """Map one Gazebo model state to the matching FreeCAD object."""
        label = self._model_map.get(state.model_name, state.model_name)
        obj = self._find_object(doc, label)
        if obj is None:
            return
        placement = self._model_state_to_placement(state)
        try:
            obj.Placement = placement
        except Exception as exc:
            log.debug("Could not set Placement on %r: %s", label, exc)

    @staticmethod
    def _find_object(doc, label: str):
        """Return the first FreeCAD object with the given Label, or None."""
        try:
            for obj in doc.Objects:
                if obj.Label == label:
                    return obj
        except Exception:
            pass
        return None

    def _model_state_to_placement(self, state: "ModelState"):
        """
        Convert a ModelState to a FreeCAD Placement.

        FreeCAD Placement: Vector(mm) + Rotation (Euler XYZ).
        Gazebo pose: metres + quaternion (ZYX RPY).
        """
        s = self._scale
        try:
            import FreeCAD as FC
            vec = FC.Vector(
                state.pose.x * s,
                state.pose.y * s,
                state.pose.z * s,
            )
            rot = FC.Rotation(
                math.degrees(state.pose.yaw),   # Yaw  (Z)
                math.degrees(state.pose.pitch),  # Pitch (Y)
                math.degrees(state.pose.roll),   # Roll  (X)
            )
            return FC.Placement(vec, rot)
        except ImportError:
            # Not inside FreeCAD — return a plain dict for testing
            return {
                "position": {
                    "x": state.pose.x * s,
                    "y": state.pose.y * s,
                    "z": state.pose.z * s,
                },
                "rotation_deg": {
                    "yaw":   math.degrees(state.pose.yaw),
                    "pitch": math.degrees(state.pose.pitch),
                    "roll":  math.degrees(state.pose.roll),
                },
            }

    # ------------------------------------------------------------------
    # Query helpers (for panels)
    # ------------------------------------------------------------------

    @property
    def last_states(self) -> list:
        """Most-recently-received list of ModelState objects."""
        return list(self._last_states)

    def _resolve_doc(self):
        """Return the target document, falling back to ActiveDocument."""
        if self._doc is not None:
            return self._doc
        try:
            import FreeCAD
            return FreeCAD.ActiveDocument
        except ImportError:
            return None
