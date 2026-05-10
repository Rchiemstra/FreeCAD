#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""
tests/test_bridge.py — Smoke tests for the Phase 2 bridge module.

Tests the bridge Python package without requiring FreeCAD or Gazebo to be
running. Where live connections are needed, tests are marked with
pytest.mark.needs_freecad or pytest.mark.needs_gazebo and skipped by default.

Run:
    # All offline tests (no FreeCAD / Gazebo needed):
    python -m pytest tests/test_bridge.py -v

    # Include FreeCAD tests (FreeCAD must be running with MCP addon active):
    python -m pytest tests/test_bridge.py -v -m freecad

    # Include Gazebo tests (Gazebo Docker must be running):
    python -m pytest tests/test_bridge.py -v -m gazebo

    # Everything:
    python -m pytest tests/test_bridge.py -v -m ""
"""

from __future__ import annotations

import sys
from pathlib import Path
import pytest

# Add repo root to path so `bridge` is importable without installation
REPO_ROOT = Path(__file__).parent.parent.resolve()
sys.path.insert(0, str(REPO_ROOT))

# ── Fixtures ────────────────────────────────────────────────────────────────────

@pytest.fixture
def project():
    """Load the real project.yaml from the repository root."""
    from bridge.project import load_project
    return load_project(REPO_ROOT / "project.yaml")


@pytest.fixture
def urdf_path():
    return REPO_ROOT / "robots" / "arm_2dof.urdf"


@pytest.fixture
def sdf_path():
    return REPO_ROOT / "worlds" / "empty_world.sdf"


# ══════════════════════════════════════════════════════════════════════════════
#  bridge.project
# ══════════════════════════════════════════════════════════════════════════════

class TestProjectLoader:
    def test_load_project_returns_config(self, project):
        assert project.name == "freecad-gazebo-mcp"
        assert project.version

    def test_paths_are_absolute(self, project):
        for attr in ("robots", "worlds", "generated", "tests", "sim_runs", "config"):
            p = getattr(project.paths, attr)
            assert p.is_absolute(), f"paths.{attr} should be absolute"

    def test_robots_dir_exists(self, project):
        assert project.paths.robots.exists()

    def test_worlds_dir_exists(self, project):
        assert project.paths.worlds.exists()

    def test_mcp_freecad_config(self, project):
        fc = project.mcp.freecad
        assert fc.server_package == "freecad-mcp"
        assert fc.port == 9875

    def test_mcp_gazebo_config(self, project):
        gz = project.mcp.gazebo
        assert gz.server_package == "gazebo-mcp"
        assert gz.requires_ros2 is True

    def test_load_missing_file_raises(self, tmp_path):
        from bridge.project import load_project
        with pytest.raises(FileNotFoundError):
            load_project(tmp_path / "nonexistent.yaml")

    def test_load_malformed_yaml_raises(self, tmp_path):
        from bridge.project import load_project
        bad = tmp_path / "project.yaml"
        bad.write_text("- this\n- is\n- a list\n")
        with pytest.raises(ValueError):
            load_project(bad)


# ══════════════════════════════════════════════════════════════════════════════
#  bridge.validate — URDF
# ══════════════════════════════════════════════════════════════════════════════

class TestValidateURDF:
    def test_arm_2dof_urdf_is_valid(self, urdf_path):
        from bridge.validate import validate_urdf
        result = validate_urdf(urdf_path)
        assert result.ok, f"URDF validation failed:\n{result.summary()}"

    def test_arm_2dof_no_errors(self, urdf_path):
        from bridge.validate import validate_urdf
        result = validate_urdf(urdf_path)
        assert result.errors == [], f"Unexpected errors: {result.errors}"

    def test_missing_file(self, tmp_path):
        from bridge.validate import validate_urdf
        r = validate_urdf(tmp_path / "missing.urdf")
        assert not r.ok
        assert any("not found" in e.lower() for e in r.errors)

    def test_xml_parse_error(self, tmp_path):
        from bridge.validate import validate_urdf
        bad = tmp_path / "bad.urdf"
        bad.write_text("<robot><unclosed>")
        r = validate_urdf(bad)
        assert not r.ok
        assert any("parse" in e.lower() for e in r.errors)

    def test_wrong_root_element(self, tmp_path):
        from bridge.validate import validate_urdf
        bad = tmp_path / "bad.urdf"
        bad.write_text('<?xml version="1.0"?><sdf/>')
        r = validate_urdf(bad)
        assert not r.ok
        assert any("robot" in e.lower() for e in r.errors)

    def test_zero_inertia_detected(self, tmp_path):
        from bridge.validate import validate_urdf
        urdf = tmp_path / "zero_inertia.urdf"
        urdf.write_text("""<?xml version="1.0"?>
<robot name="test">
  <link name="base_link"/>
  <joint name="joint1" type="revolute">
    <parent link="base_link"/><child link="link1"/>
    <axis xyz="0 0 1"/>
    <limit lower="-1.5" upper="1.5" effort="10" velocity="1"/>
  </joint>
  <link name="link1">
    <inertial>
      <mass value="1.0"/>
      <inertia ixx="0" ixy="0" ixz="0" iyy="0" iyz="0" izz="0"/>
    </inertial>
  </link>
</robot>""")
        r = validate_urdf(urdf)
        assert not r.ok
        assert any("zero" in e.lower() or "inertia" in e.lower() for e in r.errors)

    def test_non_unit_axis_detected(self, tmp_path):
        from bridge.validate import validate_urdf
        urdf = tmp_path / "bad_axis.urdf"
        urdf.write_text("""<?xml version="1.0"?>
<robot name="test">
  <link name="base_link"/>
  <joint name="joint1" type="revolute">
    <parent link="base_link"/><child link="link1"/>
    <axis xyz="1 1 0"/>
    <limit lower="-1.5" upper="1.5" effort="10" velocity="1"/>
  </joint>
  <link name="link1">
    <inertial>
      <mass value="1.0"/>
      <inertia ixx="0.01" ixy="0" ixz="0" iyy="0.01" iyz="0" izz="0.01"/>
    </inertial>
  </link>
</robot>""")
        r = validate_urdf(urdf)
        assert not r.ok
        assert any("unit" in e.lower() or "axis" in e.lower() for e in r.errors)

    def test_absolute_mesh_path_detected(self, tmp_path):
        from bridge.validate import validate_urdf
        urdf = tmp_path / "abs_mesh.urdf"
        urdf.write_text("""<?xml version="1.0"?>
<robot name="test">
  <link name="base_link">
    <visual>
      <geometry><mesh filename="/absolute/path/robot.stl"/></geometry>
    </visual>
  </link>
</robot>""")
        r = validate_urdf(urdf)
        assert not r.ok
        assert any("absolute" in e.lower() or "portability" in e.lower() for e in r.errors)

    def test_unknown_joint_type_detected(self, tmp_path):
        from bridge.validate import validate_urdf
        urdf = tmp_path / "bad_joint.urdf"
        urdf.write_text("""<?xml version="1.0"?>
<robot name="test">
  <link name="base_link"/>
  <joint name="j1" type="magic">
    <parent link="base_link"/><child link="link1"/>
  </joint>
  <link name="link1"/>
</robot>""")
        r = validate_urdf(urdf)
        assert not r.ok
        assert any("unknown" in e.lower() or "type" in e.lower() for e in r.errors)

    def test_missing_inertial_warns(self, tmp_path):
        from bridge.validate import validate_urdf
        urdf = tmp_path / "no_inertial.urdf"
        urdf.write_text("""<?xml version="1.0"?>
<robot name="test">
  <link name="base_link"/>
  <joint name="j1" type="revolute">
    <parent link="base_link"/><child link="link1"/>
    <axis xyz="0 0 1"/>
    <limit lower="-1" upper="1" effort="1" velocity="1"/>
  </joint>
  <link name="link1"/>
</robot>""")
        r = validate_urdf(urdf)
        # No <inertial> on a non-fixed link should produce a warning (not a hard error)
        assert any("inertial" in w.lower() for w in r.warnings)


# ══════════════════════════════════════════════════════════════════════════════
#  bridge.validate — SDF
# ══════════════════════════════════════════════════════════════════════════════

class TestValidateSDF:
    def test_empty_world_sdf_is_valid(self, sdf_path):
        from bridge.validate import validate_sdf
        result = validate_sdf(sdf_path)
        assert result.ok, f"SDF validation failed:\n{result.summary()}"

    def test_empty_world_no_errors(self, sdf_path):
        from bridge.validate import validate_sdf
        result = validate_sdf(sdf_path)
        assert result.errors == [], f"Unexpected errors: {result.errors}"

    def test_missing_file(self, tmp_path):
        from bridge.validate import validate_sdf
        r = validate_sdf(tmp_path / "missing.sdf")
        assert not r.ok

    def test_wrong_root_element(self, tmp_path):
        from bridge.validate import validate_sdf
        bad = tmp_path / "bad.sdf"
        bad.write_text('<?xml version="1.0"?><robot/>')
        r = validate_sdf(bad)
        assert not r.ok

    def test_no_world_element_detected(self, tmp_path):
        from bridge.validate import validate_sdf
        sdf = tmp_path / "noworld.sdf"
        sdf.write_text('<?xml version="1.0"?><sdf version="1.9"><model name="x"/></sdf>')
        r = validate_sdf(sdf)
        assert not r.ok
        assert any("world" in e.lower() for e in r.errors)


# ══════════════════════════════════════════════════════════════════════════════
#  bridge.freecad_bridge — offline checks
# ══════════════════════════════════════════════════════════════════════════════

class TestFreeCADBridgeOffline:
    def test_export_sdf_world_stages_file(self, tmp_path, sdf_path, project):
        """export_sdf_world should copy the world SDF to the generated dir."""
        from bridge.freecad_bridge import export_sdf_world
        out_dir = tmp_path / "generated" / "empty_world"
        result  = export_sdf_world(
            world_name = "empty_world",
            out_dir    = out_dir,
            source_dir = project.paths.worlds,
        )
        assert result.ok, f"export_sdf_world failed: {result.messages}"
        assert result.path is not None
        assert result.path.exists()
        # Destination should contain valid SDF
        from bridge.validate import validate_sdf
        assert validate_sdf(result.path).ok

    def test_export_sdf_world_missing_source(self, tmp_path):
        from bridge.freecad_bridge import export_sdf_world
        r = export_sdf_world(
            world_name = "nonexistent_world",
            out_dir    = tmp_path / "generated" / "nonexistent",
            source_dir = tmp_path,  # empty dir
        )
        assert not r.ok
        assert any("not found" in m.lower() for m in r.messages)

    def test_check_robotcad_fails_cleanly_when_freecad_not_running(self):
        """check_robotcad should return ok=False with a clear message, not raise."""
        from bridge.freecad_bridge import check_robotcad
        # FreeCAD is likely not running in CI — expect a clean failure
        result = check_robotcad(host="localhost", port=9875, timeout=2.0)
        # Either ok (if FreeCAD is running) or a clean error with a message
        if not result.ok:
            assert result.messages, "Expected at least one error message"
            assert any(
                "freecad" in m.lower() or "rpc" in m.lower() or "connect" in m.lower()
                for m in result.messages
            )

    def test_export_urdf_fails_cleanly_when_freecad_not_running(self, tmp_path):
        from bridge.freecad_bridge import export_urdf
        result = export_urdf(
            robot_name = "arm_2dof",
            out_dir    = tmp_path / "generated" / "arm_2dof",
            timeout    = 2.0,
        )
        # Should fail cleanly, not raise
        assert not result.ok
        assert result.messages


# ══════════════════════════════════════════════════════════════════════════════
#  bridge.handoff — offline
# ══════════════════════════════════════════════════════════════════════════════

class TestHandoffOffline:
    def test_export_and_spawn_fails_cleanly_no_gazebo(self):
        """export_and_spawn with skip_freecad_export=True should fail at gazebo_ready."""
        from bridge.handoff import export_and_spawn
        result = export_and_spawn(
            robot_name          = "arm_2dof",
            world_name          = "empty_world",
            project_root        = REPO_ROOT,
            skip_freecad_export = True,
            gazebo_timeout      = 3.0,
        )
        # Expected: validate_urdf PASS, stage_world PASS, gazebo_ready FAIL
        step_names = [s.name for s in result.steps]
        assert "validate_urdf" in step_names
        assert "stage_world"   in step_names
        # Should not succeed overall (Gazebo not running)
        assert not result.ok

    def test_handoff_summary_is_readable(self):
        from bridge.handoff import export_and_spawn
        result = export_and_spawn(
            robot_name          = "arm_2dof",
            world_name          = "empty_world",
            project_root        = REPO_ROOT,
            skip_freecad_export = True,
            gazebo_timeout      = 2.0,
        )
        summary = result.summary()
        assert "HANDOFF" in summary
        assert len(summary) > 10

    def test_handoff_nonexistent_robot(self, tmp_path):
        """Handoff for a nonexistent robot should fail at resolve_urdf."""
        from bridge.handoff import export_and_spawn
        result = export_and_spawn(
            robot_name          = "nonexistent_robot_xyz",
            world_name          = "empty_world",
            project_root        = REPO_ROOT,
            skip_freecad_export = True,
            gazebo_timeout      = 2.0,
        )
        assert not result.ok
        step = next((s for s in result.steps if s.name == "resolve_urdf"), None)
        assert step is not None and not step.ok


# ══════════════════════════════════════════════════════════════════════════════
#  Live integration tests (skipped unless marks passed)
# ══════════════════════════════════════════════════════════════════════════════

@pytest.mark.freecad
class TestFreeCADBridgeLive:
    """Requires FreeCAD with MCP addon running on localhost:9875."""

    @pytest.fixture(autouse=True)
    def require_freecad(self):
        import xmlrpc.client
        try:
            proxy = xmlrpc.client.ServerProxy("http://localhost:9875", allow_none=True)
            proxy.ping()
        except Exception:
            pytest.skip("FreeCAD RPC not reachable on localhost:9875 — start FreeCAD with MCP addon")

    def test_ping_freecad(self):
        from bridge.freecad_bridge import _connect
        rpc = _connect("localhost", 9875, timeout=5.0)
        assert rpc is not None

    def test_check_robotcad_live(self):
        from bridge.freecad_bridge import check_robotcad
        result = check_robotcad(timeout=5.0)
        print(f"RobotCAD check: ok={result.ok}, messages={result.messages}")
        assert isinstance(result.ok, bool)


@pytest.mark.gazebo
class TestGazeboBridgeLive:
    """Requires Gazebo Docker container running (Start-gz-sim.bat)."""

    @pytest.fixture(autouse=True)
    def require_gazebo(self):
        """Skip unless RUN_GAZEBO_LIVE=1 and gazebo-mcp-server starts."""
        import os
        import subprocess
        import time

        if os.environ.get("RUN_GAZEBO_LIVE", "").strip().lower() not in ("1", "yes", "true"):
            pytest.skip("Live Gazebo MCP tests skipped (set RUN_GAZEBO_LIVE=1)")

        from bridge.gazebo_bridge import _GAZEBO_SERVER_CMD
        try:
            proc = subprocess.Popen(
                _GAZEBO_SERVER_CMD,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=False,
            )
            time.sleep(1.5)
            if proc.poll() is not None:
                proc.kill()
                pytest.skip("gazebo-mcp-server process failed to start")
            proc.terminate()
            proc.wait(timeout=5)
        except Exception as exc:
            pytest.skip(f"Gazebo MCP unavailable: {exc}")

    def test_gazebo_ready(self):
        from bridge.gazebo_bridge import wait_for_ready
        result = wait_for_ready(retries=5, delay=1.0, timeout=10.0)
        assert result.ok, (
            "Gazebo not ready. Run Start-gz-sim.bat and wait for container to start.\n"
            + "\n".join(result.messages)
        )

    def test_spawn_arm_2dof(self):
        from bridge.gazebo_bridge import spawn_model
        urdf = REPO_ROOT / "robots" / "arm_2dof.urdf"
        result = spawn_model(
            model_name = "arm_2dof_test",
            urdf_path  = urdf,
            pose       = {"position": {"x": 0, "y": 0, "z": 0}},
            timeout    = 30.0,
        )
        assert result.ok, "\n".join(result.messages)

    def test_get_model_state(self):
        from bridge.gazebo_bridge import get_model_state
        result = get_model_state("arm_2dof_test", timeout=10.0)
        assert result.ok, "\n".join(result.messages)

    def test_full_handoff(self):
        from bridge.handoff import export_and_spawn
        result = export_and_spawn(
            robot_name          = "arm_2dof",
            world_name          = "empty_world",
            project_root        = REPO_ROOT,
            skip_freecad_export = True,
            gazebo_timeout      = 30.0,
        )
        print(result.summary())
        assert result.ok, result.summary()
