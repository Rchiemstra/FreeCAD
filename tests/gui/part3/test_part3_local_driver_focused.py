# SPDX-License-Identifier: LGPL-2.1-or-later
"""Focused Windows-native LocalUserDriver GUI acceptance tests."""

from __future__ import annotations

import json
import subprocess
import sys
import uuid
from pathlib import Path
from typing import Any

import pytest

REPO_ROOT = Path(__file__).resolve().parents[3]

from tests.gui.part3.local_user_driver import TOKEN_ENV

pytestmark = [
    pytest.mark.skipif(sys.platform != "win32", reason="WP04 gate is Windows-native"),
]


def _create_box_document(rpc, name: str) -> dict[str, Any]:
    document_name = name
    try:
        created = rpc.call("create_document", {"name": document_name}, timeout=60.0)
    except Exception:
        created = rpc.call("create_document", document_name, timeout=60.0)
    if isinstance(created, dict):
        document_name = str(
            created.get("document_name") or created.get("name") or document_name
        )
    rpc.call(
        "create_object",
        {
            "doc_name": document_name,
            "obj_data": {
                "Name": "StressBox",
                "Type": "Part::Box",
                "Properties": {
                    "Length": 10.0,
                    "Width": 10.0,
                    "Height": 10.0,
                },
            },
        },
        timeout=60.0,
    )
    return {"document_name": document_name}


def _identity_selector(local, document_name: str) -> dict[str, Any]:
    state = local.invoke("view_state")["result"]
    assert state.get("active_document") == document_name
    selector = state.get("identity_selector")
    assert isinstance(selector, dict)
    assert selector.get("document_uid")
    return selector


def _semantic_model_revision(
    rpc, local, document_name: str, object_name: str = "StressBox"
) -> int:
    result = rpc.call(
        "get_semantic_revisions",
        {
            "doc_selector": _identity_selector(local, document_name),
            "revision_keys": [{"kind": "ObjectModel", "subject": object_name}],
        },
        timeout=30.0,
    )
    revisions = result.get("revisions") if isinstance(result, dict) else None
    assert isinstance(revisions, list) and revisions
    return int(revisions[0]["revision"])


def _file_change_state(local, document_name: str) -> dict[str, Any]:
    state = local.invoke("view_state")["result"]
    assert state.get("active_document") == document_name
    file_state = state.get("file_change_state")
    assert isinstance(file_state, dict)
    return file_state


def test_rotate_changes_camera_without_model_dirty(
    freecad_gui_session, tmp_path: Path
) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    document = _create_box_document(rpc, f"RotateDoc_{uuid.uuid4().hex[:8]}")
    document_name = document["document_name"]
    save_path = tmp_path / f"{document_name}.FCStd"
    rpc.call(
        "save_document_as",
        {
            "selector": {"document_name": document_name},
            "destination": str(save_path),
            "overwrite": True,
        },
        timeout=60.0,
    )
    local.invoke("set_active_document", {"document": document_name})
    before_revision = _semantic_model_revision(rpc, local, document_name)
    before_state = local.invoke("view_state")["result"]
    local.invoke("rotate_camera", {"yaw": 25.0, "pitch": 12.0})
    after_state = local.invoke("view_state")["result"]
    after_revision = _semantic_model_revision(rpc, local, document_name)
    assert before_state["camera_orientation"] != after_state["camera_orientation"]
    assert after_revision == before_revision
    assert after_state["touched"] is False
    file_state = _file_change_state(local, document_name)
    assert file_state.get("has_pending_file_changes") is False
    assert str(file_state.get("state", "")).lower() == "clean"


def test_pan_and_zoom_change_view_without_revision_drift(freecad_gui_session) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    document = _create_box_document(rpc, f"PanZoomDoc_{uuid.uuid4().hex[:8]}")
    document_name = document["document_name"]
    local.invoke("set_active_document", {"document": document_name})
    before_revision = _semantic_model_revision(rpc, local, document_name)
    before_state = local.invoke("view_state")["result"]
    local.invoke("pan_view", {"dx": 0.2, "dy": 0.0, "dz": 0.0})
    local.invoke("zoom_view", {"direction": "in"})
    local.invoke("fit_all", {"factor": 1.0})
    after_state = local.invoke("view_state")["result"]
    after_revision = _semantic_model_revision(rpc, local, document_name)
    assert after_state != before_state
    assert after_revision == before_revision


def test_selection_tree_and_unchanged_save(
    freecad_gui_session, tmp_path: Path
) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    document = _create_box_document(rpc, f"TreeDoc_{uuid.uuid4().hex[:8]}")
    document_name = document["document_name"]
    save_path = tmp_path / f"{document_name}.FCStd"
    rpc.call(
        "save_document_as",
        {
            "selector": {"document_name": document_name},
            "destination": str(save_path),
            "overwrite": True,
        },
        timeout=60.0,
    )
    local.invoke("set_active_document", {"document": document_name})
    before_revision = _semantic_model_revision(rpc, local, document_name)
    local.invoke("select_object", {"document": document_name, "object": "StressBox"})
    local.invoke("expand_tree", {"document": document_name, "object": "StressBox"})
    after_revision = _semantic_model_revision(rpc, local, document_name)
    assert after_revision == before_revision
    file_state = _file_change_state(local, document_name)
    assert file_state.get("has_pending_file_changes") is False
    save_result = rpc.call(
        "save_document",
        {"selector": {"document_name": document_name}},
        timeout=60.0,
    )
    assert isinstance(save_result, dict)
    assert str(save_result.get("save_disposition", "")).lower() == "unchanged"


def test_concurrent_remote_mutation_commits_once(freecad_gui_session) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    launcher_module = freecad_gui_session["launcher_module"]
    document = _create_box_document(rpc, f"ConcurrentDoc_{uuid.uuid4().hex[:8]}")
    document_name = document["document_name"]
    local.invoke("set_active_document", {"document": document_name})
    selector = _identity_selector(local, document_name)
    float_object = rpc.call(
        "create_object",
        {
            "doc_name": document_name,
            "obj_data": {
                "Name": "StressFloat",
                "Type": "App::FeatureTest",
                "Properties": {"Float": 2.0},
            },
        },
        timeout=60.0,
    )
    assert isinstance(float_object, dict)
    operation_id = str(uuid.uuid4())
    local.invoke("rotate_camera", {"named_view": "Top"})
    begin = rpc.call(
        "begin_checked_edit",
        {
            "doc_selector": selector,
            "revision_keys": [{"kind": "ObjectModel", "subject": "StressFloat"}],
            "operation_id": f"{operation_id}-begin",
        },
        timeout=60.0,
    )
    assert isinstance(begin, dict)
    assert begin.get("success") is True, begin
    session_id = begin["session_id"]
    commit_params = {
        "session_id": session_id,
        "doc_selector": selector,
        "object_name": "StressFloat",
        "property_name": "Float",
        "value_type": "float",
        "value": "42.0",
        "operation_id": operation_id,
    }
    result = rpc.call("commit_checked_property", commit_params, timeout=60.0)
    assert isinstance(result, dict)
    assert result.get("success", True) is not False
    duplicate = rpc.call("commit_checked_property", commit_params, timeout=60.0)
    assert duplicate == result
    with pytest.raises(launcher_module.JsonRpcError):
        rpc.call(
            "commit_checked_property",
            {
                **commit_params,
                "value": "13.0",
            },
            timeout=60.0,
        )


def test_checkbox_pause_and_resume_gate_remote_writes(freecad_gui_session) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    launcher_module = freecad_gui_session["launcher_module"]
    document = _create_box_document(rpc, f"PauseDoc_{uuid.uuid4().hex[:8]}")
    document_name = document["document_name"]
    local.invoke("pause_writes")
    with pytest.raises(launcher_module.JsonRpcError) as paused_exc:
        rpc.call(
            "edit_object",
            {
                "doc_name": document_name,
                "obj_name": "StressBox",
                "properties": {"Width": 11.0},
            },
            timeout=30.0,
        )
    paused_text = f"{paused_exc.value} {paused_exc.value.data}"
    assert (
        "AUTOMATION_PAUSED" in paused_text
        or "paused new MCP writes" in paused_text
    )
    local.invoke("resume_writes")
    admitted = rpc.call(
        "edit_object",
        {
            "doc_name": document_name,
            "obj_name": "StressBox",
            "properties": {"Width": 12.0},
        },
        timeout=30.0,
    )
    assert isinstance(admitted, dict)


def test_remote_child_lacks_control_token() -> None:
    env = dict(**{key: str(value) for key, value in __import__("os").environ.items()})
    env.pop(TOKEN_ENV, None)
    completed = subprocess.run(
        [
            sys.executable,
            str(Path(__file__).resolve().parent / "remote_agent_driver.py"),
            "--inspect-token-absence",
        ],
        env=env,
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr
    report = json.loads(completed.stdout)
    assert report["absent"] is True
    assert TOKEN_ENV not in env


def _property_revision(rpc, local, document_name: str, object_name: str, property_name: str) -> int:
    result = rpc.call(
        "get_semantic_revisions",
        {
            "doc_selector": _identity_selector(local, document_name),
            "revision_keys": [
                {
                    "kind": "ObjectProperty",
                    "subject": object_name,
                    "property_name": property_name,
                },
            ],
        },
        timeout=30.0,
    )
    revisions = result.get("revisions") if isinstance(result, dict) else None
    assert isinstance(revisions, list) and revisions
    return int(revisions[0]["revision"])


def test_local_property_edit_publishes_expected_revisions(
    freecad_gui_session, tmp_path: Path
) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    document_name = f"LocalEditDoc_{uuid.uuid4().hex[:8]}"
    try:
        created = rpc.call("create_document", {"name": document_name}, timeout=60.0)
    except Exception:
        created = rpc.call("create_document", document_name, timeout=60.0)
    if isinstance(created, dict):
        document_name = str(
            created.get("document_name") or created.get("name") or document_name
        )
    rpc.call(
        "create_object",
        {
            "doc_name": document_name,
            "obj_data": {
                "Name": "StressFloat",
                "Type": "App::FeatureTest",
                "Properties": {"Float": 2.0},
            },
        },
        timeout=60.0,
    )
    save_path = tmp_path / f"{document_name}.FCStd"
    rpc.call(
        "save_document_as",
        {
            "selector": {"document_name": document_name},
            "destination": str(save_path),
            "overwrite": True,
        },
        timeout=60.0,
    )
    local.invoke("set_active_document", {"document": document_name})
    before_model = _semantic_model_revision(rpc, local, document_name, "StressFloat")
    before_property = _property_revision(
        rpc, local, document_name, "StressFloat", "Float"
    )
    local.invoke(
        "local_property_edit",
        {
            "document": document_name,
            "object": "StressFloat",
            "property": "Float",
            "value": 42.0,
        },
    )
    after_model = _semantic_model_revision(rpc, local, document_name, "StressFloat")
    after_property = _property_revision(
        rpc, local, document_name, "StressFloat", "Float"
    )
    assert after_model == before_model + 1
    assert after_property == before_property + 1
    file_state = _file_change_state(local, document_name)
    assert file_state.get("has_pending_file_changes") is True
    state = local.invoke("view_state")["result"]
    assert state.get("modified") is True or state.get("touched") is True


def test_local_save_orders_with_remote_mutation(
    freecad_gui_session, tmp_path: Path
) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    document = _create_box_document(rpc, f"LocalSaveDoc_{uuid.uuid4().hex[:8]}")
    document_name = document["document_name"]
    save_path = tmp_path / f"{document_name}.FCStd"
    rpc.call(
        "save_document_as",
        {
            "selector": {"document_name": document_name},
            "destination": str(save_path),
            "overwrite": True,
        },
        timeout=60.0,
    )
    local.invoke("set_active_document", {"document": document_name})
    rpc.call(
        "edit_object",
        {
            "doc_name": document_name,
            "obj_name": "StressBox",
            "properties": {"Width": 11.0},
        },
        timeout=60.0,
    )
    local.invoke("local_save")
    file_state = _file_change_state(local, document_name)
    assert file_state.get("has_pending_file_changes") is False
    assert str(file_state.get("state", "")).lower() == "clean"
    save_result = rpc.call(
        "save_document",
        {"selector": {"document_name": document_name}},
        timeout=60.0,
    )
    assert isinstance(save_result, dict)
    assert str(save_result.get("save_disposition", "")).lower() == "unchanged"


def test_remote_rpc_cannot_resume_local_pause() -> None:
    facade = (
        REPO_ROOT
        / "tools"
        / "mcp"
        / "freecad-mcp"
        / "addon"
        / "FreeCADMCP"
        / "rpc_server"
        / "rpc_server_ops"
        / "facade_bindings.py"
    )
    source = facade.read_text(encoding="utf-8")
    assert "resume_local_agent_writes" not in source
    assert "request_local_pause_after_current" not in source
