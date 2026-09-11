# SPDX-License-Identifier: LGPL-2.1-or-later
"""Deterministic Windows GUI integration suite for Part 3 WP07 (ADR §6–§7, §2.2, §13).

This module is the WP07 gate. Focused GUI tests and FreeCADCmd e2e are substrate only.
"""

from __future__ import annotations

import contextlib
import threading
import uuid
from pathlib import Path
from typing import Any

import pytest

from tests.gui.part3.conftest import launch_freecad_gui_session

pytestmark = [
    pytest.mark.skipif(
        __import__("sys").platform != "win32",
        reason="Part 3 integration gate is Windows-native",
    ),
]


@pytest.fixture(scope="function")
def freecad_gui_session():
    with launch_freecad_gui_session() as session:
        yield session


def _identity_selector(local, document_name: str) -> dict[str, Any]:
    state = local.invoke("view_state")["result"]
    assert state.get("active_document") == document_name
    selector = state.get("identity_selector")
    assert isinstance(selector, dict)
    assert selector.get("document_uid")
    return selector


def _property_key(object_name: str, property_name: str) -> dict[str, str]:
    return {
        "kind": "ObjectProperty",
        "subject": object_name,
        "property_name": property_name,
    }


def _property_revision(
    rpc,
    local,
    document_name: str,
    object_name: str,
    property_name: str,
) -> int:
    result = rpc.call(
        "get_semantic_revisions",
        {
            "doc_selector": _identity_selector(local, document_name),
            "revision_keys": [_property_key(object_name, property_name)],
        },
        timeout=30.0,
    )
    revisions = result.get("revisions") if isinstance(result, dict) else None
    assert isinstance(revisions, list) and revisions
    return int(revisions[0]["revision"])


def _object_property_value(
    rpc,
    document_name: str,
    object_name: str,
    property_name: str,
) -> int:
    payload = rpc.call(
        "get_object",
        {"doc_name": document_name, "obj_name": object_name},
        timeout=30.0,
    )
    assert isinstance(payload, dict)
    properties = payload.get("properties") or payload.get("Properties") or {}
    assert isinstance(properties, dict)
    value = properties.get(property_name)
    if isinstance(value, dict) and "value" in value:
        return int(value["value"])
    return int(value)


def _teardown_pe_document(local, document_name: str) -> None:
    with contextlib.suppress(Exception):
        local.invoke("clear_selection")
    with contextlib.suppress(Exception):
        local.invoke("close_document", {"document": document_name})


def _provision_alpha_beta(
    local,
    document_name: str,
    rpc=None,
    save_path: Path | None = None,
    *,
    alpha: int = 0,
    beta: int = 0,
    with_async_blocker: bool = False,
) -> dict[str, Any]:
    response = local.invoke(
        "provision_alpha_beta_fixture",
        {
            "document": document_name,
            "alpha": alpha,
            "beta": beta,
            "with_async_blocker": with_async_blocker,
        },
    )
    local.invoke("set_active_document", {"document": document_name})
    if rpc is not None and save_path is not None:
        rpc.call(
            "save_document_as",
            {
                "selector": {"document_name": document_name},
                "destination": str(save_path),
                "overwrite": True,
            },
            timeout=60.0,
        )
    return response["result"]


def _mutation_readiness(rpc, document_name: str) -> dict[str, Any]:
    result = rpc.call(
        "get_mutation_readiness",
        {"doc_name": document_name},
        timeout=30.0,
    )
    assert isinstance(result, dict)
    return result


def _expect_checked_edit_conflict(
    rpc,
    launcher_module,
    commit_params: dict[str, Any],
) -> dict[str, Any]:
    try:
        result = rpc.call("commit_checked_property", commit_params, timeout=60.0)
    except launcher_module.JsonRpcError as exc:
        data = exc.data if isinstance(exc.data, dict) else {}
        return {
            "success": False,
            "error_code": data.get("error_code"),
            "error": str(exc),
            "expected_revisions": data.get("expected_revisions"),
            "current_revisions": data.get("current_revisions"),
            "changed_semantic_keys": data.get("changed_semantic_keys"),
        }
    return result if isinstance(result, dict) else {"success": False, "error": str(result)}


def _teardown_pe_document(local, document_name: str) -> None:
    with contextlib.suppress(Exception):
        local.invoke("reset_property_editor")
    with contextlib.suppress(Exception):
        local.invoke("clear_selection")


def _prepare_local_property_edit(local, document_name: str, object_name: str) -> None:
    local.invoke("reset_property_editor")
    local.invoke("clear_selection")
    local.invoke("set_active_document", {"document": document_name})
    local.invoke("select_object", {"document": document_name, "object": object_name})
    local.invoke("expand_tree", {"document": document_name, "object": object_name})


def _warm_property_editor(rpc, local, document_name: str) -> None:
    selector = _identity_selector(local, document_name)
    rpc.call(
        "get_semantic_revisions",
        {
            "doc_selector": selector,
            "revision_keys": [_property_key("StressBox", "AlphaValue")],
        },
        timeout=30.0,
    )


def _remote_edit_properties(
    rpc,
    document_name: str,
    object_name: str,
    properties: dict[str, Any],
) -> dict[str, Any]:
    result = rpc.call(
        "edit_object",
        {
            "doc_name": document_name,
            "obj_name": object_name,
            "properties": {"Properties": properties},
        },
        timeout=60.0,
    )
    assert isinstance(result, dict)
    return result


def _readiness_flag(readiness: dict[str, Any], field: str) -> Any:
    value = readiness.get(field)
    if value is not None:
        return value
    documents = readiness.get("documents")
    if isinstance(documents, list) and documents:
        first = documents[0]
        if isinstance(first, dict):
            return first.get(field)
    return None


def _automation_paused(readiness: dict[str, Any]) -> bool:
    paused = _readiness_flag(readiness, "automation_paused")
    if paused is not None:
        return bool(paused)
    pause = readiness.get("automation_pause")
    if isinstance(pause, dict) and pause.get("paused") is not None:
        return bool(pause["paused"])
    reasons = readiness.get("reasons")
    return isinstance(reasons, list) and "automation_paused" in reasons


def _active_write_count(readiness: dict[str, Any]) -> int:
    count = readiness.get("active_write_count")
    if count is not None:
        return int(count)
    documents = readiness.get("documents")
    if isinstance(documents, list) and documents:
        first = documents[0]
        if isinstance(first, dict) and first.get("active_write_count") is not None:
            return int(first["active_write_count"])
    pause = readiness.get("automation_pause")
    if isinstance(pause, dict) and pause.get("active_write_count") is not None:
        return int(pause["active_write_count"])
    return 0


def _is_recomputing(readiness: dict[str, Any]) -> bool:
    if readiness.get("recomputing"):
        return bool(readiness["recomputing"])
    documents = readiness.get("documents")
    if isinstance(documents, list) and documents:
        first = documents[0]
        if isinstance(first, dict) and first.get("recomputing") is not None:
            return bool(first["recomputing"])
    return False


def test_same_property_conflict_via_local_pe(
    freecad_gui_session, tmp_path: Path
) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    launcher_module = freecad_gui_session["launcher_module"]
    document_name = f"ConflictDoc_{uuid.uuid4().hex[:8]}"
    save_path = tmp_path / f"{document_name}.FCStd"
    _provision_alpha_beta(
        local,
        document_name,
        rpc,
        save_path,
        alpha=0,
        beta=0,
    )

    local.invoke("set_active_document", {"document": document_name})
    selector = _identity_selector(local, document_name)
    revision_keys = [{"kind": "ObjectModel", "subject": "StressBox"}]

    begin = rpc.call(
        "begin_checked_edit",
        {
            "doc_selector": selector,
            "revision_keys": revision_keys,
            "operation_id": f"{uuid.uuid4()}-begin",
        },
        timeout=60.0,
    )
    assert begin.get("success") is True, begin
    session_id = begin["session_id"]
    _warm_property_editor(rpc, local, document_name)
    _prepare_local_property_edit(local, document_name, "StressBox")

    local.invoke(
        "local_property_edit",
        {
            "document": document_name,
            "object": "StressBox",
            "property": "AlphaValue",
            "value": 42,
        },
    )
    assert _object_property_value(rpc, document_name, "StressBox", "AlphaValue") == 42

    conflict = _expect_checked_edit_conflict(
        rpc,
        launcher_module,
        {
            "session_id": session_id,
            "doc_selector": selector,
            "object_name": "StressBox",
            "property_name": "AlphaValue",
            "value_type": "integer",
            "value": "10",
            "operation_id": str(uuid.uuid4()),
        },
    )
    assert conflict.get("success") is False, conflict
    assert conflict.get("error_code") == "DOCUMENT_CONFLICT" or (
        "semantic revisions changed" in str(conflict.get("error") or "")
    )
    assert isinstance(conflict.get("expected_revisions"), dict)
    assert isinstance(conflict.get("current_revisions"), dict)
    semantic_key = "ObjectProperty:StressBox:AlphaValue"
    model_key = "ObjectModel:StressBox"
    changed = conflict.get("changed_semantic_keys", [])
    assert semantic_key in changed or model_key in changed

    readiness = _mutation_readiness(rpc, document_name)
    assert _readiness_flag(readiness, "quarantined") is False
    assert _readiness_flag(readiness, "collaboration_poisoned") is not True

    rpc.call(
        "cancel_checked_edit",
        {
            "session_id": session_id,
            "reason": "integration cleanup",
            "operation_id": str(uuid.uuid4()),
        },
        timeout=30.0,
    )
    _teardown_pe_document(local, document_name)


def test_independent_property_alpha_beta_both_land_once(
    freecad_gui_session, tmp_path: Path
) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    document_name = f"IndependentDoc_{uuid.uuid4().hex[:8]}"
    save_path = tmp_path / f"{document_name}.FCStd"
    _provision_alpha_beta(
        local,
        document_name,
        rpc,
        save_path,
        alpha=0,
        beta=0,
    )

    local.invoke("set_active_document", {"document": document_name})
    selector = _identity_selector(local, document_name)
    alpha_before = _property_revision(
        rpc, local, document_name, "StressBox", "AlphaValue"
    )
    beta_before = _property_revision(
        rpc, local, document_name, "SecondBox", "BetaValue"
    )

    _prepare_local_property_edit(local, document_name, "StressBox")
    local.invoke(
        "local_property_edit",
        {
            "document": document_name,
            "object": "StressBox",
            "property": "AlphaValue",
            "value": 11,
        },
    )

    begin = rpc.call(
        "begin_checked_edit",
        {
            "doc_selector": selector,
            "revision_keys": [_property_key("SecondBox", "BetaValue")],
            "operation_id": f"{uuid.uuid4()}-independent-begin",
        },
        timeout=60.0,
    )
    assert begin.get("success") is True, begin
    session_id = begin["session_id"]

    success = rpc.call(
        "commit_checked_property",
        {
            "session_id": session_id,
            "doc_selector": selector,
            "object_name": "SecondBox",
            "property_name": "BetaValue",
            "value_type": "integer",
            "value": "30",
            "operation_id": str(uuid.uuid4()),
        },
        timeout=60.0,
    )
    assert success.get("success") is True, success
    assert success.get("committed") is True, success

    assert _object_property_value(rpc, document_name, "StressBox", "AlphaValue") == 11
    assert _object_property_value(rpc, document_name, "SecondBox", "BetaValue") == 30
    assert _property_revision(
        rpc, local, document_name, "StressBox", "AlphaValue"
    ) == alpha_before + 1
    assert _property_revision(
        rpc, local, document_name, "SecondBox", "BetaValue"
    ) == beta_before + 1


def test_pause_after_current_admitted_write_may_finish(
    freecad_gui_session, tmp_path: Path
) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    launcher_module = freecad_gui_session["launcher_module"]
    document_name = f"PauseAfterDoc_{uuid.uuid4().hex[:8]}"
    save_path = tmp_path / f"{document_name}.FCStd"
    _provision_alpha_beta(local, document_name, rpc, save_path)
    for index in range(40):
        rpc.call(
            "create_object",
            {
                "doc_name": document_name,
                "obj_data": {
                    "Name": f"PauseBox{index}",
                    "Type": "Part::Box",
                    "Properties": {
                        "Length": 5.0,
                        "Width": 5.0,
                        "Height": 5.0,
                    },
                },
            },
            timeout=60.0,
        )

    local.invoke("set_active_document", {"document": document_name})

    done_event = threading.Event()
    error_holder: list[str] = []

    def recompute_while_admitted() -> None:
        try:
            result = rpc.call(
                "recompute_document",
                [document_name],
                timeout=120.0,
            )
            if isinstance(result, dict) and result.get("success") is False:
                error_holder.append(str(result))
        except Exception as exc:
            error_holder.append(str(exc))
        finally:
            done_event.set()

    thread = threading.Thread(target=recompute_while_admitted, name="pause-after-current")
    thread.start()
    for _ in range(2000):
        if done_event.is_set():
            break
        readiness = _mutation_readiness(rpc, document_name)
        if _active_write_count(readiness) > 0 or _is_recomputing(readiness):
            break
        rpc.call("ping", timeout=2.0)
    local.invoke("pause_writes")
    thread.join(timeout=120.0)
    assert not thread.is_alive(), "recompute did not finish after blocker release"
    assert error_holder == []

    with pytest.raises(launcher_module.JsonRpcError) as paused_exc:
        _remote_edit_properties(
            rpc,
            document_name,
            "SecondBox",
            {"BetaValue": 7},
        )
    paused_text = f"{paused_exc.value} {paused_exc.value.data}"
    assert (
        "AUTOMATION_PAUSED" in paused_text
        or "paused new MCP writes" in paused_text
    )
    local.invoke("resume_writes")


def test_reads_while_paused_remain_available(
    freecad_gui_session, tmp_path: Path
) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    document_name = f"ReadsPausedDoc_{uuid.uuid4().hex[:8]}"
    save_path = tmp_path / f"{document_name}.FCStd"
    _provision_alpha_beta(local, document_name, rpc, save_path)

    local.invoke("set_active_document", {"document": document_name})
    selector = _identity_selector(local, document_name)
    local.invoke("pause_writes")

    revisions = rpc.call(
        "get_semantic_revisions",
        {
            "doc_selector": selector,
            "revision_keys": [_property_key("StressBox", "AlphaValue")],
        },
        timeout=30.0,
    )
    assert revisions.get("success") is True, revisions

    readiness = _mutation_readiness(rpc, document_name)
    assert _automation_paused(readiness)
    assert _active_write_count(readiness) == 0


def test_local_checkbox_resume_restores_admission(
    freecad_gui_session, tmp_path: Path
) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    document_name = f"ResumeDoc_{uuid.uuid4().hex[:8]}"
    save_path = tmp_path / f"{document_name}.FCStd"
    _provision_alpha_beta(local, document_name, rpc, save_path)

    local.invoke("set_active_document", {"document": document_name})
    local.invoke("pause_writes")
    local.invoke("resume_writes")

    admitted = _remote_edit_properties(
        rpc,
        document_name,
        "SecondBox",
        {"BetaValue": 9},
    )
    assert isinstance(admitted, dict)
    assert _object_property_value(rpc, document_name, "SecondBox", "BetaValue") == 9


def test_lost_response_retry_commits_once(
    freecad_gui_session, tmp_path: Path
) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    launcher_module = freecad_gui_session["launcher_module"]
    document_name = f"RetryDoc_{uuid.uuid4().hex[:8]}"
    save_path = tmp_path / f"{document_name}.FCStd"
    _provision_alpha_beta(
        local,
        document_name,
        rpc,
        save_path,
        alpha=2,
        beta=0,
    )

    local.invoke("set_active_document", {"document": document_name})
    selector = _identity_selector(local, document_name)
    before_alpha = _object_property_value(rpc, document_name, "StressBox", "AlphaValue")

    begin = rpc.call(
        "begin_checked_edit",
        {
            "doc_selector": selector,
            "revision_keys": [_property_key("StressBox", "AlphaValue")],
            "operation_id": f"{uuid.uuid4()}-retry-begin",
        },
        timeout=60.0,
    )
    assert begin.get("success") is True, begin
    session_id = begin["session_id"]
    operation_id = str(uuid.uuid4())
    commit_params = {
        "session_id": session_id,
        "doc_selector": selector,
        "object_name": "StressBox",
        "property_name": "AlphaValue",
        "value_type": "integer",
        "value": "25",
        "operation_id": operation_id,
    }
    first = rpc.call("commit_checked_property", commit_params, timeout=60.0)
    assert first.get("success") is True, first
    assert _object_property_value(rpc, document_name, "StressBox", "AlphaValue") == 25

    retry = rpc.call("commit_checked_property", commit_params, timeout=60.0)
    assert retry == first
    assert _object_property_value(rpc, document_name, "StressBox", "AlphaValue") == 25
    assert _object_property_value(rpc, document_name, "StressBox", "AlphaValue") != before_alpha

    with pytest.raises(launcher_module.JsonRpcError):
        rpc.call(
            "commit_checked_property",
            {
                **commit_params,
                "value": "13",
            },
            timeout=60.0,
        )


def test_healthy_conflict_does_not_poison_global_lane(
    freecad_gui_session, tmp_path: Path
) -> None:
    rpc = freecad_gui_session["rpc"]
    local = freecad_gui_session["local_driver"]
    launcher_module = freecad_gui_session["launcher_module"]
    document_name = f"LaneDoc_{uuid.uuid4().hex[:8]}"
    save_path = tmp_path / f"{document_name}.FCStd"
    _provision_alpha_beta(
        local,
        document_name,
        rpc,
        save_path,
        alpha=0,
        beta=0,
    )

    local.invoke("set_active_document", {"document": document_name})
    selector = _identity_selector(local, document_name)
    begin = rpc.call(
        "begin_checked_edit",
        {
            "doc_selector": selector,
            "revision_keys": [{"kind": "ObjectModel", "subject": "StressBox"}],
            "operation_id": f"{uuid.uuid4()}-lane-begin",
        },
        timeout=60.0,
    )
    assert begin.get("success") is True, begin
    session_id = begin["session_id"]

    _warm_property_editor(rpc, local, document_name)
    _prepare_local_property_edit(local, document_name, "StressBox")
    local.invoke(
        "local_property_edit",
        {
            "document": document_name,
            "object": "StressBox",
            "property": "AlphaValue",
            "value": 99,
        },
    )

    conflict = _expect_checked_edit_conflict(
        rpc,
        launcher_module,
        {
            "session_id": session_id,
            "doc_selector": selector,
            "object_name": "StressBox",
            "property_name": "AlphaValue",
            "value_type": "integer",
            "value": "5",
            "operation_id": str(uuid.uuid4()),
        },
    )
    assert conflict.get("success") is False, conflict
    assert conflict.get("error_code") == "DOCUMENT_CONFLICT" or (
        "semantic revisions changed" in str(conflict.get("error") or "")
    )

    readiness = _mutation_readiness(rpc, document_name)
    assert _readiness_flag(readiness, "quarantined") is False
    assert _readiness_flag(readiness, "collaboration_poisoned") is not True
    assert _readiness_flag(readiness, "ready") is True

    independent = _remote_edit_properties(
        rpc,
        document_name,
        "SecondBox",
        {"BetaValue": 44},
    )
    assert isinstance(independent, dict)
    assert _object_property_value(rpc, document_name, "SecondBox", "BetaValue") == 44

    rpc.call(
        "cancel_checked_edit",
        {
            "session_id": session_id,
            "reason": "integration cleanup",
            "operation_id": str(uuid.uuid4()),
        },
        timeout=30.0,
    )
