"""
tests/test_sim_workbench.py — Offline tests for the SimWorkbench addon.

These tests cover the logic layer only (transport.py, state_bridge.py).
FreeCAD GUI tests require a live FreeCAD instance and are auto-skipped.

Run with:
    pytest tests/test_sim_workbench.py -v
"""
from __future__ import annotations

import math
import sys
import os
import pytest

# ---------------------------------------------------------------------------
# Make addon directory importable without installing it
# ---------------------------------------------------------------------------
ADDON_DIR = os.path.join(os.path.dirname(__file__), "..", "addons", "SimWorkbench")
sys.path.insert(0, os.path.abspath(ADDON_DIR))


# ===========================================================================
# Transport tests (no Gazebo / FreeCAD needed)
# ===========================================================================

class TestQuat2RPY:
    """Test the quaternion → roll-pitch-yaw conversion."""

    def setup_method(self):
        from transport import _quat_to_rpy
        self.rpy = _quat_to_rpy

    def test_identity_is_zero_rpy(self):
        r, p, y = self.rpy(0, 0, 0, 1)
        assert abs(r) < 1e-9
        assert abs(p) < 1e-9
        assert abs(y) < 1e-9

    def test_90_yaw(self):
        # Rotation of 90° around Z: quat = (0, 0, sin45, cos45)
        s = math.sqrt(0.5)
        r, p, y = self.rpy(0, 0, s, s)
        assert abs(r) < 1e-9
        assert abs(p) < 1e-9
        assert abs(y - math.pi / 2) < 1e-9

    def test_90_roll(self):
        # Rotation of 90° around X: quat = (sin45, 0, 0, cos45)
        s = math.sqrt(0.5)
        r, p, y = self.rpy(s, 0, 0, s)
        assert abs(r - math.pi / 2) < 1e-9
        assert abs(p) < 1e-9
        assert abs(y) < 1e-9

    def test_negative_yaw(self):
        s = math.sqrt(0.5)
        r, p, y = self.rpy(0, 0, -s, s)
        assert abs(y - (-math.pi / 2)) < 1e-9


class TestModelStateParsing:
    """GazeboTransport._parse_model_state — no bridge needed."""

    def setup_method(self):
        from transport import GazeboTransport
        self.parse = GazeboTransport._parse_model_state

    def test_empty_raw_gives_zero_pose(self):
        state = self.parse("robot1", {})
        assert state.model_name == "robot1"
        assert state.pose.x == 0.0
        assert state.pose.y == 0.0
        assert state.pose.z == 0.0

    def test_position_extracted(self):
        raw = {"pose": {"position": {"x": 1.0, "y": 2.0, "z": 3.0}}}
        state = self.parse("arm", raw)
        assert state.pose.x == pytest.approx(1.0)
        assert state.pose.y == pytest.approx(2.0)
        assert state.pose.z == pytest.approx(3.0)

    def test_orientation_quaternion_converted_to_rpy(self):
        s = math.sqrt(0.5)
        raw = {"pose": {"orientation": {"x": 0, "y": 0, "z": s, "w": s}}}
        state = self.parse("arm", raw)
        assert abs(state.pose.yaw - math.pi / 2) < 1e-9

    def test_joint_states_parsed(self):
        raw = {
            "joint_states": [
                {"name": "j1", "position": 0.5, "velocity": 0.1, "effort": 2.0},
                {"name": "j2", "position": -0.3},
            ]
        }
        state = self.parse("arm", raw)
        assert len(state.joint_states) == 2
        assert state.joint_states[0].name     == "j1"
        assert state.joint_states[0].position == pytest.approx(0.5)
        assert state.joint_states[0].effort   == pytest.approx(2.0)
        assert state.joint_states[1].name     == "j2"
        assert state.joint_states[1].position == pytest.approx(-0.3)

    def test_sim_time_and_rtf(self):
        raw = {"sim_time": 12.345, "rtf": 0.97}
        state = self.parse("arm", raw)
        assert state.sim_time == pytest.approx(12.345)
        assert state.rtf      == pytest.approx(0.97)

    def test_missing_joint_fields_default_to_zero(self):
        raw = {"joint_states": [{"name": "j1"}]}
        state = self.parse("arm", raw)
        js = state.joint_states[0]
        assert js.position == 0.0
        assert js.velocity == 0.0
        assert js.effort   == 0.0


class TestGazeboTransportOffline:
    """GazeboTransport with a mock bridge — no real Gazebo needed."""

    def _make_mock_bridge(self, models=None):
        """Return a minimal namespace that looks like bridge.gazebo_bridge."""
        states = models or {}

        class MockBridge:
            @staticmethod
            def list_models():
                return list(states.keys())

            @staticmethod
            def get_model_state(name):
                return states.get(name, {})

        return MockBridge()

    def test_poll_returns_states_for_each_model(self):
        from transport import GazeboTransport

        bridge = self._make_mock_bridge({
            "arm": {"pose": {"position": {"x": 1.0, "y": 0.0, "z": 0.0}}},
        })
        t = GazeboTransport(bridge_module=bridge, model_names=["arm"])
        t._running = True

        results = t.poll()
        assert len(results) == 1
        assert results[0].model_name == "arm"
        assert results[0].pose.x == pytest.approx(1.0)

    def test_poll_fires_state_callbacks(self):
        from transport import GazeboTransport

        bridge = self._make_mock_bridge({"robot": {}})
        t = GazeboTransport(bridge_module=bridge, model_names=["robot"])
        t._running = True

        received = []
        t.on_state_update(received.extend)
        t.poll()

        assert len(received) == 1
        assert received[0].model_name == "robot"

    def test_poll_empty_bridge_returns_empty(self):
        from transport import GazeboTransport

        bridge = self._make_mock_bridge({})
        t = GazeboTransport(bridge_module=bridge, model_names=[])
        t._running = True

        results = t.poll()
        assert results == []

    def test_status_changes_on_poll_success(self):
        from transport import GazeboTransport, ConnectionStatus

        bridge = self._make_mock_bridge({"r": {}})
        t = GazeboTransport(bridge_module=bridge, model_names=["r"])
        t._running = True

        statuses = []
        t.on_status_change(statuses.append)
        t.poll()

        assert ConnectionStatus.CONNECTED in statuses

    def test_status_changes_on_bridge_error(self):
        from transport import GazeboTransport, ConnectionStatus

        class BrokenBridge:
            @staticmethod
            def list_models():
                raise RuntimeError("Gazebo not running")

            @staticmethod
            def get_model_state(name):
                raise RuntimeError("Gazebo not running")

        t = GazeboTransport(bridge_module=BrokenBridge(), model_names=["arm"])
        t._running = True

        statuses = []
        t.on_status_change(statuses.append)
        t.poll()

        assert ConnectionStatus.ERROR in statuses

    def test_stop_sets_disconnected(self):
        from transport import GazeboTransport, ConnectionStatus

        t = GazeboTransport()
        t._running = True
        statuses = []
        t.on_status_change(statuses.append)
        t.stop()

        assert not t._running
        assert ConnectionStatus.DISCONNECTED in statuses


# ===========================================================================
# StateBridge tests (no FreeCAD / Gazebo needed)
# ===========================================================================

class TestStateBridgeOffline:
    """StateBridge with FreeCAD mocked out."""

    def _make_transport_and_bridge(self):
        """Return (transport, state_bridge) with a mock bridge module."""
        from transport import GazeboTransport

        class MockBridge:
            @staticmethod
            def list_models():
                return []

            @staticmethod
            def get_model_state(name):
                return {}

        t = GazeboTransport(bridge_module=MockBridge(), model_names=[])

        from state_bridge import StateBridge
        sb = StateBridge(t)   # doc=None — will get None from _resolve_doc
        sb.start()
        return t, sb

    def test_start_registers_callbacks(self):
        from transport import GazeboTransport

        class MockBridge:
            @staticmethod
            def list_models(): return []

            @staticmethod
            def get_model_state(n): return {}

        t = GazeboTransport(bridge_module=MockBridge())

        from state_bridge import StateBridge
        sb = StateBridge(t)
        sb.start()

        assert len(t._state_cbs) == 1

    def test_model_state_to_placement_dict_when_no_freecad(self):
        """Outside FreeCAD, _model_state_to_placement returns a dict."""
        from state_bridge import StateBridge
        from transport import ModelState, Pose

        sb = StateBridge(transport=None)  # type: ignore
        state = ModelState(
            model_name="arm",
            pose=Pose(x=1.0, y=2.0, z=0.5, roll=0.0, pitch=0.0, yaw=1.5708),
        )
        placement = sb._model_state_to_placement(state)
        assert isinstance(placement, dict)
        pos = placement["position"]
        # scale is 1000 (mm)
        assert pos["x"] == pytest.approx(1000.0)
        assert pos["y"] == pytest.approx(2000.0)
        assert pos["z"] == pytest.approx( 500.0)
        rot = placement["rotation_deg"]
        assert abs(rot["yaw"] - math.degrees(1.5708)) < 0.01

    def test_last_states_initially_empty(self):
        t, sb = self._make_transport_and_bridge()
        assert sb.last_states == []

    def test_custom_scale(self):
        from state_bridge import StateBridge
        from transport import ModelState, Pose

        sb = StateBridge(transport=None, scale=1.0)  # type: ignore  # no scaling
        state = ModelState(
            model_name="arm",
            pose=Pose(x=2.5, y=0.0, z=0.0),
        )
        placement = sb._model_state_to_placement(state)
        assert placement["position"]["x"] == pytest.approx(2.5)

    def test_model_map_renames(self):
        """model_map overrides the Gazebo model name when looking up FreeCAD objects."""
        from state_bridge import StateBridge

        sb = StateBridge(transport=None, model_map={"gazebo_arm": "fc_arm"})  # type: ignore

        # Simulate _apply_state finding no matching object (doc=None → _resolve_doc returns None)
        from transport import ModelState, Pose
        state = ModelState("gazebo_arm", Pose())
        # Should not raise even when doc is None
        sb._apply_state(None, state)  # doc=None → returns early


# ===========================================================================
# Installation helper tests
# ===========================================================================

class TestInstallAddon:
    """install_addon.py logic — filesystem test (no FreeCAD needed)."""

    def test_install_copies_files(self, tmp_path):
        """install() should copy all addon files to the destination."""
        import shutil
        import importlib.util

        # Load install_addon from the addon dir
        install_path = os.path.join(ADDON_DIR, "install_addon.py")
        spec = importlib.util.spec_from_file_location("install_addon", install_path)
        mod  = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(mod)

        # Monkey-patch find_freecad_mod_dir to point to tmp_path
        original = mod.find_freecad_mod_dir
        mod.find_freecad_mod_dir = lambda: tmp_path

        mod.install()

        dst = tmp_path / "SimWorkbench"
        assert (dst / "InitGui.py").exists()
        assert (dst / "transport.py").exists()
        assert (dst / "state_bridge.py").exists()
        assert (dst / "sim_workbench.py").exists()
        assert (dst / "panels" / "sim_controls.py").exists()

        mod.find_freecad_mod_dir = original
