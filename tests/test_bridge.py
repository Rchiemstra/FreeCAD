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
#  bridge.gazebo_bridge — MCP tool wiring (offline)
# ══════════════════════════════════════════════════════════════════════════════

class TestUrdfForGazebo:
    def test_prepare_urdf_rewrites_package_uri(self, tmp_path):
        from bridge.urdf_for_gazebo import prepare_urdf_for_gazebo, robotcad_description_root

        pkg = tmp_path / "arm_2dof_description"
        (pkg / "meshes").mkdir(parents=True)
        (pkg / "package.xml").write_text("<package/>", encoding="utf-8")
        urdf = pkg / "urdf" / "arm.urdf"
        urdf.parent.mkdir()
        urdf.write_text(
            '<robot name="r">'
            '<mesh filename="package://arm_2dof_description/meshes/part.dae"/>'
            "</robot>",
            encoding="utf-8",
        )
        assert robotcad_description_root(urdf) == pkg.resolve()
        out = prepare_urdf_for_gazebo(urdf.read_text(encoding="utf-8"), urdf)
        assert "package://arm_2dof_description/" not in out
        assert "file:///models/arm_2dof_description/meshes/part.dae" in out

    def test_prepare_urdf_replaces_end_effector_collision_mesh(self, tmp_path, monkeypatch):
        from bridge.urdf_for_gazebo import (
            collision_mesh_policy,
            prepare_urdf_for_gazebo,
            simplify_collision_meshes_for_gazebo,
        )

        monkeypatch.delenv("GAZEBO_COLLISION_MESH_POLICY", raising=False)
        assert collision_mesh_policy() == "replace_end_effector_mesh"

        urdf_xml = """<robot name="arm">
  <link name="end_effector">
    <visual>
      <geometry>
        <mesh filename="package://arm_2dof_description/meshes/col_end_effector_.dae"/>
      </geometry>
    </visual>
    <collision>
      <geometry>
        <mesh filename="package://arm_2dof_description/meshes/col_end_effector_.dae"/>
      </geometry>
    </collision>
  </link>
</robot>"""
        out, n = simplify_collision_meshes_for_gazebo(urdf_xml)
        assert n == 1
        assert "col_end_effector_.dae" in out
        assert out.count("<collision>") == 1
        assert "<sphere radius=" in out
        assert "<collision>" in out and "<mesh" not in out.split("<collision>")[1]

        pkg = tmp_path / "arm_2dof_description"
        pkg.mkdir(parents=True)
        (pkg / "package.xml").write_text("<package/>", encoding="utf-8")
        urdf = pkg / "urdf" / "arm_2dof.urdf"
        urdf.parent.mkdir(parents=True)
        urdf.write_text(urdf_xml, encoding="utf-8")
        prepared = prepare_urdf_for_gazebo(urdf_xml, urdf)
        assert "file:///models/arm_2dof_description/meshes/col_end_effector_.dae" in prepared
        assert "<sphere radius=" in prepared
        assert prepared.count("<mesh") == 1

    def test_verify_fcstd_matches_runtime_lock(self):
        from bridge.runtime_versions import verify_fcstd

        ok, msg = verify_fcstd(REPO_ROOT)
        assert ok, msg

    def test_compare_observed_to_lock_apt_mismatch_is_error(self):
        from bridge.runtime_versions import compare_observed_to_lock, load_runtime_lock

        lock = load_runtime_lock(REPO_ROOT)
        observed = {
            "apt_versions": {"freecad-daily": "0.0.0-1"},
            "robot_source": {"ok": True, "sha256": lock["robot_source"]["fcstd_sha256"]},
            "robotcad_commit": lock["docker_e2e"]["robotcad"]["commit"],
            "docker_base_image_ref": (
                f"{lock['docker_e2e']['base_image']}@{lock['docker_e2e']['base_image_digest']}"
            ),
            "mcp_venv": {"mcp": lock["pypi"]["mcp"], "pydantic": lock["pypi"]["pydantic"], "pyyaml": lock["pypi"]["pyyaml"]},
        }
        warnings, errors = compare_observed_to_lock(observed, lock)
        assert any("freecad-daily" in e for e in errors)
        assert not warnings or isinstance(warnings, list)

    def test_collision_policy_keep_skips_replacement(self, monkeypatch):
        from bridge.urdf_for_gazebo import simplify_collision_meshes_for_gazebo

        monkeypatch.setenv("GAZEBO_COLLISION_MESH_POLICY", "keep")
        urdf_xml = """<robot><link><collision><geometry>
        <mesh filename="meshes/col_end_effector_.dae"/>
        </geometry></collision></link></robot>"""
        out, n = simplify_collision_meshes_for_gazebo(urdf_xml)
        assert n == 0
        assert "<mesh" in out
        assert "<sphere" not in out


class TestGazeboGzDocker:
    def test_container_urdf_path_maps_robotcad_export(self, tmp_path):
        from bridge.gazebo_gz_docker import container_urdf_path

        pkg = tmp_path / "arm_2dof_description"
        pkg.mkdir(parents=True)
        (pkg / "package.xml").write_text("<package/>", encoding="utf-8")
        host = pkg / "urdf" / "arm_2dof.urdf"
        host.parent.mkdir(parents=True)
        host.write_text("<robot/>", encoding="utf-8")
        assert container_urdf_path(host) == "/models/arm_2dof_description/urdf/arm_2dof.urdf"

    def test_container_urdf_path_none_for_placeholder(self, urdf_path):
        from bridge.gazebo_gz_docker import container_urdf_path

        assert container_urdf_path(urdf_path) is None


class TestGazeboBridgeHelpers:
    def test_model_names_from_list_data(self):
        from bridge.gazebo_bridge import _model_names_from_list_data

        names = _model_names_from_list_data({
            "models": [{"name": "a"}, {"name": "b"}, {}],
        })
        assert names == ["a", "b"]

    def test_spawn_model_calls_gazebo_spawn_sdf(self, monkeypatch, urdf_path):
        captured = []

        class FakeSession:
            def __init__(self, timeout=15.0):
                pass

            def __enter__(self):
                def _call(tool, args):
                    captured.append((tool, args))
                    return {"content": [{"type": "text", "text": '{"success": true}'}]}
                return _call

            def __exit__(self, *_):
                pass

        monkeypatch.setattr("bridge.gazebo_bridge.GazeboSession", FakeSession)
        monkeypatch.setenv("GAZEBO_SPAWN_VIA_GZ_CLI", "0")
        monkeypatch.setenv("GAZEBO_MCP_DOCKER", "0")

        from bridge.gazebo_bridge import spawn_model

        result = spawn_model(
            model_name="arm_test",
            urdf_path=urdf_path,
            pose={"position": {"x": 1, "y": 0, "z": 0.5}},
        )
        assert result.ok
        spawn_calls = [c for c in captured if c[0] == "gazebo_spawn_sdf"]
        assert spawn_calls, f"expected gazebo_spawn_sdf in {captured}"
        tool, args = spawn_calls[0]
        assert args["entity_name"] == "arm_test"
        assert "<robot" in args["sdf_xml"]
        assert args["x"] == pytest.approx(1.0)
        assert args["z"] == pytest.approx(0.5)


class TestMcpStatus:
    def test_service_status_label(self):
        from bridge.mcp_status import ServiceStatus

        on = ServiceStatus("freecad-mcp", True, "12 tools")
        off = ServiceStatus("gazebo-mcp", False, "venv missing")
        assert "online" in on.label()
        assert "offline" in off.label()


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
#  bridge.freecad_bridge — execute_code response parsing (offline)
# ══════════════════════════════════════════════════════════════════════════════

class TestRobotcadSnippets:
    def test_check_snippet_imports_freecad_cross(self):
        from bridge.freecad_bridge import _ROBOTCAD_CHECK_SNIPPET

        assert "import freecad.cross" in _ROBOTCAD_CHECK_SNIPPET
        assert "import CROSS" not in _ROBOTCAD_CHECK_SNIPPET
        assert "_ensure_freecad_utils_path" in _ROBOTCAD_CHECK_SNIPPET
        assert "_purge_cached_freecad" in _ROBOTCAD_CHECK_SNIPPET

    def test_export_snippet_uses_proxy_export_urdf(self):
        from bridge.freecad_bridge import _EXPORT_URDF_SNIPPET_TEMPLATE

        assert "Cross::Robot" in _EXPORT_URDF_SNIPPET_TEMPLATE
        assert "export_urdf(interactive=True)" in _EXPORT_URDF_SNIPPET_TEMPLATE
        assert "get_urdf_path" in _EXPORT_URDF_SNIPPET_TEMPLATE

    def test_expected_exported_urdf_path(self):
        from bridge.freecad_bridge import expected_exported_urdf_path

        p = expected_exported_urdf_path("arm_2dof", Path("generated/arm_2dof"))
        assert p.as_posix().endswith(
            "generated/arm_2dof/arm_2dof_description/arm_2dof_description/urdf/arm_2dof.urdf"
        )


class TestExecuteCodeParsing:
    """FreeCAD RPC success vs snippet inner success are different layers."""

    def test_interpret_inner_failure_when_rpc_succeeds(self):
        from bridge.freecad_bridge import _interpret_execute_code

        raw = {
            "success": True,
            "message": (
                "Python code execution scheduled. \nOutput: "
                "{'success': False, 'message': \"RobotCAD/CROSS not installed: "
                "No module named 'CROSS'\"}\n"
            ),
        }
        inner = _interpret_execute_code(raw)
        assert inner["success"] is False
        assert "CROSS" in inner["message"]

    def test_interpret_inner_success_with_path(self):
        from bridge.freecad_bridge import _interpret_execute_code

        raw = {
            "success": True,
            "message": (
                "Python code execution scheduled. \nOutput: "
                "{'success': True, 'message': 'Exported', 'path': 'C:/out/arm.urdf'}\n"
            ),
        }
        inner = _interpret_execute_code(raw)
        assert inner["success"] is True
        assert inner["path"] == "C:/out/arm.urdf"

    def test_interpret_rpc_execution_error(self):
        from bridge.freecad_bridge import _interpret_execute_code

        inner = _interpret_execute_code({"success": False, "error": "SyntaxError"})
        assert inner["success"] is False
        assert "SyntaxError" in inner["message"]

    def test_export_urdf_uses_inner_success(self, monkeypatch, tmp_path):
        from bridge.freecad_bridge import export_urdf

        class FakeRPC:
            def ping(self):
                return True

            def execute_code(self, code):
                return {
                    "success": True,
                    "message": (
                        "Python code execution scheduled. \nOutput: "
                        "{'success': False, 'message': 'RobotCAD/CROSS not installed'}\n"
                    ),
                }

        monkeypatch.setattr(
            "bridge.freecad_bridge._connect",
            lambda host, port, timeout: FakeRPC(),
        )
        result = export_urdf(
            robot_name="arm_2dof",
            out_dir=tmp_path / "generated" / "arm_2dof",
            prefer_cmd=False,
        )
        assert not result.ok
        assert any("robotcad" in m.lower() or "cross" in m.lower() for m in result.messages)


# ══════════════════════════════════════════════════════════════════════════════
#  bridge.freecad_bridge — offline checks
# ══════════════════════════════════════════════════════════════════════════════

class TestExportUrdfCmd:
    def test_export_urdf_cmd_parses_stdout(self, monkeypatch, tmp_path):
        from bridge.freecad_bridge import export_urdf_cmd

        fcstd = tmp_path / "robots" / "arm_2dof.FCStd"
        fcstd.parent.mkdir(parents=True)
        fcstd.write_bytes(b"fake")
        out = tmp_path / "generated" / "arm_2dof"
        urdf = (
            out
            / "arm_2dof_description"
            / "arm_2dof_description"
            / "urdf"
            / "arm_2dof.urdf"
        )
        urdf.parent.mkdir(parents=True)
        urdf.write_text("<robot name='arm_2dof'/>\n", encoding="utf-8")

        class FakeProc:
            returncode = 0
            stdout = f"URDF_EXPORT_PATH: {urdf}\n"
            stderr = ""

        monkeypatch.setattr(
            "bridge.freecad_bridge.resolve_freecad_cmd",
            lambda: tmp_path / "FreeCADCmd.exe",
        )
        monkeypatch.setattr(
            "bridge.freecad_bridge.subprocess.run",
            lambda *a, **k: FakeProc(),
        )

        result = export_urdf_cmd("arm_2dof", out, fcstd_path=fcstd)
        assert result.ok
        assert result.path == urdf


@pytest.mark.needs_freecad
class TestExportUrdfCmdLive:
    def test_export_arm_2dof_via_freecadcmd(self):
        from bridge.freecad_bridge import export_urdf_cmd, resolve_freecad_cmd
        from bridge.validate import validate_urdf

        if resolve_freecad_cmd() is None:
            pytest.skip("FreeCADCmd not available")

        repo = Path(__file__).resolve().parents[1]
        fcstd = repo / "robots" / "arm_2dof.FCStd"
        if not fcstd.is_file():
            pytest.skip("robots/arm_2dof.FCStd not built")

        out = repo / "generated" / "arm_2dof"
        result = export_urdf_cmd("arm_2dof", out, fcstd_path=fcstd, timeout=600)
        assert result.ok, result.messages
        assert result.path and result.path.is_file()
        validation = validate_urdf(result.path)
        assert validation.ok, validation.summary()


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

    def test_check_robotcad_when_freecad_unreachable(self):
        """check_robotcad should return ok=False with a clear message when RPC is down."""
        from bridge.freecad_bridge import check_robotcad
        result = check_robotcad(host="127.0.0.1", port=19875, timeout=1.0)
        assert not result.ok
        assert result.messages
        assert any(
            "rpc" in m.lower() or "reach" in m.lower() or "connect" in m.lower()
            for m in result.messages
        )

    def test_export_urdf_fails_cleanly_without_robotcad_or_rpc(self, tmp_path):
        """Fails when RPC is down and batch export has no FCStd (no accidental repo export)."""
        from bridge.freecad_bridge import export_urdf
        result = export_urdf(
            robot_name = "arm_2dof",
            out_dir    = tmp_path / "generated" / "arm_2dof",
            fcstd_path = tmp_path / "nonexistent_robot.FCStd",
            timeout    = 2.0,
            prefer_cmd = False,
        )
        assert not result.ok
        assert result.messages


# ══════════════════════════════════════════════════════════════════════════════
#  bridge.handoff — offline
# ══════════════════════════════════════════════════════════════════════════════

class TestResolveRobotUrdf:
    def test_prefers_robotcad_nested_export(self, tmp_path):
        from bridge.handoff import resolve_robot_urdf
        from bridge.freecad_bridge import expected_exported_urdf_path

        robots = tmp_path / "robots"
        generated = tmp_path / "generated" / "arm_2dof"
        robots.mkdir(parents=True)
        generated.mkdir(parents=True)
        (robots / "arm_2dof.urdf").write_text("<robot name='placeholder'/>", encoding="utf-8")

        robotcad = expected_exported_urdf_path("arm_2dof", generated)
        robotcad.parent.mkdir(parents=True, exist_ok=True)
        robotcad.write_text("<robot name='exported'/>", encoding="utf-8")

        path, msgs, needs_export = resolve_robot_urdf(
            "arm_2dof",
            robots_dir=robots,
            generated_dir=generated,
            skip_freecad_export=True,
        )
        assert path == robotcad
        assert not needs_export
        assert any("RobotCAD-exported" in m for m in msgs)

    def test_skip_export_falls_back_to_placeholder(self, tmp_path):
        from bridge.handoff import resolve_robot_urdf

        robots = tmp_path / "robots"
        generated = tmp_path / "generated" / "arm_2dof"
        robots.mkdir(parents=True)
        generated.mkdir(parents=True)
        placeholder = robots / "arm_2dof.urdf"
        placeholder.write_text("<robot name='placeholder'/>", encoding="utf-8")

        path, msgs, needs_export = resolve_robot_urdf(
            "arm_2dof",
            robots_dir=robots,
            generated_dir=generated,
            skip_freecad_export=True,
        )
        assert path == placeholder
        assert not needs_export
        assert any("hand-crafted" in m for m in msgs)


class TestHandoffOffline:
    def test_export_and_spawn_fails_cleanly_no_gazebo(self):
        """export_and_spawn with skip_freecad_export=True should fail at gazebo_ready."""
        from bridge.handoff import export_and_spawn, GAZEBO_NOT_RUNNING_PREFIX
        from bridge.freecad_bridge import expected_exported_urdf_path

        generated_urdf = expected_exported_urdf_path(
            "arm_2dof", REPO_ROOT / "generated" / "arm_2dof"
        )

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
        resolve = next(s for s in result.steps if s.name == "resolve_urdf")
        assert resolve.ok
        if generated_urdf.is_file():
            assert any("RobotCAD-exported" in m for m in resolve.messages)
        gazebo = next(s for s in result.steps if s.name == "gazebo_ready")
        assert not gazebo.ok
        assert any(GAZEBO_NOT_RUNNING_PREFIX in m for m in gazebo.messages)
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
        result = check_robotcad(timeout=15.0)
        print(f"RobotCAD check: ok={result.ok}, messages={result.messages}")
        assert isinstance(result.ok, bool)
        if not result.ok:
            pytest.skip(
                "RobotCAD/CROSS not importable in FreeCAD — "
                "run scripts/install_robotcad_cross.ps1 and restart FreeCAD"
            )


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

        os.environ.setdefault("GAZEBO_WORLD_NAME", "empty_world")
        os.environ.setdefault("GZ_SIM_WORLD_NAME", "empty_world")
        os.environ.setdefault("GAZEBO_MCP_DOCKER", "1")
        os.environ.setdefault("GAZEBO_SPAWN_VIA_GZ_CLI", "1")

        bridge_sh = REPO_ROOT / "scripts" / "ensure_ros_gz_bridge.sh"
        if bridge_sh.is_file():
            wsl_sh = subprocess.run(
                ["wsl", "wslpath", "-a", str(bridge_sh)],
                capture_output=True, text=True, timeout=15,
            )
            if wsl_sh.returncode == 0:
                path = wsl_sh.stdout.strip()
                subprocess.run(
                    ["wsl", "bash", "-lc", f"sed -i 's/\\r$//' '{path}' && bash '{path}'"],
                    check=False,
                    timeout=180,
                )

        from bridge.gazebo_bridge import get_gazebo_server_cmd
        cmd = get_gazebo_server_cmd()
        try:
            proc = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                text=False,
            )
            time.sleep(60.0 if os.environ.get("GAZEBO_MCP_DOCKER", "1") != "0" else 1.5)
            if proc.poll() is not None:
                err = proc.stderr.read(4000).decode("utf-8", errors="replace")
                proc.kill()
                pytest.skip(f"gazebo-mcp-server failed to start: {err[:500]}")
            proc.terminate()
            proc.wait(timeout=10)
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
        from bridge.freecad_bridge import expected_exported_urdf_path

        gen_dir = REPO_ROOT / "generated" / "arm_2dof"
        urdf = expected_exported_urdf_path("arm_2dof", gen_dir)
        if not urdf.is_file():
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
        from bridge.gazebo_bridge import wait_for_ready, _gazebo_connected

        ready = wait_for_ready(retries=6, delay=2.0, timeout=30.0)
        assert ready.ok, "\n".join(ready.messages)
        assert _gazebo_connected(ready.data) is True, (
            "Expected real Gazebo (gazebo_connected=True). "
            "Run Start-gz-sim.bat and scripts/ensure_ros_gz_bridge.sh."
        )

        result = export_and_spawn(
            robot_name          = "arm_2dof",
            world_name          = "empty_world",
            project_root        = REPO_ROOT,
            skip_freecad_export = True,
            gazebo_timeout      = 120.0,
            skip_spawn          = False,
        )
        resolve = next((s for s in result.steps if s.name == "resolve_urdf"), None)
        gazebo = next((s for s in result.steps if s.name == "gazebo_ready"), None)
        spawn = next((s for s in result.steps if s.name == "spawn_model"), None)
        assert resolve and resolve.ok
        assert gazebo and gazebo.ok, "\n".join(gazebo.messages)
        assert spawn and spawn.ok, "\n".join(spawn.messages)
        assert result.ok, result.summary()
