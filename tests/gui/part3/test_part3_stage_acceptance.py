# SPDX-License-Identifier: LGPL-2.1-or-later
"""Stage A and Stage B acceptance gate for Part 3 WP10 (ADR §8, §9, §13).

The live tests here run the real coordinator CLI end to end. They own their
FreeCAD session, so this module must stay outside the shared
``freecad_gui_session`` fixture. The offline tests prove the honesty rules that
do not need a GUI: Stage C is not executable, a forced kill is never green, and
the evidence recorders classify artifacts the way the save contract requires.

Running this file does not start FreeCAD. The two live tests launch a real GUI
session and bind MCP port 9875, so they run only when the operator opts in with
``PART3_STAGE_LIVE=1`` (GRK-P3-097). Without it they are NOT_RUN - which is
never a pass, and leaves Stage A/B acceptance unproven. They remain the
acceptance proof and are unchanged in what they assert.
"""

from __future__ import annotations

import ast
import contextlib
import hashlib
import inspect
import json
import os
import re
import socket
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from types import SimpleNamespace
from typing import Any

import pytest

from tests.gui.part3.evidence import (
    ALPHA_MODEL_KEY,
    ALPHA_OBJECT,
    ALPHA_PROPERTY,
    ALPHA_PROPERTY_KEY,
    BETA_MODEL_KEY,
    BETA_OBJECT,
    BETA_PROPERTY,
    BETA_PROPERTY_KEY,
    SHUTDOWN_TIMESTAMP_KEYS,
    binary_fingerprint,
    classify_artifact,
    empty_evidence,
    empty_shutdown_record,
    git_state,
    record_check,
    scan_artifacts,
    sha256_file,
    stamp_shutdown_transition,
    verdict_from_checks,
    write_evidence,
)
from tests.gui.part3.scenarios import (
    COVERAGE_ITEMS,
    EXECUTABLE_STAGES,
    STAGE_A,
    STAGE_B,
    STAGE_C,
    resolve_executable_stage,
    resolve_stage,
)
from tests.gui.part3.stress_coordinator import (
    default_freecad_exe,
    main as coordinator_main,
    run_stage,
    stage_verdict,
)

REPO_ROOT = Path(__file__).resolve().parents[3]
PACKAGE_ROOT = Path(__file__).resolve().parent
COORDINATOR = PACKAGE_ROOT / "stress_coordinator.py"
MCP_RPC_PORT = 9875
EVIDENCE_LINE = re.compile(r"^evidence:\s*(?P<path>.+)$", re.MULTILINE)
HANDOFF_ENV = "PART3_STAGE_EVIDENCE_HANDOFF"

STAGE_TIMEOUTS = {"a": 3600.0, "b": 10800.0}


def test_provision_waits_for_transient_readiness_before_each_first_save(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from tests.gui.part3 import stress_coordinator as coordinator

    events: list[tuple[Any, ...]] = []
    readiness_calls: dict[str, int] = {}
    waits: list[float] = []
    checks: list[tuple[str, Any]] = []

    class Rpc:
        def call(self, method: str, params: Any, *, timeout: float) -> Any:
            del timeout
            if method == "create_document":
                events.append(("create", params["name"]))
                return {"success": True}
            if method == "get_mutation_readiness":
                document = params["doc_name"]
                call = readiness_calls.get(document, 0)
                readiness_calls[document] = call + 1
                ready = call > 0
                snapshot = {
                    "ready": ready,
                    "quarantined": False,
                    "collaboration_poisoned": False,
                }
                events.append(("readiness", document, ready))
                return snapshot
            if method == "save_document_as":
                document = params["selector"]["document_name"]
                events.append(("save", document))
                assert readiness_calls[document] == 2
                return {"saved": True}
            raise AssertionError(f"unexpected RPC method: {method}")

    context = SimpleNamespace(
        primary="Primary",
        secondary="Secondary",
        rpc=Rpc(),
        launcher_module=SimpleNamespace(JsonRpcError=RuntimeError),
        active_document=None,
        documents_dir=tmp_path,
        document_paths={},
        coverage=set(),
    )

    def local_action(
        _context: Any,
        _cycle: Any,
        action: str,
        params: dict[str, Any],
        **_kwargs: Any,
    ) -> dict[str, Any]:
        events.append((action, params["document"]))
        return {"success": True}

    def require(_context: Any, name: str, passed: bool, detail: Any = None) -> None:
        assert passed is True
        checks.append((name, detail))

    monkeypatch.setattr(coordinator, "_local_action", local_action)
    monkeypatch.setattr(coordinator, "_require", require)
    monkeypatch.setattr(
        coordinator,
        "_DOCUMENT_READINESS_POLL_EVENT",
        SimpleNamespace(wait=lambda seconds: waits.append(seconds)),
    )

    coordinator._provision_stage_documents(context)

    for document in (context.primary, context.secondary):
        assert events.index(("provision_alpha_beta_fixture", document)) < events.index(
            ("readiness", document, False)
        )
        assert events.index(("readiness", document, False)) < events.index(
            ("readiness", document, True)
        ) < events.index(("save", document))
    ready_checks = [
        detail
        for name, detail in checks
        if name == "stage_document_ready_before_first_save"
    ]
    assert [detail["document"] for detail in ready_checks] == ["Primary", "Secondary"]
    assert all(detail["readiness"]["ready"] is True for detail in ready_checks)
    assert waits and all(
        0 < seconds <= coordinator.DOCUMENT_READINESS_POLL_INTERVAL_SECONDS
        for seconds in waits
    )


@pytest.mark.parametrize("poison_field", ["quarantined", "collaboration_poisoned"])
def test_first_save_readiness_poison_fails_closed_without_polling(
    monkeypatch: pytest.MonkeyPatch, poison_field: str
) -> None:
    from tests.gui.part3 import stress_coordinator as coordinator

    snapshot = {
        "ready": False,
        "quarantined": poison_field == "quarantined",
        "collaboration_poisoned": poison_field == "collaboration_poisoned",
    }
    context = SimpleNamespace(
        rpc=SimpleNamespace(
            call=lambda method, params, timeout: snapshot
        )
    )
    monkeypatch.setattr(
        coordinator,
        "_DOCUMENT_READINESS_POLL_EVENT",
        SimpleNamespace(
            wait=lambda _seconds: pytest.fail("poison must fail without polling")
        ),
    )

    with pytest.raises(coordinator.StageCheckFailed, match=poison_field):
        coordinator._wait_for_document_ready_before_first_save(context, "Poisoned")


def test_first_save_readiness_timeout_is_bounded_without_real_waiting(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from tests.gui.part3 import stress_coordinator as coordinator

    snapshot = {"ready": False, "reason": "pending recompute"}
    ticks = iter([0.0, 0.0, 120.0])
    context = SimpleNamespace(
        rpc=SimpleNamespace(
            call=lambda method, params, timeout: snapshot
        )
    )
    monkeypatch.setattr(coordinator, "monotonic", lambda: next(ticks))
    monkeypatch.setattr(
        coordinator,
        "_DOCUMENT_READINESS_POLL_EVENT",
        SimpleNamespace(
            wait=lambda _seconds: pytest.fail("deadline control must not really wait")
        ),
    )

    assert coordinator.DOCUMENT_READY_BEFORE_FIRST_SAVE_TIMEOUT_SECONDS == 120.0
    with pytest.raises(TimeoutError) as error:
        coordinator._wait_for_document_ready_before_first_save(
            context,
            "Pending",
            timeout_seconds=coordinator.DOCUMENT_READY_BEFORE_FIRST_SAVE_TIMEOUT_SECONDS,
        )
    assert "last_readiness" in str(error.value)
    assert repr(snapshot) in str(error.value)


def _personal_revision_vector(base_revision: int) -> list[dict[str, int | str]]:
    """Build the canonical revision vector emitted by personal snapshots."""

    return [
        {"kind": "ObjectModel", "subject": BETA_OBJECT, "revision": base_revision + 3},
        {"kind": "ObjectModel", "subject": ALPHA_OBJECT, "revision": base_revision + 2},
        {
            "kind": "ObjectProperty", "subject": BETA_OBJECT,
            "property_name": BETA_PROPERTY, "revision": base_revision + 1,
        },
        {
            "kind": "ObjectProperty", "subject": ALPHA_OBJECT,
            "property_name": ALPHA_PROPERTY, "revision": base_revision,
        },
    ]


def _port_is_open(host: str, port: int) -> bool:
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as sock:
        sock.settimeout(0.75)
        return sock.connect_ex((host, port)) == 0


LIVE_STAGE_OPT_IN_ENV = "PART3_STAGE_LIVE"
LIVE_STAGE_OPT_IN_VALUES = frozenset({"1", "true", "yes", "on"})


def _clear_freecad_overrides(monkeypatch: pytest.MonkeyPatch) -> None:
    for name in ("FREECAD", "FC_FREECAD", "FREECAD_EXE"):
        monkeypatch.delenv(name, raising=False)


def test_default_freecad_exe_linux_selects_extensionless_debug_candidate(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """GRK-P3-094: a Windows PE must not mask a usable Linux debug build."""

    from tests.gui.part3 import stress_coordinator as module

    _clear_freecad_overrides(monkeypatch)
    monkeypatch.setattr(module.sys, "platform", "linux")
    windows_binary = tmp_path / "build" / "release" / "bin" / "FreeCAD.exe"
    linux_binary = tmp_path / "build" / "debug" / "bin" / "FreeCAD"
    windows_binary.parent.mkdir(parents=True)
    linux_binary.parent.mkdir(parents=True)
    windows_binary.write_bytes(b"MZ")
    linux_binary.write_bytes(b"\x7fELF")

    assert module.default_freecad_exe(tmp_path) == linux_binary


@pytest.mark.parametrize("override_name", ("FREECAD", "FC_FREECAD", "FREECAD_EXE"))
def test_default_freecad_exe_honors_launcher_override_names(
    override_name: str,
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The coordinator and tracked launcher share one override vocabulary."""

    from tests.gui.part3 import stress_coordinator as module

    _clear_freecad_overrides(monkeypatch)
    monkeypatch.setattr(module.sys, "platform", "linux")
    override = tmp_path / "custom" / "FreeCAD"
    override.parent.mkdir()
    override.write_bytes(b"\x7fELF")
    monkeypatch.setenv(override_name, str(override))

    assert module.default_freecad_exe(tmp_path) == override.resolve()


def test_default_freecad_exe_windows_selects_only_exe_candidates(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """An extensionless Linux artifact must never be selected on Windows."""

    from tests.gui.part3 import stress_coordinator as module

    _clear_freecad_overrides(monkeypatch)
    monkeypatch.setattr(module.sys, "platform", "win32")
    linux_binary = tmp_path / "build" / "release" / "bin" / "FreeCAD"
    windows_binary = tmp_path / "build" / "debug" / "bin" / "FreeCAD.exe"
    linux_binary.parent.mkdir(parents=True)
    windows_binary.parent.mkdir(parents=True)
    linux_binary.write_bytes(b"\x7fELF")
    windows_binary.write_bytes(b"MZ")

    assert module.default_freecad_exe(tmp_path) == windows_binary


def test_default_freecad_exe_returns_none_when_candidates_are_missing(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Discovery remains fail closed when neither overrides nor builds exist."""

    from tests.gui.part3 import stress_coordinator as module

    _clear_freecad_overrides(monkeypatch)
    monkeypatch.setattr(module.sys, "platform", "linux")

    assert module.default_freecad_exe(tmp_path) is None


def live_stage_opt_in() -> bool:
    """True only when the operator explicitly asked for a real GUI session."""

    value = os.environ.get(LIVE_STAGE_OPT_IN_ENV, "")
    return value.strip().lower() in LIVE_STAGE_OPT_IN_VALUES


def _require_stage_preconditions() -> None:
    """Refuse to start a FreeCAD session that the operator did not ask for.

    GRK-P3-097: this file used to launch a real GUI for anyone who ran it,
    because the only guards were an absent binary and a busy port - and on a
    correctly provisioned host both are favourable, so launching was the
    default. It happened to a reviewer who was forbidden from launching
    FreeCAD at all. The opt-in below is checked first, before anything looks
    at the binary or the port.

    A skip here is NOT_RUN. It is never a pass, and it never turns into one:
    the two live tests are untouched in what they assert and remain the
    Stage A/B acceptance proof.
    """

    if not live_stage_opt_in():
        pytest.skip(
            "NOT_RUN (never a pass): this test owns a real FreeCAD GUI session "
            f"and binds MCP port {MCP_RPC_PORT}. Set "
            f"{LIVE_STAGE_OPT_IN_ENV}=1 to opt in. Until it is set, Stage A/B "
            "acceptance is unproven."
        )
    if default_freecad_exe(REPO_ROOT) is None:
        pytest.skip(
            "FreeCAD GUI binary not found via launcher overrides or "
            "under build/release or build/debug"
        )
    if _port_is_open("127.0.0.1", MCP_RPC_PORT):
        pytest.skip(
            f"MCP port {MCP_RPC_PORT} is already occupied; "
            "refusing to attach to an unknown session"
        )


def _run_stage_cli(stage: str) -> tuple[subprocess.CompletedProcess[str], dict[str, Any]]:
    completed = subprocess.run(
        [sys.executable, str(COORDINATOR), "--stage", stage],
        cwd=str(REPO_ROOT),
        capture_output=True,
        text=True,
        check=False,
        timeout=STAGE_TIMEOUTS[stage],
    )
    match = EVIDENCE_LINE.search(completed.stderr or "")
    assert match is not None, completed.stderr[-4000:]
    evidence_path = Path(match.group("path").strip())
    assert evidence_path.is_file(), evidence_path
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    handoff_value = os.environ.get(HANDOFF_ENV, "").strip()
    if handoff_value:
        handoff_path = Path(handoff_value)
        handoff_path.parent.mkdir(parents=True, exist_ok=True)
        temporary = handoff_path.with_name(f".{handoff_path.name}.{os.getpid()}.tmp")
        temporary.write_text(
            json.dumps(
                {
                    "schema_version": 1,
                    "stage": stage.upper(),
                    "coordinator_return_code": int(completed.returncode),
                    "evidence_path": str(evidence_path.resolve()),
                    "launcher_path": str(
                        (evidence_path.parents[1] / "launcher.log").resolve()
                    ),
                },
                indent=2,
                sort_keys=True,
            )
            + "\n",
            encoding="utf-8",
            newline="\n",
        )
        os.replace(temporary, handoff_path)
    return completed, evidence


def _assert_green_stage(
    completed: subprocess.CompletedProcess[str],
    evidence: dict[str, Any],
    stage: str,
    view_mutation_cycles: int,
    save_cycles: int,
) -> None:
    tail = f"\nSTDOUT:\n{completed.stdout[-4000:]}\nSTDERR:\n{completed.stderr[-4000:]}"
    assert "PART3_RESULT: PASSED" in (completed.stdout or ""), tail
    assert completed.returncode == 0, tail

    # Both the direct stage assertion and retained packets use one production
    # predicate.  Passing explicit counts keeps reduced unit fixtures honest
    # without maintaining a second semantic validator here.
    from tests.gui.part3.evidence import validate_completed_stage_evidence
    try:
        validate_completed_stage_evidence(
            evidence,
            stage=stage,
            view_mutation_cycles=view_mutation_cycles,
            save_cycles=save_cycles,
            repo_root=REPO_ROOT,
        )
    except ValueError as exc:
        raise AssertionError(str(exc)) from exc


def _personal_view_stage_fixture(*, clean: bool) -> dict[str, Any]:
    """Small complete `_assert_green_stage` fixture for ADR section 4."""

    if clean:
        # Reuse the complete producer-shaped fixture so the direct assertion
        # cannot drift from the retained-packet semantic schedule.
        from tests.gui.part3.test_part3_stage_gate_runner import _stage_evidence

        evidence = _stage_evidence("A")
        evidence["cycles"] = evidence["cycles"][:2]
        final_save = evidence["saves"][-1]
        final_save["index"] = 0
        evidence["saves"] = [final_save]
        retained_proofs = evidence["personal_action_proofs"][:20]
        out_of_cycle_action = evidence["out_of_cycle_local_actions"][0]
        out_of_cycle_proof = dict(
            evidence["personal_action_proofs"][
                out_of_cycle_action["personal_action_proof_index"]
            ]
        )
        out_of_cycle_proof["index"] = len(retained_proofs)
        out_of_cycle_proof["operation_id"] = out_of_cycle_action["operation_id"]
        out_of_cycle_action["personal_action_proof_index"] = out_of_cycle_proof["index"]
        evidence["personal_action_proofs"] = retained_proofs + [out_of_cycle_proof]
        evidence["_complete_fixture"] = True
        return evidence

    from tests.gui.part3.stress_coordinator import STAGE_SOURCE_FILES
    from tests.gui.part3.test_part3_stage_gate_runner import _stage_evidence

    fixture_documents = _stage_evidence("A")["artifacts"]["documents"]
    fixture_canonical = fixture_documents[0]["path"]
    fixture_copy = fixture_documents[1]["path"]
    file_state = {
        "pending_changes": [] if clean else ["model"],
        "has_pending_file_changes": not clean,
    }
    return {
        "schema_version": 2,
        "stage": "A",
        "mode": "stage",
        "verdict": "PASSED",
        "failed_checks": [],
        "cycles": [
            {
                "index": 0,
                "checks": {
                    "personal_view_state_inert": True,
                    "personal_view_state_not_dirtying": clean,
                    "typed_mutation_committed_once": True,
                },
                "document": "Primary",
                "written_result": {"saved": True, "save_disposition": "written", "file_written": True, "durability_verified": True},
                "revisions_before": [{"key": "ObjectProperty:Beta:Length", "revision": 1}],
                "revisions_after_personal_view": [{"key": "ObjectProperty:Beta:Length", "revision": 1}],
                "revisions_after": [{"key": "ObjectProperty:Beta:Length", "revision": 2}],
                "file_change_state_before": dict(file_state),
                "file_change_state_after_personal_view": dict(file_state),
                "file_change_state_after": dict(file_state),
                "readiness": {"success": True, "documents": [{"quarantined": False, "collaboration_poisoned": False}]},
                "typed_mutation": {"operation_id": "remote-commit-0", "revision_before": 1, "revision_after": 2, "expected_value": 100, "landed_value": 100, "first_result": {"success": True, "committed": True}, "replay_result": {"success": True, "committed": True}},
                "local_actions": [{
                    "operation_id": "local-personal-0", "action": "rotate_camera",
                    "ack_utc": "2026-08-22T00:00:00+00:00", "observed": {},
                    "personal_action_proof_index": 0,
                }],
                "remote_actions": [{
                    "operation_id": "remote-commit-0", "method": "commit_checked_property",
                    "ack_utc": "2026-08-22T00:00:00+00:00", "result_envelope": {"success": True, "committed": True},
                }, {
                    "operation_id": "remote-commit-0", "method": "commit_checked_property",
                    "ack_utc": "2026-08-22T00:00:00+00:00", "result_envelope": {"success": True, "committed": True}, "committed_once": True,
                }, {
                    "operation_id": "remote-recompute-0", "method": "recompute_document",
                    "ack_utc": "2026-08-22T00:00:00+00:00", "result_envelope": {"success": True},
                }],
            }
        ],
        "personal_action_proofs": [
            {
                "index": 0,
                "operation_id": "local-personal-0",
                "action": "rotate_camera",
                "documents": ["Primary"],
                "before": {
                    "Primary": {
                        "observed_document": "Primary",
                        "identity_selector": {
                            "document_uid": "uid-Primary",
                            "document_instance_id": 11,
                            "lifecycle_epoch": 2,
                            "document_name": "Primary",
                        },
                        "file_change_state": dict(file_state),
                        "semantic_revisions": _personal_revision_vector(1),
                    }
                },
                "after": {
                    "Primary": {
                        "observed_document": "Primary",
                        "identity_selector": {
                            "document_uid": "uid-Primary",
                            "document_instance_id": 11,
                            "lifecycle_epoch": 2,
                            "document_name": "Primary",
                        },
                        "file_change_state": dict(file_state),
                        "semantic_revisions": _personal_revision_vector(1),
                    }
                },
                "clean_before": clean,
                "clean_after": clean,
                "semantic_revisions_unchanged": True,
                "passed": clean,
            }
        ],
        "saves": [
            {
                "index": 0,
                "truthful": True,
                "disposition": "written",
                "file_written": True,
                "durability_verified": True,
                "sha256_before": "a" * 64,
                "sha256_after": "b" * 64,
                "document": "Primary",
                "written_result": {"saved": True, "save_disposition": "written", "file_written": True, "durability_verified": True},
                "unchanged_save": {
                    "file_written": False,
                    "disposition": "unchanged",
                    "sha256_after": "b" * 64,
                    "result": {"unchanged": True, "save_disposition": "unchanged", "file_written": False},
                },
                "actual_save_operations": [
                    {"kind": "canonical_written_save", "document": "Primary", "sha256_before": "a" * 64, "sha256_after": "b" * 64, "disposition": "written", "file_written": True, "durability_verified": True, "truthful": True, "result": {"saved": True, "save_disposition": "written", "file_written": True, "durability_verified": True}},
                    {"kind": "canonical_unchanged_save", "document": "Primary", "sha256_before": "b" * 64, "sha256_after": "b" * 64, "disposition": "unchanged", "file_written": False, "truthful": True, "result": {"unchanged": True, "save_disposition": "unchanged", "file_written": False}},
                    {"kind": "save_copy", "document": "Primary", "destination": fixture_copy, "canonical_sha256_before": "b" * 64, "canonical_sha256_after": "b" * 64, "disposition": "copy_written", "file_written": True, "truthful": True, "result": {"saved": True, "save_disposition": "copy_written", "file_written": True}},
                ],
                "canonical_path": fixture_canonical,
                "save_copy": {
                    "destination": fixture_copy,
                    "readable_archive": True,
                    "canonical_unchanged": True,
                    "result": {"saved": True, "save_disposition": "copy_written", "file_written": True},
                },
            }
        ],
        "shutdown": {
            "forced": False,
            "stalled_stage": None,
            "failed_step": None,
            "rpc_error": None,
            "deadline_seconds": 60,
            "requested_utc": "2026-08-22T00:00:00+00:00",
            "documents_closed_utc": "2026-08-22T00:00:01+00:00",
            "rpc_admission_closed_utc": "2026-08-22T00:00:02+00:00",
            "worker_shutdown_utc": "2026-08-22T00:00:03+00:00",
            "listener_shutdown_utc": "2026-08-22T00:00:04+00:00",
            "window_closed_utc": "2026-08-22T00:00:05+00:00",
            "process_exit_utc": "2026-08-22T00:00:06+00:00",
        },
        "environment": {
            "binary_fingerprint": {
                "FreeCAD.exe": {"sha256": "c" * 64, "size": 1, "mtime_ns": 1}
            },
            "git": {
                "parent_commit": "d" * 40,
                "nested_commit": "e" * 40,
                "recorded_gitlink": "e" * 40,
                "branch": "fix/change-aware-save-mcp-autonomy",
            },
            "isolation_verified": True,
            "reported_user_app_data": "isolated-profile",
            "auth": {
                "v2_session": True,
                "session_token_present": True,
                "session_token_length": 32,
                "mcp_instance_id": "instance",
                "profile_instance_id": "profile",
                "protocol_version": 2,
            },
            "remote_actor": {
                "mode": "in_process_typed_session",
                "child_token_absence_proved": True,
                "adr_deviation": "section 1.1",
                "holds_rpc_session_in_coordinator_process": True,
            },
            "session_ttl": {
                "environment_variable": "FREECAD_MCP_SESSION_TTL_SECONDS",
                "override_present": False,
                "override_value": None,
                "effective_seconds": 300.0,
                "default_seconds": 300.0,
                "source": "default",
            },
            "build_provenance": {
                "head_commit": "d" * 40,
                "head_committed_utc": "2026-08-22T00:00:00+00:00",
                "history_depth": 1,
                "binaries": {
                    "FreeCAD.exe": {
                        "mtime_utc": "2026-08-22T00:00:00+00:00",
                        "commits_not_in_binary": [], "predates_head": False,
                    }
                },
                "binaries_predating_head": [],
                "binary_commit_binding_enforced": False,
                "provenance_caveat": "Recorded, not enforced.",
                "stage_sources": {
                    relative: sha256_file(REPO_ROOT.joinpath(*relative.split("/")))
                    for relative in STAGE_SOURCE_FILES
                },
            },
        },
        "artifacts": {
            "documents": [
                dict(fixture_documents[0]),
                dict(fixture_documents[1]),
            ],
            "lock_anchors": [], "unexplained": [],
        },
        "coverage": {
            "required": list(COVERAGE_ITEMS),
            "observed": list(COVERAGE_ITEMS),
            "missing": [],
        },
        "conflicts": {
            "same_property": {"document": "Primary", "targeted": True, "write_lane_healthy": True, "refusal": {"success": False, "error_code": "DOCUMENT_CONFLICT"}, "changed_semantic_keys": ["ObjectProperty:Alpha:Length"], "expected_revisions": {"ObjectProperty:Alpha:Length": 1}, "current_revisions": {"ObjectProperty:Alpha:Length": 2}, "readiness": {"success": True, "documents": [{"document": "Primary", "ready": True, "quarantined": False, "collaboration_poisoned": False}]}},
            "independent_property": {"document": "Secondary", "both_landed": True, "committed_once": True, "commit": {"success": True, "committed": True}, "replay": {"success": True, "committed": True}, "alpha_revision_before": 1, "alpha_revision_after": 2, "beta_revision_before": 1, "beta_revision_after": 2, "alpha_value": 11, "beta_value": 30},
        },
        "pause_resume": {
            "pause": {"observed": {"paused": True}},
            "refused": {
                "success": False, "error_code": "AUTOMATION_PAUSED",
                "error": "paused new MCP writes", "data": {},
            },
            "readiness_while_paused": {
                "success": True,
                "documents": [{"automation_paused": True, "active_write_count": 0}],
                "automation_pause": {"paused": True, "active_write_count": 0},
            },
            "resume": {"observed": {"paused": False}},
            "after": {"commit": {"success": True}, "value": 55},
        },
        "history": {
            "undo_head": {"count": 1, "head": "undo"},
            "undo_head_refusal": {
                "success": False,
                "error_code": "HISTORY_HEAD_REJECTED",
                "current_undo_count": 1,
                "current_undo_head": "undo",
            },
            "undo_result": {"success": True}, "value_after_edit": 77, "value_after_undo": 0,
            "redo_head": {"count": 1, "head": "redo"},
            "redo_head_refusal": {
                "success": False,
                "error_code": "HISTORY_HEAD_REJECTED",
                "current_redo_count": 1, "current_redo_head": "redo",
            },
            "redo_result": {"success": True}, "value_after_redo": 77,
            "mismatched_head_refused": True,
        },
    }


def _record_personal_view_fixture_aggregate(payload: dict[str, Any]) -> None:
    from tests.gui.part3 import stress_coordinator as module

    aggregate_names = {
        module.CYCLE_COUNT_CHECK,
        module.SAVE_COUNT_CHECK,
        "adr_section_13_coverage_complete",
        "every_save_cycle_is_truthful",
        "every_personal_action_has_exact_clean_revision_proof",
        "every_cycle_keeps_personal_view_state_inert",
        "every_cycle_starts_and_ends_personal_view_file_clean",
        "every_cycle_commits_its_typed_mutation_once",
    }
    payload["checks"] = [
        entry for entry in payload.get("checks", [])
        if not isinstance(entry, dict) or entry.get("name") not in aggregate_names
    ]
    payload["failed_checks"] = [
        entry for entry in payload.get("failed_checks", [])
        if not isinstance(entry, dict) or entry.get("name") not in aggregate_names
    ]
    coverage = payload.get("coverage")
    observed = coverage.get("observed", []) if isinstance(coverage, dict) else []
    context = SimpleNamespace(
        payload=payload,
        definition=SimpleNamespace(
            view_mutation_cycles=len(payload["cycles"]),
            save_cycles=len(payload["saves"]),
        ),
        coverage=set(observed),
    )
    payload.pop("_complete_fixture", None)
    module._record_stage_aggregates(context)
    # This reduced fixture models the producer's post-shutdown observations
    # that the normal stage runner records after aggregate collection.
    from tests.gui.part3.evidence import record_check

    existing = {entry["name"] for entry in payload.get("checks", [])}
    for name in (
        "artifact_scan_has_no_unexplained_files",
        "saved_documents_are_ordinarily_readable_archives",
        "lock_anchors_are_classified_separately",
        "build_provenance_is_recorded_for_binaries_and_stage_sources",
        "head_bound_undo_reverts_the_local_transaction",
        "redo_stack_records_the_reverted_transaction",
        "head_bound_redo_restores_the_reverted_transaction",
        "remote_write_refused_while_paused",
        "reads_remain_available_while_paused",
        "next_typed_mutation_succeeds_after_resume",
        "graceful_shutdown_completed_without_forced_termination",
    ):
        if name not in existing:
            record_check(payload, name, True, {"fixture": True})


def test_required_scenario_passes_are_retained_by_the_live_check_gate(
    tmp_path: Path,
) -> None:
    from tests.gui.part3 import stress_coordinator as module

    context = SimpleNamespace(
        payload=empty_evidence(stage="A"),
        evidence_path=tmp_path / "evidence.json",
    )
    for name in module.RECORDED_SCENARIO_CHECKS:
        module._require(context, name, True, {"observed": name})
    module._require(context, "low_level_cycle_check", True, {"observed": True})

    checks = context.payload["checks"]
    assert {entry["name"] for entry in checks} == set(module.RECORDED_SCENARIO_CHECKS)
    assert all(entry["passed"] is True for entry in checks)
    assert context.payload["failed_checks"] == []


def test_dirty_personal_view_fixture_fails_stage_aggregate() -> None:
    """GRK-P3-122 RED: equality of two dirty observations is not clean-state proof."""

    evidence = _personal_view_stage_fixture(clean=False)
    _record_personal_view_fixture_aggregate(evidence)
    checks = {entry["name"]: entry for entry in evidence["checks"]}
    exact = checks["every_cycle_starts_and_ends_personal_view_file_clean"]
    assert exact["passed"] is False, exact
    assert exact in evidence["failed_checks"]


def test_dirty_personal_view_fixture_fails_green_stage_validator() -> None:
    """GRK-P3-122 RED: the final validator must reject unchanged-but-dirty state."""

    evidence = _personal_view_stage_fixture(clean=False)
    completed = SimpleNamespace(returncode=0, stdout="PART3_RESULT: PASSED\n", stderr="")
    with pytest.raises(AssertionError):
        _assert_green_stage(completed, evidence, "A", 1, 1)


def test_clean_personal_view_fixture_passes_both_validators() -> None:
    """GRK-P3-122 clean control: before/after empty and false remains green."""

    evidence = _personal_view_stage_fixture(clean=True)
    proof_indexes = [proof["index"] for proof in evidence["personal_action_proofs"]]
    out_of_cycle_action = evidence["out_of_cycle_local_actions"][0]
    assert proof_indexes == list(range(21))
    assert out_of_cycle_action["personal_action_proof_index"] == proof_indexes[-1]
    assert (
        evidence["personal_action_proofs"][-1]["operation_id"]
        == out_of_cycle_action["operation_id"]
    )
    _record_personal_view_fixture_aggregate(evidence)
    checks = {entry["name"]: entry for entry in evidence["checks"]}
    exact = checks["every_cycle_starts_and_ends_personal_view_file_clean"]
    assert exact["passed"] is True, exact
    assert evidence["failed_checks"] == []
    completed = SimpleNamespace(returncode=0, stdout="PART3_RESULT: PASSED\n", stderr="")
    _assert_green_stage(
        completed, evidence, "A", len(evidence["cycles"]), len(evidence["saves"])
    )


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("defect", ["binary_path", "git_identity"])
def test_direct_stage_assertion_rejects_fake_runtime_provenance(
    stage: str, defect: str
) -> None:
    """Completed direct A/B evidence is always bound to this repository/runtime."""

    evidence = _personal_view_stage_fixture(clean=True)
    evidence["stage"] = stage
    _record_personal_view_fixture_aggregate(evidence)
    if defect == "binary_path":
        evidence["environment"]["binary_fingerprint"]["FreeCAD.exe"]["path"] = "missing/FreeCAD.exe"
    else:
        evidence["environment"]["git"]["parent_commit"] = "f" * 40
        evidence["environment"]["build_provenance"]["head_commit"] = "f" * 40
    completed = SimpleNamespace(returncode=0, stdout="PART3_RESULT: PASSED\n", stderr="")
    with pytest.raises(AssertionError):
        _assert_green_stage(
            completed,
            evidence,
            stage,
            len(evidence["cycles"]),
            len(evidence["saves"]),
        )


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("defect", ["missing", "corrupt"])
@pytest.mark.parametrize(
    "family",
    [
        "provenance", "archive", "paused_refusal", "paused_read", "post_resume", "undo", "redo",
        "history_values", "provenance_commit", "provenance_binaries", "provenance_predating",
        "artifact_backup", "artifact_wrong_bucket", "artifact_malformed",
    ],
)
def test_shared_green_stage_predicate_rejects_each_new_decisive_field(
    stage: str, defect: str, family: str
) -> None:
    """Direct A/B assertions use the same strict predicate as packets."""

    evidence = _personal_view_stage_fixture(clean=True)
    evidence["stage"] = stage
    _record_personal_view_fixture_aggregate(evidence)
    if family == "provenance":
        target = evidence["environment"]
        key = "build_provenance"
        if defect == "missing":
            target.pop(key)
        else:
            target[key]["stage_sources"] = {"source": "bad"}
    elif family == "archive":
        if defect == "missing":
            evidence["artifacts"].pop("lock_anchors")
        else:
            evidence["artifacts"]["documents"][0]["readable_archive"] = False
    elif family == "paused_refusal":
        if defect == "missing":
            evidence["pause_resume"].pop("refused")
        else:
            evidence["pause_resume"]["refused"]["success"] = True
    elif family == "paused_read":
        if defect == "missing":
            evidence["pause_resume"].pop("readiness_while_paused")
        else:
            evidence["pause_resume"]["readiness_while_paused"] = {
                "mutation_readiness": [{"ready": True}],
            }
    elif family == "post_resume":
        if defect == "missing":
            evidence["pause_resume"].pop("after")
        else:
            evidence["pause_resume"]["after"]["value"] = 0
    elif family == "undo":
        if defect == "missing":
            evidence["history"].pop("undo_result")
        else:
            evidence["history"]["undo_result"]["success"] = False
    elif defect == "missing":
        if family == "redo":
            evidence["history"]["redo_head_refusal"].pop("current_redo_head")
        elif family == "history_values":
            evidence["history"].pop("value_after_edit")
        elif family == "provenance_commit":
            evidence["environment"]["build_provenance"].pop("head_commit")
        elif family == "provenance_binaries":
            evidence["environment"]["build_provenance"].pop("binaries")
        elif family == "provenance_predating":
            evidence["environment"]["build_provenance"].pop("binaries_predating_head")
        else:
            evidence["artifacts"].pop("documents")
    elif family == "history_values":
        evidence["history"]["value_after_undo"] = evidence["history"]["value_after_edit"]
        evidence["history"]["value_after_redo"] = 0
    elif family == "provenance_commit":
        evidence["environment"]["build_provenance"]["head_commit"] = "f" * 40
    elif family == "provenance_binaries":
        evidence["environment"]["build_provenance"]["binaries"] = {"wrong": {}}
    elif family == "provenance_predating":
        evidence["environment"]["build_provenance"]["binaries_predating_head"] = ["FreeCAD.exe"]
    elif family == "artifact_backup":
        evidence["artifacts"]["documents"] = [{"path": "legacy.fcbak", "size": 1, "readable_archive": False}]
    elif family == "artifact_wrong_bucket":
        evidence["artifacts"]["documents"] = [{"path": "model.FCStd.FreeCAD-save.lock", "size": 1}]
    elif family == "artifact_malformed":
        evidence["artifacts"]["documents"] = [{"path": "model.FCStd", "readable_archive": True}]
    else:
        evidence["history"]["redo_result"]["success"] = False
    completed = SimpleNamespace(returncode=0, stdout="PART3_RESULT: PASSED\n", stderr="")
    with pytest.raises(AssertionError):
        _assert_green_stage(
            completed,
            evidence,
            stage,
            len(evidence["cycles"]),
            len(evidence["saves"]),
        )


@pytest.mark.parametrize("stage", ["A", "B"])
def test_shared_green_stage_predicate_accepts_producer_shaped_backup_artifacts(
    stage: str,
) -> None:
    evidence = _personal_view_stage_fixture(clean=True)
    evidence["stage"] = stage
    evidence["artifacts"]["documents"].extend(
        [
            {"path": "legacy.fcbak", "size": 2},
            {"path": "numbered.FCStd1", "size": 3},
        ]
    )
    _record_personal_view_fixture_aggregate(evidence)
    completed = SimpleNamespace(returncode=0, stdout="PART3_RESULT: PASSED\n", stderr="")
    _assert_green_stage(
        completed, evidence, stage, len(evidence["cycles"]), len(evidence["saves"])
    )


def test_shared_green_stage_predicate_accepts_real_paused_readiness_shape() -> None:
    evidence = _personal_view_stage_fixture(clean=True)
    primary_document = evidence["cycles"][0]["document"]
    readiness = {
        "success": True,
        "documents": [{
            "document": primary_document,
            "automation_paused": True,
            "active_write_count": 0,
        }],
        "automation_pause": {"paused": True, "active_write_count": 0},
    }
    evidence["pause_resume"]["readiness_while_paused"] = readiness
    _record_personal_view_fixture_aggregate(evidence)
    completed = SimpleNamespace(returncode=0, stdout="PART3_RESULT: PASSED\n", stderr="")
    _assert_green_stage(
        completed, evidence, "A", len(evidence["cycles"]), len(evidence["saves"])
    )


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize(
    "defect",
    [
        "orphan_canonical", "orphan_copy", "source_digest", "commit_relation",
        "gitlink", "readiness_active", "readiness_missing_count", "readiness_extra_document",
    ],
)
def test_shared_green_stage_predicate_rejects_exact_evidence_bindings(
    stage: str, defect: str
) -> None:
    evidence = _personal_view_stage_fixture(clean=True)
    evidence["stage"] = stage
    _record_personal_view_fixture_aggregate(evidence)
    if defect == "orphan_canonical":
        canonical_path = evidence["saves"][0]["canonical_path"]
        evidence["artifacts"]["documents"] = [
            entry for entry in evidence["artifacts"]["documents"]
            if entry["path"] != canonical_path
        ]
    elif defect == "orphan_copy":
        copy_path = evidence["saves"][0]["save_copy"]["destination"]
        evidence["artifacts"]["documents"] = [
            entry for entry in evidence["artifacts"]["documents"]
            if entry["path"] != copy_path
        ]
    elif defect == "source_digest":
        source = next(iter(evidence["environment"]["build_provenance"]["stage_sources"]))
        evidence["environment"]["build_provenance"]["stage_sources"][source] = "z" * 64
    elif defect == "commit_relation":
        binary = evidence["environment"]["build_provenance"]["binaries"]["FreeCAD.exe"]
        binary["commits_not_in_binary"] = ["f" * 40]
        binary["predates_head"] = False
    elif defect == "gitlink":
        evidence["environment"]["git"]["recorded_gitlink"] = "f" * 40
    else:
        readiness = evidence["pause_resume"]["readiness_while_paused"]
        if defect == "readiness_active":
            readiness["documents"][0]["active_write_count"] = 3
        elif defect == "readiness_missing_count":
            readiness["automation_pause"].pop("active_write_count")
        else:
            readiness["documents"].append(
                {"automation_paused": True, "active_write_count": 0}
            )
    completed = SimpleNamespace(returncode=0, stdout="PART3_RESULT: PASSED\n", stderr="")
    with pytest.raises(AssertionError):
        _assert_green_stage(
            completed,
            evidence,
            stage,
            len(evidence["cycles"]),
            len(evidence["saves"]),
        )


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize(
    "defect",
    ["binary_digest", "save_digest", "active_bool", "ordinary_size_bool", "backup_size_bool", "provenance_bool", "fingerprint_size_bool", "fingerprint_mtime_bool"],
)
def test_shared_green_stage_predicate_rejects_boolean_and_nonhex_fields(
    stage: str, defect: str
) -> None:
    evidence = _personal_view_stage_fixture(clean=True)
    evidence["stage"] = stage
    _record_personal_view_fixture_aggregate(evidence)
    if defect == "binary_digest":
        evidence["environment"]["binary_fingerprint"]["FreeCAD.exe"]["sha256"] = "Z" * 64
    elif defect == "save_digest":
        evidence["saves"][0]["sha256_before"] = "not-a-digest"
    elif defect == "active_bool":
        evidence["pause_resume"]["readiness_while_paused"]["automation_pause"]["active_write_count"] = True
    elif defect == "ordinary_size_bool":
        evidence["artifacts"]["documents"][0]["size"] = True
    elif defect == "backup_size_bool":
        evidence["artifacts"]["documents"].append({"path": "legacy.fcbak", "size": False})
    elif defect == "fingerprint_size_bool":
        evidence["environment"]["binary_fingerprint"]["FreeCAD.exe"]["size"] = True
    elif defect == "fingerprint_mtime_bool":
        evidence["environment"]["binary_fingerprint"]["FreeCAD.exe"]["mtime_ns"] = False
    else:
        evidence["environment"]["build_provenance"]["history_depth"] = True
    completed = SimpleNamespace(returncode=0, stdout="PART3_RESULT: PASSED\n", stderr="")
    with pytest.raises(AssertionError):
        _assert_green_stage(
            completed,
            evidence,
            stage,
            len(evidence["cycles"]),
            len(evidence["saves"]),
        )


def test_clean_observation_precedes_every_personal_view_action() -> None:
    """GRK-P3-122: active-view switching belongs inside the proved sequence."""

    from tests.gui.part3 import stress_coordinator as module

    source = inspect.getsource(module._run_view_mutation_cycle)
    clean_position = source.index("_ensure_document_clean_for_personal_view")
    observation_position = source.index('cycle["file_change_state_before"]')
    first_local_action_position = source.index("_local_action(")
    assert clean_position < observation_position < first_local_action_position


def _active_switch_personal_proof(*, secondary_clean: bool) -> dict[str, Any]:
    def snapshot(document: str, *, clean: bool, revision: int) -> dict[str, Any]:
        return {
            "observed_document": document,
            "identity_selector": {
                "document_uid": f"uid-{document}",
                "document_instance_id": 11 if document == "Primary" else 17,
                "lifecycle_epoch": 2,
                "document_name": document,
            },
            "file_change_state": {
                "pending_changes": [] if clean else ["model"],
                "has_pending_file_changes": not clean,
            },
            "semantic_revisions": _personal_revision_vector(revision),
        }

    return {
        "action": "set_active_document",
        "documents": ["Primary", "Secondary"],
        "left_document": "Primary",
        "activated_document": "Secondary",
        "before": {
            "Primary": snapshot("Primary", clean=True, revision=3),
            "Secondary": snapshot(
                "Secondary", clean=secondary_clean, revision=7
            ),
        },
        "after": {
            "Primary": snapshot("Primary", clean=True, revision=3),
            "Secondary": snapshot(
                "Secondary", clean=secondary_clean, revision=7
            ),
        },
        "clean_before": True,
        "clean_after": True,
        "semantic_revisions_unchanged": True,
        "passed": True,
    }


def _assert_dirty_secondary_active_switch_is_rejected() -> None:
    from tests.gui.part3 import stress_coordinator as module

    proof = _active_switch_personal_proof(secondary_clean=False)
    assert not module._personal_action_proof_is_exact(proof)


def test_active_switch_complete_clean_two_document_proof_passes() -> None:
    """REVIEW-P3-WP26-007: exact Primary+Secondary clean proof is accepted."""

    from tests.gui.part3 import stress_coordinator as module

    proof = _active_switch_personal_proof(secondary_clean=True)
    assert module._personal_action_proof_is_exact(proof)


def test_active_switch_proof_fails_when_only_secondary_is_dirty() -> None:
    """REVIEW-P3-WP26-007: rejection depends only on Secondary's dirty state."""

    evidence = _personal_view_stage_fixture(clean=True)
    evidence["personal_action_proofs"] = [
        _active_switch_personal_proof(secondary_clean=False)
    ]

    _record_personal_view_fixture_aggregate(evidence)
    checks = {entry["name"]: entry for entry in evidence["checks"]}
    exact = checks["every_personal_action_has_exact_clean_revision_proof"]
    assert exact["passed"] is False, exact
    completed = SimpleNamespace(returncode=0, stdout="PART3_RESULT: PASSED\n", stderr="")
    with pytest.raises(AssertionError):
        _assert_green_stage(
            completed,
            evidence,
            "A",
            len(evidence["cycles"]),
            len(evidence["saves"]),
        )


def test_active_switch_dirty_secondary_bites_per_document_clean_guard(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """REVIEW-P3-WP26-007: neutralising the nested clean guard breaks the negative."""

    from tests.gui.part3 import stress_coordinator as module

    _assert_dirty_secondary_active_switch_is_rejected()
    monkeypatch.setattr(module, "_file_change_state_is_clean", lambda _state: True)
    with pytest.raises(AssertionError):
        _assert_dirty_secondary_active_switch_is_rejected()


def test_complete_fixture_aggregate_replaces_stale_pass_after_coverage_mutation() -> None:
    """A complete fixture cannot retain aggregate credit after mutation."""

    evidence = _personal_view_stage_fixture(clean=True)
    evidence["coverage"]["observed"] = []
    _record_personal_view_fixture_aggregate(evidence)
    checks = {entry["name"]: entry for entry in evidence["checks"]}
    coverage = checks["adr_section_13_coverage_complete"]
    assert coverage["passed"] is False
    assert coverage in evidence["failed_checks"]


def test_missing_personal_action_proofs_fail_closed() -> None:
    """REVIEW-P3-WP25-001: a stage cannot substitute cycle-level aggregates."""

    evidence = _personal_view_stage_fixture(clean=True)
    evidence.pop("personal_action_proofs")
    _record_personal_view_fixture_aggregate(evidence)
    checks = {entry["name"]: entry for entry in evidence["checks"]}
    assert checks["every_personal_action_has_exact_clean_revision_proof"]["passed"] is False


class _OfflineDocument:
    def __init__(self, name: str, *, revision: int) -> None:
        self.Name = name
        self.Uid = SimpleNamespace(Value=f"uid-{name}")
        self.revision = revision
        self.PropertiesList = ["AlphaValue"]
        self.AlphaValue = 1

    def isTouched(self) -> bool:
        return False

    def hasPendingFileChanges(self) -> bool:
        return False

    def getFileChangeState(self) -> dict[str, Any]:
        return {
            "pending_changes": [],
            "has_pending_file_changes": False,
            "document": self.Name,
        }

    def collaborationIdentity(self) -> dict[str, int]:
        return {"instance_id": self.revision + 10, "lifecycle_epoch": 2}

    def getObject(self, name: str) -> Any:
        return self if name == "StressBox" else None


class _OfflineGuiDocument:
    def __init__(self, document: _OfflineDocument) -> None:
        self.Document = document
        self.ActiveView = None

    def toggleTreeItem(self, _obj: Any, _mod: int) -> None:
        raise AssertionError("stage-prepared edit repeated toggleTreeItem")


def _install_offline_freecad_modules(
    monkeypatch: pytest.MonkeyPatch,
) -> tuple[_OfflineDocument, _OfflineDocument, Any]:
    primary = _OfflineDocument("Primary", revision=3)
    secondary = _OfflineDocument("Secondary", revision=7)
    documents = {document.Name: document for document in (primary, secondary)}
    gui_documents = {
        name: _OfflineGuiDocument(document) for name, document in documents.items()
    }

    def get_document(name: str) -> _OfflineDocument:
        if name not in documents:
            raise ValueError(f"unknown document: {name}")
        return documents[name]

    selection = SimpleNamespace(
        getSelectionEx=lambda _name: [],
        clearSelection=lambda: None,
        addSelection=lambda _document, _object: None,
    )
    freecad = SimpleNamespace(getDocument=get_document)
    freecad_gui = SimpleNamespace(
        ActiveDocument=gui_documents["Primary"],
        Selection=selection,
        getDocument=lambda name: gui_documents.get(name),
        setActiveDocument=lambda _name: None,
        updateGui=lambda: None,
    )
    monkeypatch.setitem(sys.modules, "FreeCAD", freecad)
    monkeypatch.setitem(sys.modules, "FreeCADGui", freecad_gui)
    return primary, secondary, freecad_gui


def test_view_state_selector_observes_inactive_document_without_switching_active(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """REVIEW-P3-WP26-001 RED: read Secondary while Primary stays GUI-active."""

    from tests.gui.part3.local_driver import actions

    _primary, secondary, freecad_gui = _install_offline_freecad_modules(monkeypatch)
    switches: list[str] = []
    freecad_gui.setActiveDocument = switches.append

    state = actions._view_state({"document": "Secondary"})

    assert switches == []
    assert state["active_document"] == "Primary"
    assert state["observed_document"] == "Secondary"
    assert state["file_change_state"] == secondary.getFileChangeState()
    assert state["identity_selector"]["document_name"] == "Secondary"


def test_view_state_without_selector_preserves_active_observation(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Selectorless callers retain the original active-document observation."""

    from tests.gui.part3.local_driver import actions

    primary, _secondary, _freecad_gui = _install_offline_freecad_modules(monkeypatch)
    state = actions._view_state({})
    assert state["active_document"] == "Primary"
    assert state.get("observed_document", "Primary") == "Primary"
    assert state["file_change_state"] == primary.getFileChangeState()
    assert state["identity_selector"]["document_name"] == "Primary"


def test_view_state_selector_rejects_unknown_document_without_fallback(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """An unknown selector must fail hard instead of silently reading Primary."""

    from tests.gui.part3.local_driver import actions

    _install_offline_freecad_modules(monkeypatch)
    with pytest.raises((ValueError, RuntimeError)):
        actions._view_state({"document": "Missing"})


def test_inactive_personal_snapshot_binds_exact_document_and_revision_input(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """REVIEW-P3-WP26-001 RED: snapshot and RPC selector both bind Secondary."""

    from tests.gui.part3 import stress_coordinator as module

    local_calls: list[tuple[str, dict[str, Any]]] = []
    revision_inputs: list[dict[str, Any]] = []

    class Local:
        def invoke(
            self, action: str, params: dict[str, Any] | None = None, **_kwargs: Any
        ) -> dict[str, Any]:
            supplied = params or {}
            local_calls.append((action, supplied))
            observed = str(supplied.get("document") or "Primary")
            return {
                "result": {
                    "active_document": "Primary",
                    "observed_document": observed,
                    "file_change_state": {
                        "pending_changes": [],
                        "has_pending_file_changes": False,
                    },
                    "identity_selector": {
                        "document_uid": f"uid-{observed}",
                        "document_instance_id": 17,
                        "lifecycle_epoch": 2,
                        "document_name": observed,
                    },
                }
            }

    class Rpc:
        def call(self, method: str, params: dict[str, Any], **_kwargs: Any) -> Any:
            assert method == "get_semantic_revisions"
            revision_inputs.append(params)
            return {"revisions": _personal_revision_vector(7)}

    evidence_path = tmp_path / "evidence.json"
    write_evidence(evidence_path, empty_evidence(stage="A"))
    context = SimpleNamespace(
        local=Local(),
        rpc=Rpc(),
        payload=empty_evidence(stage="A"),
        evidence_path=evidence_path,
    )
    snapshot = module._personal_document_snapshot(context, "Secondary")

    assert local_calls == [("view_state", {"document": "Secondary"})]
    assert snapshot["observed_document"] == "Secondary"
    assert snapshot["identity_selector"]["document_name"] == "Secondary"
    assert revision_inputs[0]["doc_selector"]["document_name"] == "Secondary"


def _exact_personal_proof() -> dict[str, Any]:
    selector = {
        "document_uid": "uid-Secondary",
        "document_instance_id": 17,
        "lifecycle_epoch": 2,
        "document_name": "Secondary",
    }
    def snapshot() -> dict[str, Any]:
        return {
            "observed_document": "Secondary",
            "identity_selector": dict(selector),
            "file_change_state": {
                "pending_changes": [],
                "has_pending_file_changes": False,
            },
            "semantic_revisions": _personal_revision_vector(7),
        }

    return {
        "action": "rotate_camera",
        "documents": ["Secondary"],
        "left_document": None,
        "activated_document": None,
        "before": {"Secondary": snapshot()},
        "after": {"Secondary": snapshot()},
        "clean_before": True,
        "clean_after": True,
        "semantic_revisions_unchanged": True,
        "passed": True,
    }


def test_personal_proof_rejects_observed_document_or_identity_mismatch() -> None:
    """Proof attribution rejects document and stable-identity substitution."""

    from tests.gui.part3 import stress_coordinator as module

    valid = _exact_personal_proof()
    assert module._personal_action_proof_is_exact(valid)

    observed_mismatch = _exact_personal_proof()
    observed_mismatch["after"]["Secondary"]["observed_document"] = "Primary"
    assert not module._personal_action_proof_is_exact(observed_mismatch)

    selector_mismatch = _exact_personal_proof()
    selector_mismatch["after"]["Secondary"]["identity_selector"] = {
        **selector_mismatch["after"]["Secondary"]["identity_selector"],
        "document_instance_id": 99,
    }
    assert not module._personal_action_proof_is_exact(selector_mismatch)

    missing_lifecycle = _exact_personal_proof()
    missing_lifecycle["before"]["Secondary"]["identity_selector"].pop(
        "lifecycle_epoch"
    )
    missing_lifecycle["after"]["Secondary"]["identity_selector"].pop(
        "lifecycle_epoch"
    )
    assert not module._personal_action_proof_is_exact(missing_lifecycle)


def test_stage_local_property_edit_does_not_repeat_unproved_personal_actions(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """REVIEW-P3-WP26-002 RED: prepared mode performs one model edit only."""

    from tests.gui.part3 import stress_coordinator as coordinator
    from tests.gui.part3.local_driver import actions

    _primary, secondary, freecad_gui = _install_offline_freecad_modules(monkeypatch)
    freecad_gui.ActiveDocument = freecad_gui.getDocument("Secondary")
    repeated: list[str] = []
    freecad_gui.setActiveDocument = lambda _name: repeated.append("setActiveDocument")
    freecad_gui.Selection.clearSelection = lambda: repeated.append("clearSelection")
    freecad_gui.Selection.addSelection = lambda *_args: repeated.append("addSelection")
    binding_checks: list[tuple[str, str]] = []
    edits: list[int] = []
    monkeypatch.setattr(actions, "_expected_property_keys", lambda _obj: frozenset())
    monkeypatch.setattr(actions, "_property_editor_data", lambda: object())
    monkeypatch.setattr(actions, "_wait_for_property_editor", lambda *_a, **_k: None)
    monkeypatch.setattr(actions, "_find_property_value_index", lambda *_a: object())
    monkeypatch.setattr(
        actions,
        "_require_selection_is",
        lambda document, obj: binding_checks.append((document, obj)),
    )

    def apply_value(_editor: Any, _index: Any, value: int) -> None:
        edits.append(value)
        secondary.AlphaValue = value

    monkeypatch.setattr(actions, "_apply_property_editor_value", apply_value)
    result = actions._local_property_edit(
        {
            "document": "Secondary",
            "object": "StressBox",
            "property": "AlphaValue",
            "value": 42,
            "stage_prepared": True,
        }
    )

    assert repeated == []
    assert binding_checks == [("Secondary", "StressBox")]
    assert edits == [42]
    assert result["value"] == 42

    preparation_source = inspect.getsource(coordinator._prepare_local_property_edit)
    preparation_positions = [
        preparation_source.index(f'"{action}"')
        for action in (
            "reset_property_editor",
            "clear_selection",
            "set_active_document",
            "select_object",
            "expand_tree",
        )
    ]
    assert preparation_positions == sorted(preparation_positions)
    edit_source = inspect.getsource(coordinator._local_property_edit)
    assert edit_source.index("_prepare_local_property_edit") < edit_source.index(
        '"stage_prepared": True'
    )


def test_direct_local_property_edit_preserves_personal_preparation_behavior(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The default focused-caller mode still performs its own preparation."""

    from tests.gui.part3.local_driver import actions

    _primary, secondary, freecad_gui = _install_offline_freecad_modules(monkeypatch)
    calls: list[str] = []
    freecad_gui.setActiveDocument = lambda _name: calls.append("setActiveDocument")
    freecad_gui.Selection.clearSelection = lambda: calls.append("clearSelection")
    freecad_gui.Selection.addSelection = lambda *_args: calls.append("addSelection")
    freecad_gui.getDocument("Secondary").toggleTreeItem = (
        lambda _obj, _mod: calls.append("toggleTreeItem")
    )
    monkeypatch.setattr(actions, "_expected_property_keys", lambda _obj: frozenset())
    monkeypatch.setattr(actions, "_property_editor_data", lambda: object())
    monkeypatch.setattr(actions, "_wait_for_property_editor", lambda *_a, **_k: None)
    monkeypatch.setattr(actions, "_find_property_value_index", lambda *_a: object())
    monkeypatch.setattr(actions, "_require_selection_is", lambda *_a: None)
    edits: list[int] = []

    def apply_value(_editor: Any, _index: Any, value: int) -> None:
        edits.append(value)
        secondary.AlphaValue = value

    monkeypatch.setattr(actions, "_apply_property_editor_value", apply_value)
    actions._local_property_edit(
        {
            "document": "Secondary",
            "object": "StressBox",
            "property": "AlphaValue",
            "value": 21,
        }
    )

    assert calls == [
        "setActiveDocument",
        "clearSelection",
        "addSelection",
        "toggleTreeItem",
    ]
    assert edits == [21]


def test_stage_prepared_property_edit_requires_exact_active_document(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """Prepared mode fails before editing when the requested document is inactive."""

    from tests.gui.part3.local_driver import actions

    _install_offline_freecad_modules(monkeypatch)
    with pytest.raises(RuntimeError, match="active-document mismatch"):
        actions._local_property_edit(
            {
                "document": "Secondary",
                "object": "StressBox",
                "property": "AlphaValue",
                "value": 42,
                "stage_prepared": True,
            }
        )


def test_stage_a_runs_ten_view_cycles_and_five_save_cycles() -> None:
    """`--stage a` must really execute ADR §13 Stage A and end green and graceful."""

    _require_stage_preconditions()
    completed, evidence = _run_stage_cli("a")
    _assert_green_stage(
        completed,
        evidence,
        "A",
        STAGE_A.view_mutation_cycles,
        STAGE_A.save_cycles,
    )


def test_stage_b_runs_fifty_view_cycles_and_twenty_save_cycles() -> None:
    """`--stage b` must really execute ADR §13 Stage B and end green and graceful."""

    _require_stage_preconditions()
    completed, evidence = _run_stage_cli("b")
    _assert_green_stage(
        completed,
        evidence,
        "B",
        STAGE_B.view_mutation_cycles,
        STAGE_B.save_cycles,
    )


def test_stage_counts_are_the_adr_section_13_counts() -> None:
    assert (STAGE_A.view_mutation_cycles, STAGE_A.save_cycles) == (10, 5)
    assert (STAGE_B.view_mutation_cycles, STAGE_B.save_cycles) == (50, 20)
    assert (STAGE_C.view_mutation_cycles, STAGE_C.save_cycles) == (500, 100)


def test_stage_c_is_defined_for_wp11_but_never_executable_here() -> None:
    assert resolve_stage("c") is STAGE_C
    assert EXECUTABLE_STAGES == frozenset({"A", "B"})
    with pytest.raises(ValueError, match="not executable in P3-WP10"):
        resolve_executable_stage("c")
    with pytest.raises(ValueError, match="not executable in P3-WP10"):
        run_stage("c")
    with pytest.raises(ValueError, match="not executable in P3-WP10"):
        run_stage(STAGE_C)


def test_stage_c_cli_refuses_without_printing_a_stage_result(
    capsys: pytest.CaptureFixture[str],
) -> None:
    exit_code = coordinator_main(["--stage", "c"])
    captured = capsys.readouterr()
    assert exit_code == 2
    assert "PART3_RESULT" not in captured.out
    assert "PART3_RESULT" not in captured.err
    assert "not executable in P3-WP10" in captured.err


def test_stage_flag_with_preflight_only_is_refused_without_a_stage_result(
    capsys: pytest.CaptureFixture[str],
) -> None:
    """GRK-P3-077: --stage plus --preflight-only is a refusal, never a result.

    A preflight-only run executes zero cycles. Routing --stage a|b to it printed
    PART3_RESULT: PASSED and exited 0 from a run that ran no stage at all. Every
    stage now gets the refusal Stage C already got: exit 2, no PART3_RESULT line
    on either stream.
    """

    for stage in ("a", "b", "c"):
        exit_code = coordinator_main(["--stage", stage, "--preflight-only"])
        captured = capsys.readouterr()
        assert exit_code == 2, stage
        assert "PART3_RESULT" not in captured.out, (stage, captured.out)
        assert "PART3_RESULT" not in captured.err, (stage, captured.err)
        assert captured.err.strip(), stage


def test_preflight_only_envelope_is_marked_so_it_is_never_a_stage_result(
    tmp_path: Path,
) -> None:
    """GRK-P3-077: the preflight artifact says what produced it."""

    from tests.gui.part3 import stress_coordinator as module

    payload = empty_evidence(stage=None)
    assert payload["mode"] is None
    evidence_path = tmp_path / "evidence.json"
    write_evidence(evidence_path, payload)

    module._stamp_evidence_mode(evidence_path, "preflight_only")
    stamped = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert stamped["mode"] == "preflight_only"
    assert stamped["stage"] is None

    source = COORDINATOR.read_text(encoding="utf-8")
    preflight_body = source.split("def _run_preflight_only")[1].split("\ndef ")[0]
    assert '_stamp_evidence_mode(coordinator.evidence_path, "preflight_only")' in (
        preflight_body
    )


def _persist_a_completed_shutdown(coordinator: Any) -> dict[str, Any]:
    """Make a stub teardown persist the block a COMPLETED shutdown writes.

    Both stub ``shutdown_launcher`` implementations in this file returned
    ``success: True`` while writing NOTHING to the artifact, so the persisted
    block kept ``empty_shutdown_record``'s null timestamps - a stub modelling
    a shutdown that stamped no phase transition at all. That was invisible
    while the acceptance gate was a denylist over failure markers. Now that
    the gate requires the transitions to be stamped (GRK-P3-112), the stub has
    to model the thing it stands in for.

    This is a harness improvement, not a weakening. It makes the stub MORE
    like ``graceful_shutdown_owned_session``, and every test that needs an
    aborted or incomplete shape still overrides this block with one captured
    from a real run of that helper.
    """

    payload = json.loads(coordinator.evidence_path.read_text(encoding="utf-8"))
    shutdown = payload.get("shutdown") or empty_shutdown_record()
    for key in SHUTDOWN_TIMESTAMP_KEYS:
        stamp_shutdown_transition(shutdown, key)
    shutdown["forced"] = False
    shutdown["stalled_stage"] = None
    payload["shutdown"] = shutdown
    write_evidence(coordinator.evidence_path, payload)
    return shutdown


def test_preflight_only_verdict_is_derived_from_its_own_checks(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """GRK-P3-082: --preflight-only may not print PASSED over a failed check.

    Before this fix ``record_preflight_check`` read a ``ready`` key the local
    driver never returns, so its only check was permanently ``passed: false``,
    and it was hand-appended to ``checks`` instead of going through
    ``record_check``, so the failure never reached ``failed_checks`` and
    ``_run_preflight_only`` never looked at it: PART3_RESULT: PASSED, exit 0,
    over an envelope contradicting itself.

    The two ``_run_preflight_only`` cases below are driven by stub actors: they
    prove the control flow of this one function and nothing whatsoever about a
    real FreeCAD preflight. No GUI is launched, and both verdict lines are
    captured by capsys rather than printed.
    """

    from tests.gui.part3 import stress_coordinator as module

    # Bound once: the stubs below replace module.StressCoordinator, and a stub
    # must never end up subclassing another stub.
    real_coordinator = module.StressCoordinator

    def make_coordinator(name: str, preflight: dict[str, Any]) -> Any:
        """Real recorder and real verdict logic; stubbed actors around them."""

        recorded: dict[str, Any] = {}

        class _StubPreflightCoordinator(real_coordinator):  # type: ignore[misc, valid-type]
            def __init__(self) -> None:
                real_coordinator.__init__(
                    self, run_root=tmp_path / name, repo_root=REPO_ROOT
                )

            def provision(self) -> str:
                self.evidence_dir.mkdir(parents=True, exist_ok=True)
                write_evidence(self.evidence_path, empty_evidence(stage=None))
                return "stub-control-token"

            def launch_freecad(self, freecad_exe: Path | None = None) -> Any:
                return None

            def wait_for_launcher_ready(self, timeout_s: float = 0.0) -> None:
                return None

            def spawn_remote_agent_child(self, **kwargs: Any) -> Any:
                return SimpleNamespace(returncode=0, stdout="{}", stderr="")

            def connect_local_driver(self) -> Any:
                return SimpleNamespace(endpoint={}, local_driver=None)

            def run_preflight(self, handoff: Any) -> dict[str, Any]:
                return dict(preflight)

            def shutdown_launcher(self, *, success_verdict: str = "PASSED") -> dict[str, Any]:
                recorded["success_verdict"] = success_verdict
                shutdown = _persist_a_completed_shutdown(self)
                return {
                    "success": True,
                    "forced": False,
                    "stalled_stage": None,
                    "shutdown": shutdown,
                }

        return _StubPreflightCoordinator, recorded

    # --- the recorder itself -------------------------------------------------
    # The exact dict local_driver/actions.py::_preflight returns for a healthy
    # session; nothing named "ready" is in it.
    healthy_cls, _ = make_coordinator(
        "healthy-recorder", {"pause_checkbox_visible": True, "pause_checkbox_wired": True}
    )
    healthy = healthy_cls()
    healthy.provision()
    healthy.record_preflight_check({"pause_checkbox_visible": True, "pause_checkbox_wired": True})
    payload = json.loads(healthy.evidence_path.read_text(encoding="utf-8"))
    assert [check["name"] for check in payload["checks"]] == ["local_driver_preflight"]
    assert payload["checks"][0]["passed"] is True
    assert payload["checks"][0]["detail"] == {
        "pause_checkbox_visible": True,
        "pause_checkbox_wired": True,
    }
    assert payload["failed_checks"] == []
    assert verdict_from_checks(payload) == "PASSED"

    broken_cls, _ = make_coordinator(
        "broken-recorder", {"pause_checkbox_visible": False, "pause_checkbox_wired": False}
    )
    broken = broken_cls()
    broken.provision()
    broken.record_preflight_check({"pause_checkbox_visible": False, "pause_checkbox_wired": False})
    payload = json.loads(broken.evidence_path.read_text(encoding="utf-8"))
    assert payload["checks"][0]["passed"] is False
    assert [check["name"] for check in payload["failed_checks"]] == ["local_driver_preflight"]
    assert verdict_from_checks(payload) == "FAILED"

    # --- the command that reports it ----------------------------------------
    monkeypatch.setattr(
        module, "default_freecad_exe", lambda *_args, **_kwargs: tmp_path / "FreeCAD.exe"
    )

    failing_cls, failing_shutdown = make_coordinator(
        "failing-run", {"pause_checkbox_visible": False, "pause_checkbox_wired": False}
    )
    monkeypatch.setattr(module, "StressCoordinator", failing_cls)
    exit_code = module._run_preflight_only()
    captured = capsys.readouterr()
    assert exit_code != 0
    assert "PART3_RESULT: FAILED" in captured.out
    assert "PART3_RESULT: PASSED" not in captured.out
    assert failing_shutdown["success_verdict"] == "FAILED"
    failed_payload = json.loads(
        (tmp_path / "failing-run" / "evidence" / "evidence.json").read_text(encoding="utf-8")
    )
    assert failed_payload["verdict"] != "PASSED"
    assert failed_payload["mode"] == "preflight_only"
    assert failed_payload["failed_checks"]

    passing_cls, passing_shutdown = make_coordinator(
        "passing-run", {"pause_checkbox_visible": True, "pause_checkbox_wired": True}
    )
    monkeypatch.setattr(module, "StressCoordinator", passing_cls)
    exit_code = module._run_preflight_only()
    captured = capsys.readouterr()
    assert exit_code == 0
    assert "PART3_RESULT: PASSED" in captured.out
    assert passing_shutdown["success_verdict"] == "PASSED"

    # --- the source, so neither defect can quietly come back -----------------
    source = COORDINATOR.read_text(encoding="utf-8")
    recorder_body = source.split("def record_preflight_check")[1].split("\n    def ")[0]
    assert 'preflight.get("ready")' not in recorder_body
    assert "record_check(" in recorder_body
    assert "pause_checkbox_wired" in recorder_body
    assert "pause_checkbox_visible" in recorder_body

    preflight_body = source.split("def _run_preflight_only")[1].split("\ndef ")[0]
    assert "verdict_from_checks(" in preflight_body
    assert "shutdown_launcher(" in preflight_body
    assert "success_verdict=" in preflight_body


def test_freshly_provisioned_evidence_claims_nothing_it_has_not_verified(
    tmp_path: Path,
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """GRK-P3-078: provision() must not write isolation_verified true."""

    from tests.gui.part3 import stress_coordinator as module

    monkeypatch.setattr(module, "_port_is_open", lambda _host, _port: False)
    coordinator = module.StressCoordinator(run_root=tmp_path / "run", repo_root=REPO_ROOT)
    coordinator.provision()
    evidence = json.loads(coordinator.evidence_path.read_text(encoding="utf-8"))
    assert evidence["environment"]["isolation_verified"] is False
    assert evidence["environment"]["auth"]["v2_session"] is False
    assert evidence["stage"] is None
    assert evidence["mode"] is None


# ---------------------------------------------------------------------------
# Stubbed-actor harness.
#
# It drives the real run_stage control flow with stub actors so the ordering
# invariants can be proved without a GUI. It is deliberately incapable of
# producing a green line: the stubbed cycle program always records a failed
# check named stub_harness_is_not_a_stage_run, so nothing here can ever be
# mistaken for a Stage A/B result.
# ---------------------------------------------------------------------------

STUB_MARKER_CHECK = "stub_harness_is_not_a_stage_run"


class _StubLocalDriver:
    def invoke(self, action: str, params: Any = None, **kwargs: Any) -> dict[str, Any]:
        return {"result": {}}

    def preflight(self) -> dict[str, Any]:
        return {"pause_checkbox_visible": True, "pause_checkbox_wired": True}


class _StubRpc:
    session_token = "stub-session-token"
    mcp_instance_id = "stub-mcp-instance"

    def call(self, method: str, params: Any = None, timeout: float = 30.0) -> Any:
        return {}


class _StubProcess:
    pid = -1

    def poll(self) -> int:
        return 0


@dataclass
class _StubStageRun:
    exit_code: int
    evidence: dict[str, Any]
    coordinator: Any
    held_session_during_program: list[bool]


def _run_stubbed_stage(
    monkeypatch: pytest.MonkeyPatch,
    workdir: Path,
    *,
    on_shutdown: Any,
    program: Any = None,
    binary_mtime: float | None = None,
) -> _StubStageRun:
    """Drive ``run_stage`` with stub actors.

    ``program`` runs after the stub marker is stamped, so a caller can make
    the cycle program record cycles and then abort exactly the way a real
    stage aborts on a failed check. ``binary_mtime`` ages the stub binary, so
    a caller can observe what the provenance record says about a build that
    predates every commit in the recorded history.
    """

    from tests.gui.part3 import stress_coordinator as module

    instances: list[Any] = []
    held: list[bool] = []

    class _StubCoordinator(module.StressCoordinator):
        def __init__(self, **kwargs: Any) -> None:
            super().__init__(**kwargs)
            instances.append(self)

        def launch_freecad(self, freecad_exe: Path | None = None) -> Any:
            self._launcher_process = _StubProcess()
            return self._launcher_process

        def wait_for_launcher_ready(self, timeout_s: float = 120.0) -> None:
            return None

        def spawn_remote_agent_child(self, **kwargs: Any) -> Any:
            return SimpleNamespace(
                returncode=0, stdout='{"absent": true}', stderr=""
            )

        def connect_local_driver(self) -> Any:
            endpoint = {"host": "127.0.0.1", "port": 0, "path": "/action", "ready": True}
            handoff = module.CoordinatorHandoff(
                control_token=self._control_token or "",
                local_driver=_StubLocalDriver(),
                endpoint=endpoint,
            )
            self._handoff = handoff
            self._control_token = None
            return handoff

        def authenticate_typed_session(self, *, authenticator: Any = None) -> Any:
            return super().authenticate_typed_session(
                authenticator=lambda *_args, **_kwargs: _StubRpc()
            )

        def shutdown_launcher(self, *, success_verdict: str = "PASSED") -> dict[str, Any]:
            self._launcher_process = None
            self.release_typed_session()
            # Persist first, so ``on_shutdown`` can still override the block
            # with one captured from a real ordered shutdown.
            shutdown = _persist_a_completed_shutdown(self)
            on_shutdown(self)
            return {
                "success": True,
                "forced": False,
                "stalled_stage": None,
                "shutdown": shutdown,
            }

    def _stub_program(context: Any) -> None:
        held.append(instances[-1].holds_rpc_session())
        record_check(
            context.payload,
            STUB_MARKER_CHECK,
            False,
            "stubbed actors: this harness proves ordering, never a stage result",
        )
        if program is not None:
            program(context)

    monkeypatch.setattr(module, "StressCoordinator", _StubCoordinator)
    monkeypatch.setattr(module, "_port_is_open", lambda _host, _port: False)
    monkeypatch.setattr(module, "_verify_isolation", lambda _context, _coordinator: None)
    monkeypatch.setattr(module, "_run_cycle_program", _stub_program)
    monkeypatch.setattr(module, "_record_stage_aggregates", lambda _context: None)

    exe = workdir / "bin" / "FreeCAD.exe"
    exe.parent.mkdir(parents=True, exist_ok=True)
    exe.write_bytes(b"stub-binary")
    if binary_mtime is not None:
        os.utime(exe, (binary_mtime, binary_mtime))
    exit_code = module.run_stage(
        STAGE_A,
        repo_root=REPO_ROOT,
        run_root=workdir / "run",
        freecad_exe=exe,
    )
    coordinator = instances[-1]
    evidence = json.loads(coordinator.evidence_path.read_text(encoding="utf-8"))
    return _StubStageRun(exit_code, evidence, coordinator, held)


def test_stage_path_reports_its_real_actor_boundary(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """GRK-P3-076: the harness reports the boundary it has, not the ADR ideal.

    The stage path authenticates the typed session in the coordinator process.
    ``holds_rpc_session()`` must say so while the cycle program runs, the
    evidence must name the actor mode, and the session must be released at
    shutdown.
    """

    run = _run_stubbed_stage(monkeypatch, tmp_path, on_shutdown=lambda _c: None)

    assert run.held_session_during_program == [True], run.held_session_during_program
    actor = run.evidence["environment"]["remote_actor"]
    assert actor["mode"] == "in_process_typed_session", actor
    assert actor["child_token_absence_proved"] is True, actor
    assert actor["adr_deviation"] == "section 1.1", actor
    assert actor["holds_rpc_session_in_coordinator_process"] is True, actor
    assert run.coordinator.holds_rpc_session() is False
    assert run.coordinator.holds_control_token() is False

    # The harness marks itself failed, so it can never be read as a green
    # stage - and the stub program runs zero cycles, which the abort-path
    # count checks now state in the artifact instead of leaving it to a
    # reader who counts the arrays by hand (GRK-P3-103).
    assert [entry["name"] for entry in run.evidence["failed_checks"]] == [
        STUB_MARKER_CHECK,
        "cycle_count_matches_stage_definition",
        "save_count_matches_stage_definition",
    ]
    assert run.evidence["verdict"] == "FAILED"
    assert run.exit_code == 1


def test_artifact_scan_is_taken_after_shutdown(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """GRK-P3-079: teardown leftovers must be able to fail the stage.

    Two identical stubbed stages; only the teardown differs. The one whose
    shutdown drops a stray file into the documents directory must report it in
    artifacts.unexplained and fail the artifact check - which is only possible
    if the deciding scan runs after shutdown returns.
    """

    clean = _run_stubbed_stage(monkeypatch, tmp_path / "clean", on_shutdown=lambda _c: None)
    assert clean.evidence["artifacts"]["unexplained"] == [], clean.evidence["artifacts"]
    assert [entry["name"] for entry in clean.evidence["failed_checks"]] == [
        STUB_MARKER_CHECK,
        "cycle_count_matches_stage_definition",
        "save_count_matches_stage_definition",
    ]

    def _leave_stray(coordinator: Any) -> None:
        stray = coordinator.run_root / "documents" / "left-by-teardown.tmp"
        stray.write_bytes(b"leftover")

    dirty = _run_stubbed_stage(monkeypatch, tmp_path / "dirty", on_shutdown=_leave_stray)
    unexplained = [entry["path"] for entry in dirty.evidence["artifacts"]["unexplained"]]
    assert any(path.endswith("left-by-teardown.tmp") for path in unexplained), unexplained
    failed = [entry["name"] for entry in dirty.evidence["failed_checks"]]
    assert "artifact_scan_has_no_unexplained_files" in failed, failed
    assert dirty.evidence["verdict"] == "FAILED"
    assert dirty.exit_code == 1

    # The mid-stage snapshot could not see it: that is exactly why the deciding
    # scan has to be the post-shutdown one.
    assert dirty.evidence["artifacts_in_stage"]["unexplained"] == []


def test_forced_shutdown_can_never_produce_a_passed_stage() -> None:
    payload = empty_evidence(stage="A")
    assert stage_verdict(payload, stage_ok=True, shutdown_ok=True) == "PASSED"
    assert stage_verdict(payload, stage_ok=True, shutdown_ok=False) == "FAILED"
    assert stage_verdict(payload, stage_ok=False, shutdown_ok=True) == "FAILED"

    failed = empty_evidence(stage="A")
    record_check(failed, "some_stage_check", False, "detail")
    assert stage_verdict(failed, stage_ok=True, shutdown_ok=True) == "FAILED"

    leftover = empty_evidence(stage="A")
    leftover["artifacts"]["unexplained"] = [{"path": "stray.tmp", "size": 1}]
    assert stage_verdict(leftover, stage_ok=True, shutdown_ok=True) == "FAILED"


def test_lock_anchors_are_their_own_artifact_class(tmp_path: Path) -> None:
    document = tmp_path / "Doc.FCStd"
    document.write_bytes(b"not-a-zip")
    (tmp_path / "Doc.FCStd.FreeCAD-save.lock").write_bytes(b"")
    (tmp_path / "Doc.FCStd1").write_bytes(b"backup")
    (tmp_path / "Doc.FCBak").write_bytes(b"backup")
    stray = tmp_path / "leftover.tmp"
    stray.write_bytes(b"stray")

    assert classify_artifact(document) == "documents"
    assert classify_artifact(tmp_path / "Doc.FCStd.FreeCAD-save.lock") == "lock_anchors"
    assert classify_artifact(tmp_path / "Doc.FCStd1") == "documents"
    assert classify_artifact(tmp_path / "Doc.FCBak") == "documents"
    assert classify_artifact(stray) == "unexplained"

    scan = scan_artifacts([tmp_path])
    assert [entry["path"] for entry in scan["unexplained"]] == [str(stray)]
    assert len(scan["lock_anchors"]) == 1
    assert len(scan["documents"]) == 3
    assert any(entry.get("readable_archive") is False for entry in scan["documents"])


def test_binary_and_git_fingerprints_are_real(tmp_path: Path) -> None:
    sample = tmp_path / "FreeCADApp.dll"
    sample.write_bytes(b"binary-content")
    fingerprints = binary_fingerprint([sample, tmp_path / "absent.dll"])
    assert set(fingerprints) == {"FreeCADApp.dll"}
    assert fingerprints["FreeCADApp.dll"]["sha256"] == hashlib.sha256(
        b"binary-content"
    ).hexdigest()
    assert fingerprints["FreeCADApp.dll"]["size"] == len(b"binary-content")
    assert sha256_file(sample) == fingerprints["FreeCADApp.dll"]["sha256"]
    assert sha256_file(tmp_path / "absent.dll") == ""

    state = git_state(REPO_ROOT)
    assert len(state["parent_commit"]) == 40
    assert len(state["nested_commit"]) == 40
    assert len(state["recorded_gitlink"]) == 40
    assert state["branch"]


def _coverage_literals() -> set[str]:
    """Every coverage id the stage program can record, read from its own source."""

    tree = ast.parse(COORDINATOR.read_text(encoding="utf-8"), filename=str(COORDINATOR))
    literals: set[str] = set()
    for node in ast.walk(tree):
        if isinstance(node, ast.Call):
            for keyword in node.keywords:
                if keyword.arg == "coverage" and isinstance(keyword.value, ast.Constant):
                    if isinstance(keyword.value.value, str):
                        literals.add(keyword.value.value)
            func = node.func
            if (
                isinstance(func, ast.Attribute)
                and func.attr == "add"
                and node.args
                and isinstance(node.args[0], ast.Constant)
                and isinstance(node.args[0].value, str)
            ):
                literals.add(node.args[0].value)
    return literals


def test_cycle_program_can_produce_every_adr_section_13_coverage_item() -> None:
    literals = _coverage_literals()
    missing = sorted(set(COVERAGE_ITEMS) - literals)
    assert missing == [], missing


def test_stage_path_drives_pause_only_through_the_real_checkbox() -> None:
    source = COORDINATOR.read_text(encoding="utf-8")
    assert '"pause_writes"' in source
    assert '"resume_writes"' in source
    for forbidden in (
        "automation_pause",
        "admit_remote_write",
        "request_local_pause_after_current",
        "resume_local_agent_writes",
    ):
        assert forbidden not in source, forbidden


def test_cycle_program_schedules_exactly_the_stage_counts(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """The interleaved program runs every cycle and every save cycle exactly once.

    This is the scheduling invariant only. It proves nothing about a live stage;
    the green path is proved by the two subprocess tests above.
    """

    from types import SimpleNamespace

    from tests.gui.part3 import stress_coordinator as coordinator

    segment_names = (
        "_run_same_property_conflict",
        "_run_history_segment",
        "_run_independent_property_success",
        "_run_pause_resume_segment",
    )
    for definition in (STAGE_A, STAGE_B):
        cycles: list[int] = []
        saves: list[int] = []
        segments: list[str] = []
        monkeypatch.setattr(coordinator, "_provision_stage_documents", lambda _ctx: None)
        monkeypatch.setattr(
            coordinator,
            "_run_view_mutation_cycle",
            lambda _ctx, index: cycles.append(index),
        )
        monkeypatch.setattr(
            coordinator,
            "_run_save_cycle",
            lambda _ctx, index: saves.append(index),
        )
        for name in segment_names:
            monkeypatch.setattr(
                coordinator,
                name,
                lambda _ctx, _name=name: segments.append(_name),
            )
        coordinator._run_cycle_program(SimpleNamespace(definition=definition))
        assert cycles == list(range(definition.view_mutation_cycles))
        assert saves == list(range(definition.save_cycles))
        assert sorted(segments) == sorted(segment_names)


# ---------------------------------------------------------------------------
# P3-WP13 regressions.
# ---------------------------------------------------------------------------

# The check reports that provenance was RECORDED. It does not, and is not
# named as though it does, prove that any binary was built from any commit
# (GRK-P3-101).
PROVENANCE_CHECK = "build_provenance_is_recorded_for_binaries_and_stage_sources"
OLD_PROVENANCE_CHECK = "build_provenance_ties_binaries_and_stage_sources_to_commits"


def test_running_this_file_does_not_start_freecad_without_an_opt_in(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    """GRK-P3-097: no opt-in, no session - and the guard runs before any probe.

    The binary lookup and the port probe are replaced with detonators. If the
    opt-in were checked after them - or not at all - the run would reach a
    host on which both are favourable and launch FreeCAD, which is exactly
    how a reviewer forbidden from launching FreeCAD ended up launching it.
    """

    def _must_not_be_reached(*_args: Any, **_kwargs: Any) -> Any:
        raise AssertionError("stage preconditions probed the host before the opt-in")

    module = sys.modules[__name__]
    monkeypatch.setattr(module, "default_freecad_exe", _must_not_be_reached)
    monkeypatch.setattr(module, "_port_is_open", _must_not_be_reached)

    for value in (None, "", "0", "no", "off", "maybe"):
        if value is None:
            monkeypatch.delenv(LIVE_STAGE_OPT_IN_ENV, raising=False)
        else:
            monkeypatch.setenv(LIVE_STAGE_OPT_IN_ENV, value)
        assert live_stage_opt_in() is False, value
        # pytest.skip raises a BaseException, so this must not be narrowed to
        # Exception - that lets the skip escape and silently skips this test
        # instead of proving anything.
        with pytest.raises(pytest.skip.Exception) as excinfo:
            _require_stage_preconditions()
        assert "NOT_RUN" in str(excinfo.value), value
        assert LIVE_STAGE_OPT_IN_ENV in str(excinfo.value), value

    for value in ("1", "true", "TRUE", "yes", "on"):
        monkeypatch.setenv(LIVE_STAGE_OPT_IN_ENV, value)
        assert live_stage_opt_in() is True, value


KNOWN_LIVE_TESTS = frozenset(
    {
        "test_stage_a_runs_ten_view_cycles_and_five_save_cycles",
        "test_stage_b_runs_fifty_view_cycles_and_twenty_save_cycles",
    }
)


def test_both_live_tests_ask_for_the_opt_in_before_they_run_a_stage() -> None:
    """The guard is only worth anything if EVERY live test goes through it.

    Enumerating the two live tests by name could not see a third one added
    later (GRK-P3-105), and the defect class this guards - a test in this file
    launching FreeCAD for someone who did not ask for it - has already
    happened once, to a reviewer forbidden from launching FreeCAD. Live tests
    are detected structurally now: any module-level test that calls a stage
    entry point. The two known names stay asserted as a floor, so a detector
    that silently matched nothing would still fail here.
    """

    source = Path(__file__).read_text(encoding="utf-8")
    found, unguarded = _live_stage_tests(source, __file__)
    assert KNOWN_LIVE_TESTS <= found, sorted(KNOWN_LIVE_TESTS - found)
    assert unguarded == set(), sorted(unguarded)
    # The Stage C exemption below is only sound while Stage C is structurally
    # not executable here, so this assertion is part of the detector's floor.
    assert EXECUTABLE_STAGES == frozenset({"A", "B"})
    assert "test_stage_c_is_defined_for_wp11_but_never_executable_here" not in found


def test_the_live_test_detector_rejects_an_unguarded_stage_test() -> None:
    """A detector that cannot fail is not a gate (GRK-P3-105).

    Run over synthetic source, so the rejection is observed rather than
    assumed from the fact that this file happens to be clean.
    """

    synthetic = (
        "def test_guarded_live_stage():\n"
        '    """Docstring first, guard second."""\n'
        "    _require_stage_preconditions()\n"
        '    completed, evidence = _run_stage_cli("a")\n'
        "\n"
        "def test_unguarded_live_stage():\n"
        '    completed, evidence = _run_stage_cli("a")\n'
        "\n"
        "def test_unguarded_run_stage():\n"
        "    run_stage(STAGE_A)\n"
        "\n"
        "def test_guard_is_not_first():\n"
        "    value = 1\n"
        "    _require_stage_preconditions()\n"
        '    _run_stage_cli("b")\n'
        "\n"
        "def test_stage_c_refusal_is_not_a_live_test():\n"
        "    run_stage(STAGE_C)\n"
        '    run_stage("c")\n'
        "\n"
        "def test_not_a_stage_test():\n"
        "    assert True\n"
        "\n"
        # GRK-P3-108: the attribute form is already this file's dominant
        # module-access idiom, so it was the bypass a future live test
        # would reach for by default - and the CLI entry point forwards
        # --stage a|b straight to run_stage.
        "def test_attr_run_stage():\n"
        '    coordinator.run_stage("a")\n'
        "\n"
        "def test_attr_cli():\n"
        '    module._run_stage_cli("b")\n'
        "\n"
        "def test_cli_main():\n"
        '    coordinator_main(["--stage", "a"])\n'
        "\n"
        "def test_cli_preflight():\n"
        '    coordinator_main(["--stage", stage, "--preflight-only"])\n'
        "\n"
        "def test_cli_stage_c():\n"
        '    coordinator_main(["--stage", "c"])\n'
        "\n"
        # GRK-P3-111: ``--preflight-only`` is a refusal only ALONGSIDE
        # ``--stage``. main() nests that refusal inside ``if args.stage:``;
        # with no stage the flag falls through to ``_run_preflight_only()``,
        # which provisions an isolated profile and launches a real GUI.
        "def test_cli_preflight_alone():\n"
        '    coordinator_main(["--preflight-only"])\n'
        "\n"
        "def test_cli_preflight_attr():\n"
        '    module.main(["--preflight-only"])\n'
    )
    found, unguarded = _live_stage_tests(synthetic, "synthetic.py")
    assert found == {
        "test_guarded_live_stage",
        "test_unguarded_live_stage",
        "test_unguarded_run_stage",
        "test_guard_is_not_first",
        "test_attr_run_stage",
        "test_attr_cli",
        "test_cli_main",
        "test_cli_preflight_alone",
        "test_cli_preflight_attr",
    }, sorted(found)
    assert unguarded == {
        "test_unguarded_live_stage",
        "test_unguarded_run_stage",
        "test_guard_is_not_first",
        "test_attr_run_stage",
        "test_attr_cli",
        "test_cli_main",
        "test_cli_preflight_alone",
        "test_cli_preflight_attr",
    }, sorted(unguarded)

    # The GRK-P3-111 correction must not over-fire. The two argv shapes that
    # really ARE refusal paths stay exempt, so no currently-passing offline
    # test acquires an opt-in guard it does not need.
    assert "test_cli_preflight" not in found, sorted(found)
    assert "test_cli_stage_c" not in found, sorted(found)


def _is_docstring(node: ast.stmt) -> bool:
    return (
        isinstance(node, ast.Expr)
        and isinstance(node.value, ast.Constant)
        and isinstance(node.value.value, str)
    )


# Calling any of these can provision an isolated profile and launch a real
# FreeCAD GUI, so a test that calls one is a live test.
LIVE_STAGE_ENTRYPOINTS = frozenset({"_run_stage_cli", "run_stage"})
# ``main`` forwards ``--stage a|b`` straight to ``run_stage``, so the CLI
# entry point launches FreeCAD just as surely as the function does.
LIVE_CLI_ENTRYPOINTS = frozenset({"main", "coordinator_main"})
# ...except for Stage C, which ``resolve_executable_stage`` refuses before any
# provisioning happens. Exempting it keeps the two refusal assertions in
# ``test_stage_c_is_defined_for_wp11_but_never_executable_here`` runnable
# without an opt-in; a skip there would be NOT_RUN, never a pass.
NON_EXECUTABLE_STAGE_LITERALS = frozenset({"c"})
NON_EXECUTABLE_STAGE_NAMES = frozenset({"STAGE_C"})
# ...and except for ``--preflight-only`` WHEN THE SAME ARGV ALSO PASSES A
# VALUE TO ``--stage``. That pairing is what ``main`` refuses before
# provisioning (GRK-P3-077), and the refusal is nested inside
# ``if args.stage:``. The flag ALONE is not a refusal at all:
# ``main(["--preflight-only"])`` falls through to ``_run_preflight_only()``,
# which provisions an isolated profile and launches a real FreeCAD GUI.
# Exempting the flag unconditionally did not leave this gate blind to that
# shape - it made the gate CERTIFY a live shape as safe, and wrote the false
# premise down right here, where a later reader would reasonably trust it
# (GRK-P3-111).
NON_EXECUTABLE_CLI_FLAGS = frozenset({"--preflight-only"})
CLI_STAGE_FLAG = "--stage"


def _called_entrypoint_name(node: ast.Call) -> str | None:
    """The bare function name a call targets, through the attribute form too.

    Matching only ``ast.Name`` left ``coordinator.run_stage("a")`` and
    ``module.run_stage(STAGE_A)`` invisible to this gate - and the attribute
    form is already the dominant module-access idiom in this very file, so
    the bypass was the one a future live test would reach for by default
    (GRK-P3-108). A gate its own file routinely walks around does not make
    the defect class structurally impossible, which is the point of it.
    """

    if isinstance(node.func, ast.Name):
        return node.func.id
    if isinstance(node.func, ast.Attribute):
        return node.func.attr
    return None


def _argv_string_literals(node: ast.Call) -> list[str | None] | None:
    """The first argument's elements as strings, or None if it is not a list.

    A non-literal element becomes ``None`` so it can never be mistaken for a
    stage name that happens to be exempt.
    """

    first = node.args[0] if node.args else None
    if not isinstance(first, (ast.List, ast.Tuple)):
        return None
    literals: list[str | None] = []
    for element in first.elts:
        if isinstance(element, ast.Constant) and isinstance(element.value, str):
            literals.append(element.value.strip().lower())
        else:
            literals.append(None)
    return literals


def _argv_passes_a_value_to_the_stage_flag(literals: list[str | None]) -> bool:
    """True when this argv hands ``--stage`` a value that argparse sees as set.

    Fails safe in the direction that matters: a non-literal value counts,
    because it could name Stage A or B. An empty literal does not, because
    ``args.stage`` is then falsy and ``main`` skips the refusal branch
    entirely.
    """

    for index, item in enumerate(literals[:-1]):
        if item != CLI_STAGE_FLAG:
            continue
        if literals[index + 1] != "":
            return True
    return False


def _cli_call_is_a_refusal_path(node: ast.Call) -> bool:
    """True only when this CLI argv provably cannot reach provisioning.

    Structural, like the Stage C exemption, and it fails safe the same way:
    a computed argv is treated as live because it could name any stage.
    """

    literals = _argv_string_literals(node)
    if literals is None:
        return False
    if _argv_passes_a_value_to_the_stage_flag(literals) and any(
        item in NON_EXECUTABLE_CLI_FLAGS for item in literals
    ):
        return True
    for index, item in enumerate(literals[:-1]):
        if item != CLI_STAGE_FLAG:
            continue
        if literals[index + 1] in NON_EXECUTABLE_STAGE_LITERALS:
            return True
    return False


def _call_can_start_a_stage(node: ast.Call) -> bool:
    """True when this call can provision a profile and launch FreeCAD."""

    name = _called_entrypoint_name(node)
    if name is None:
        return False
    if name in LIVE_CLI_ENTRYPOINTS:
        return not _cli_call_is_a_refusal_path(node)
    if name not in LIVE_STAGE_ENTRYPOINTS:
        return False
    first = node.args[0] if node.args else None
    if isinstance(first, ast.Constant) and isinstance(first.value, str):
        if first.value.strip().lower() in NON_EXECUTABLE_STAGE_LITERALS:
            return False
    if isinstance(first, ast.Name) and first.id in NON_EXECUTABLE_STAGE_NAMES:
        return False
    return True


def _live_stage_tests(source: str, filename: str) -> tuple[set[str], set[str]]:
    """Return (live tests found, live tests missing the opt-in guard).

    A live test is any module-level ``test_*`` function whose body contains a
    call that can start a stage. It is guarded only when
    ``_require_stage_preconditions()`` is its first non-docstring statement.
    """

    tree = ast.parse(source, filename=filename)
    found: set[str] = set()
    unguarded: set[str] = set()
    for node in tree.body:
        if not isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef)):
            continue
        if not node.name.startswith("test_"):
            continue
        if not any(
            isinstance(inner, ast.Call) and _call_can_start_a_stage(inner)
            for inner in ast.walk(node)
        ):
            continue
        found.add(node.name)
        body = [item for item in node.body if not _is_docstring(item)]
        first = body[0] if body else None
        guarded = (
            isinstance(first, ast.Expr)
            and isinstance(first.value, ast.Call)
            and isinstance(first.value.func, ast.Name)
            and first.value.func.id == "_require_stage_preconditions"
        )
        if not guarded:
            unguarded.add(node.name)
    return found, unguarded


class _InvalidRequest(Exception):
    """Stands in for the launcher JSON-RPC error the listener really raises."""


class _FramedRpc:
    """Typed client that enforces JSON-RPC 2.0 params framing, like the listener.

    JSON-RPC 2.0 allows params to be a structured value only - an array or an
    object. Anything else is answered with -32600 Invalid Request by the
    framing layer, before the method is looked up and before any dispatch.
    """

    session_token = "framed-session-token"
    mcp_instance_id = "framed-mcp-instance"

    def __init__(self, documents: list[str]) -> None:
        self.documents = list(documents)
        self.calls: list[tuple[str, Any]] = []

    def call(self, method: str, params: Any = None, timeout: float = 30.0) -> Any:
        if params is not None and not isinstance(params, (list, tuple, dict)):
            raise _InvalidRequest("JSON-RPC error -32600: Invalid Request")
        self.calls.append((method, params))
        if method == "list_documents":
            return [{"name": name} for name in self.documents]
        return {"success": True}


class _ExitsWhenRpcShutdownIsAccepted:
    """Owned process that exits once - and only once - the shutdown verb lands."""

    pid = -1

    def __init__(self, rpc: _FramedRpc) -> None:
        self._rpc = rpc

    def poll(self) -> int | None:
        accepted = any(
            method == "shutdown_rpc_server" for method, _params in self._rpc.calls
        )
        return 0 if accepted else None


def test_ordered_shutdown_closes_documents_instead_of_aborting_on_framing(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """GRK-P3-096: the teardown sent a bare string as JSON-RPC params.

    ``close_document`` was called with the document name itself as params. A
    bare string is neither an array nor an object, so the listener answered
    -32600 Invalid Request at the framing layer. That exception aborted the
    whole ordered shutdown on the first open document: every stage timestamp
    stayed null, the process never got the shutdown verb, and it was killed at
    the deadline with forced true.

    WP09's own shutdown gate could not see it. It tears down a freshly
    launched session with no documents open, so the loop body never ran. This
    test supplies the one thing the stage path has and that gate does not:
    open documents.
    """

    from tests.gui.part3 import stress_coordinator as module

    rpc = _FramedRpc(["Part3StagePrimary", "Part3StageSecondary"])
    killed: list[Any] = []
    monkeypatch.setattr(module, "authenticate_json_rpc", lambda *_a, **_k: rpc)
    monkeypatch.setattr(
        module, "_force_kill_owned_process_tree", lambda process: killed.append(process)
    )
    launcher = SimpleNamespace(
        JsonRpcClient=lambda **_kwargs: SimpleNamespace(host="127.0.0.1", port=0),
        JsonRpcError=_InvalidRequest,
        JsonRpcTransportError=_InvalidRequest,
    )
    evidence_path = tmp_path / "evidence.json"
    write_evidence(evidence_path, empty_evidence(stage="A"))

    result = module.graceful_shutdown_owned_session(
        process=_ExitsWhenRpcShutdownIsAccepted(rpc),
        profile_root=tmp_path / "profile",
        launcher_module=launcher,
        repo_root=REPO_ROOT,
        mcp_port=0,
        local_driver=_StubLocalDriver(),
        evidence_path=evidence_path,
        deadline_seconds=5,
    )

    shutdown = result["shutdown"]
    assert shutdown.get("rpc_error") is None, shutdown
    assert shutdown.get("failed_step") is None, shutdown
    for key in (
        "requested_utc",
        "documents_closed_utc",
        "rpc_admission_closed_utc",
        "worker_shutdown_utc",
        "listener_shutdown_utc",
        "window_closed_utc",
        "process_exit_utc",
    ):
        assert shutdown[key], (key, shutdown)
    assert shutdown["forced"] is False, shutdown
    assert shutdown["stalled_stage"] is None, shutdown
    assert result["success"] is True, result
    assert killed == [], killed

    # Every document was closed, and the ordered sequence really was ordered.
    closed = [params for method, params in rpc.calls if method == "close_document"]
    assert closed == [["Part3StagePrimary"], ["Part3StageSecondary"]], closed
    methods = [method for method, _params in rpc.calls]
    assert methods.index("list_documents") < methods.index("close_document")
    assert methods.index("close_document") < methods.index("shutdown_rpc_server")


def test_the_rpc_phase_records_which_step_it_actually_stopped_on(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """GRK-P3-100: a stalled phase must not name a step it never reached.

    The teardown stamped ``stalled_stage: rpc_shutdown`` for any failure in
    the RPC phase, including one raised while closing documents. Read from an
    artifact whose ``rpc_admission_closed_utc``, ``worker_shutdown_utc`` and
    ``listener_shutdown_utc`` are all null, that asserts the shutdown verb
    stalled when the verb was never sent - and it is what the first triage of
    this defect chased. Both fields now name the real step.

    The two cases below stop in different steps and must therefore read
    differently in the artifact. If they collapsed back to one label, the
    field would be useless again.
    """

    from tests.gui.part3 import stress_coordinator as module

    class _RefusesToCloseDocuments(_FramedRpc):
        def call(self, method: str, params: Any = None, timeout: float = 30.0) -> Any:
            if method == "close_document":
                raise _InvalidRequest("JSON-RPC error -32600: Invalid Request")
            return super().call(method, params, timeout)

    class _RefusesToShutDown(_FramedRpc):
        def call(self, method: str, params: Any = None, timeout: float = 30.0) -> Any:
            if method == "shutdown_rpc_server":
                raise _InvalidRequest("JSON-RPC error -32000: shutdown refused")
            return super().call(method, params, timeout)

    launcher = SimpleNamespace(
        JsonRpcClient=lambda **_kwargs: SimpleNamespace(host="127.0.0.1", port=0),
        JsonRpcError=_InvalidRequest,
        JsonRpcTransportError=_InvalidRequest,
    )

    def _drive(rpc: _FramedRpc, name: str) -> tuple[dict[str, Any], Path]:
        killed: list[Any] = []
        monkeypatch.setattr(module, "authenticate_json_rpc", lambda *_a, **_k: rpc)
        monkeypatch.setattr(
            module,
            "_force_kill_owned_process_tree",
            lambda process: killed.append(process),
        )
        evidence_path = tmp_path / f"{name}.json"
        write_evidence(evidence_path, empty_evidence(stage="A"))
        result = module.graceful_shutdown_owned_session(
            process=_ExitsWhenRpcShutdownIsAccepted(rpc),
            profile_root=tmp_path / "profile",
            launcher_module=launcher,
            repo_root=REPO_ROOT,
            mcp_port=0,
            local_driver=_StubLocalDriver(),
            evidence_path=evidence_path,
            deadline_seconds=1,
        )
        assert result["success"] is False, result
        assert killed, "a stalled shutdown past its deadline must reach the kill helper"
        return result["shutdown"], evidence_path

    # Case 1: the phase stops while closing documents. The shutdown verb was
    # never sent, so nothing here may say it stalled.
    on_close, close_evidence = _drive(
        _RefusesToCloseDocuments(["Part3StagePrimary"]), "close"
    )
    assert on_close["failed_step"] == "document_close", on_close
    assert on_close["stalled_stage"] == "document_close", on_close
    assert "-32600" in str(on_close["rpc_error"]), on_close
    assert on_close["documents_closed_utc"] is None, on_close
    assert on_close["rpc_admission_closed_utc"] is None, on_close
    assert on_close["forced"] is True, on_close

    persisted = json.loads(close_evidence.read_text(encoding="utf-8"))
    assert persisted["verdict"] == "FAILED", persisted["verdict"]
    assert persisted["shutdown"]["failed_step"] == "document_close"
    assert persisted["shutdown"]["stalled_stage"] == "document_close"

    # Case 2: the documents really did close and the shutdown verb really did
    # stall. Only now may the artifact say "rpc_shutdown".
    on_shutdown, shutdown_evidence = _drive(
        _RefusesToShutDown(["Part3StagePrimary"]), "shutdown"
    )
    assert on_shutdown["failed_step"] == "rpc_shutdown", on_shutdown
    assert on_shutdown["stalled_stage"] == "rpc_shutdown", on_shutdown
    assert "-32000" in str(on_shutdown["rpc_error"]), on_shutdown
    assert on_shutdown["documents_closed_utc"], on_shutdown
    assert on_shutdown["rpc_admission_closed_utc"] is None, on_shutdown
    assert on_shutdown["forced"] is True, on_shutdown

    persisted = json.loads(shutdown_evidence.read_text(encoding="utf-8"))
    assert persisted["verdict"] == "FAILED", persisted["verdict"]
    assert persisted["shutdown"]["stalled_stage"] == "rpc_shutdown"

    # The two steps are provably distinguishable in the artifact.
    assert on_close["stalled_stage"] != on_shutdown["stalled_stage"]


# The only name a stage RPC site may hand to ``params`` instead of a literal:
# the forwarders ``_remote_action`` and ``_call_expecting_failure`` pass their
# own ``params`` argument through, and every caller of those builds a dict.
PARAMS_FORWARDER_NAMES = frozenset({"params"})


def _unstructured_params_sites(source: str, label: str) -> list[str]:
    """Every ``*.call(...)`` site in ``source`` handing params a bare value.

    The second positional argument AND a ``params=`` keyword are both
    inspected. A gate that read only ``node.args[1]`` never saw
    ``rpc.call("close_document", params=document_name)`` - the same defect
    wearing a keyword - and let it through silently (GRK-P3-104).
    """

    tree = ast.parse(source, filename=label)
    unstructured: list[str] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Call):
            continue
        if not (isinstance(node.func, ast.Attribute) and node.func.attr == "call"):
            continue
        candidates: list[ast.expr] = []
        if len(node.args) >= 2:
            candidates.append(node.args[1])
        for keyword in node.keywords:
            if keyword.arg == "params":
                candidates.append(keyword.value)
        for params in candidates:
            if isinstance(params, (ast.List, ast.Tuple, ast.Dict, ast.Starred)):
                continue
            if isinstance(params, ast.Name) and params.id in PARAMS_FORWARDER_NAMES:
                continue
            unstructured.append(
                f"{label}:{node.lineno} "
                f"{ast.get_source_segment(source, params)!r}"
            )
    return unstructured


def test_no_stage_rpc_call_sends_an_unstructured_params_value() -> None:
    """GRK-P3-096, generalised: params must be an array or an object.

    JSON-RPC 2.0 allows params to be a structured value only. The defect was
    one site out of fifteen passing the document name itself, and the listener
    answered -32600 at the framing layer before any dispatch. This reads the
    coordinator and refuses the shape at every site.

    A bare name is refused too. That is the exact shape the defect had, so
    allowing names in general would let it straight back in; only the
    forwarders' own ``params`` argument is permitted.
    """

    source = COORDINATOR.read_text(encoding="utf-8")
    unstructured = _unstructured_params_sites(source, COORDINATOR.name)
    assert unstructured == [], "\n".join(unstructured)


def test_the_params_gate_refuses_the_keyword_form_as_well() -> None:
    """GRK-P3-104: the gate has to see ``params=`` or it is not a gate.

    Run over synthetic source: no coordinator site uses the keyword form
    today, so the hole was latent and reading the real file could never
    expose it.
    """

    synthetic = (
        'client.call("m", params="bare")\n'
        'client.call("m", "bare")\n'
        'client.call("m", params=["ok"])\n'
        'client.call("m", params={"ok": 1})\n'
        'client.call("m", params=params)\n'
        'client.call("m", ["ok"])\n'
        'client.call("m")\n'
    )
    reported = _unstructured_params_sites(synthetic, "synthetic.py")
    assert len(reported) == 2, reported
    assert reported[0].startswith("synthetic.py:1 "), reported
    assert "bare" in reported[0], reported
    assert reported[1].startswith("synthetic.py:2 "), reported
    assert "bare" in reported[1], reported


def test_stage_evidence_binds_the_build_and_its_sources_to_commits(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """GRK-P3-098: fingerprints alone never said which commit the build is.

    Section 13 wants Stage A/B tied to the exact committed SHAs. The parent and
    nested SHAs were already recorded; what was missing was any link from the
    running binary to a commit, so a build older than HEAD looked exactly like
    a build of HEAD. The build tree here is mixed rather than uniformly stale,
    so the answer is recorded per binary.
    """

    from tests.gui.part3 import stress_coordinator as module

    binary = tmp_path / "bin" / "FreeCAD.exe"
    binary.parent.mkdir(parents=True, exist_ok=True)
    binary.write_bytes(b"stub-binary")

    provenance = module._build_provenance(REPO_ROOT, binary)
    assert len(provenance["head_commit"]) == 40, provenance["head_commit"]
    assert provenance["head_committed_utc"], provenance
    assert provenance["history_depth"] > 0, provenance
    assert set(provenance["binaries"]) == {"FreeCAD.exe"}, provenance["binaries"]
    entry = provenance["binaries"]["FreeCAD.exe"]
    assert entry["mtime_utc"], entry
    # Written just now, so no commit in the recorded history postdates it.
    assert entry["commits_not_in_binary"] == [], entry
    assert entry["predates_head"] is False, entry
    assert set(provenance["stage_sources"]) == set(module.STAGE_SOURCE_FILES)
    for relative, digest in provenance["stage_sources"].items():
        assert len(digest) == 64, (relative, digest)

    # A binary older than the recorded history is reported as such, per file.
    ancient = tmp_path / "old" / "FreeCAD.exe"
    ancient.parent.mkdir(parents=True, exist_ok=True)
    ancient.write_bytes(b"stub-binary")
    os.utime(ancient, (0, 0))
    aged = module._build_provenance(REPO_ROOT, ancient)
    aged_entry = aged["binaries"]["FreeCAD.exe"]
    relevant_history = module._binary_relevant_history(
        REPO_ROOT, module._commit_history(REPO_ROOT)
    )
    assert aged_entry["predates_head"] is True, aged_entry
    assert aged_entry["commits_not_in_binary"] == [
        commit for commit, _epoch in relevant_history
    ]

    # And a stage run records it in the artifact, with its own check.
    run = _run_stubbed_stage(monkeypatch, tmp_path / "stage", on_shutdown=lambda _c: None)
    recorded = run.evidence["environment"]["build_provenance"]
    assert len(recorded["head_commit"]) == 40, recorded
    assert recorded["binaries"], recorded
    checks = {entry["name"]: entry for entry in run.evidence["checks"]}
    assert PROVENANCE_CHECK in checks, sorted(checks)
    assert checks[PROVENANCE_CHECK]["passed"] is True, checks[PROVENANCE_CHECK]
    # The old name asserted a binding this check has never tested (GRK-P3-101).
    assert OLD_PROVENANCE_CHECK not in checks, sorted(checks)


def test_the_provenance_check_is_named_for_what_it_actually_tests(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """GRK-P3-101: a green check may not claim a property the run lacks.

    ``build_provenance_ties_binaries_and_stage_sources_to_commits`` went green
    on a live run whose binaries were up to 39 commits behind HEAD, and that
    green was then read as validating the binding its name promised. The
    predicate never consulted ``predates_head`` or ``commits_not_in_binary``,
    and it still does not: whether a lagging binary should fail a stage is an
    ADR §13 policy decision this check does not make. What changed is that the
    check is named for what it tests, and the unenforced half is in the
    artifact rather than only in a detail dict.

    The stage below runs against a binary aged to the epoch, so every commit
    in the recorded history postdates it. The check must still be recorded,
    must still be True, and the artifact must say the binding is not enforced.
    """

    run = _run_stubbed_stage(
        monkeypatch,
        tmp_path / "aged",
        on_shutdown=lambda _c: None,
        binary_mtime=0.0,
    )
    provenance = run.evidence["environment"]["build_provenance"]
    assert provenance["binaries_predating_head"] == ["FreeCAD.exe"], provenance
    assert provenance["binary_commit_binding_enforced"] is False, provenance
    assert provenance["provenance_caveat"], provenance

    checks = {entry["name"]: entry for entry in run.evidence["checks"]}
    assert PROVENANCE_CHECK in checks, sorted(checks)
    assert OLD_PROVENANCE_CHECK not in checks, sorted(checks)
    entry = checks[PROVENANCE_CHECK]
    assert entry["passed"] is True, entry
    assert entry["detail"]["binaries_predating_head"] == ["FreeCAD.exe"], entry
    assert entry["detail"]["binary_commit_binding_enforced"] is False, entry


def test_every_module_installed_into_the_gui_profile_is_fingerprinted(
    tmp_path: Path,
) -> None:
    """GRK-P3-102: the whole local_driver directory runs inside FreeCAD.

    ``install_part3_local_driver`` copies the directory - not one file - into
    ``<profile>/FreeCAD/Mod/Part3LocalDriver``, and ``provision`` calls it on
    every run. ``STAGE_SOURCE_FILES`` listed only ``actions.py``, so two runs
    with materially different ``driver.py`` - the module that owns the Qt
    owner-thread hop this work stream turns on - produced identical
    ``stage_sources`` digests.
    """

    from tests.gui.part3 import stress_coordinator as module

    directory = PACKAGE_ROOT / "local_driver"
    installed = sorted(path.name for path in directory.glob("*.py"))
    assert installed, directory
    assert {"InitGui.py", "actions.py", "control_channel.py", "driver.py"} <= set(
        installed
    ), installed

    listed = set(module.STAGE_SOURCE_FILES)
    for name in installed:
        assert f"tests/gui/part3/local_driver/{name}" in listed, (name, sorted(listed))
    # The P3-WP12 cross-check reads actions.py by name; it must stay listed.
    assert "tests/gui/part3/local_driver/actions.py" in listed

    binary = tmp_path / "bin" / "FreeCAD.exe"
    binary.parent.mkdir(parents=True, exist_ok=True)
    binary.write_bytes(b"stub-binary")
    provenance = module._build_provenance(REPO_ROOT, binary)
    for name in installed:
        relative = f"tests/gui/part3/local_driver/{name}"
        digest = provenance["stage_sources"][relative]
        assert len(digest) == 64, (relative, digest)
    # Distinct files, distinct digests: the record can actually move.
    driver_digest = provenance["stage_sources"][
        "tests/gui/part3/local_driver/driver.py"
    ]
    actions_digest = provenance["stage_sources"][
        "tests/gui/part3/local_driver/actions.py"
    ]
    assert driver_digest != actions_digest


def test_an_aborted_stage_records_that_it_never_reached_its_counts(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """GRK-P3-103: a FAILED artifact must not be silent about not running.

    ``cycle_count_matches_stage_definition`` and
    ``save_count_matches_stage_definition`` were recorded only where the whole
    cycle program completed, so an aborted Stage A carried no count check at
    all. The counts were then recoverable only by counting the ``cycles`` and
    ``saves`` arrays by hand - which had to be done, and warned about in
    prose, twice. This cannot produce a false PASS and never could; it is an
    evidence-honesty fix.
    """

    from tests.gui.part3 import stress_coordinator as module

    executed_cycles = 3

    def _abort_after_three_cycles(context: Any) -> None:
        for index in range(executed_cycles):
            context.payload.setdefault("cycles", []).append({"index": index})
        context.payload.setdefault("saves", []).append({"index": 0})
        raise module.StageCheckFailed("history_action_refuses_mismatched_head")

    run = _run_stubbed_stage(
        monkeypatch,
        tmp_path / "aborted",
        on_shutdown=lambda _c: None,
        program=_abort_after_three_cycles,
    )

    checks = {entry["name"]: entry for entry in run.evidence["checks"]}
    assert "cycle_count_matches_stage_definition" in checks, sorted(checks)
    cycles = checks["cycle_count_matches_stage_definition"]
    assert cycles["passed"] is False, cycles
    assert cycles["detail"] == {"expected": 10, "observed": executed_cycles}, cycles
    saves = checks["save_count_matches_stage_definition"]
    assert saves["passed"] is False, saves
    assert saves["detail"] == {"expected": 5, "observed": 1}, saves

    # The aggregates that legitimately describe a COMPLETED program are not
    # manufactured on the abort path.
    assert "adr_section_13_coverage_complete" not in checks, sorted(checks)
    assert "every_save_cycle_is_truthful" not in checks, sorted(checks)

    assert run.evidence["verdict"] == "FAILED", run.evidence["verdict"]
    assert run.exit_code == 1, run.exit_code


# ---------------------------------------------------------------------------
# GRK-P3-106: an ordered shutdown that ABORTED is never one that COMPLETED.
# ---------------------------------------------------------------------------

PROBE_DOCUMENT = "Part3StageAPrimary"
PROBE_RPC_ERROR = f"JSON-RPC error -32000: Unknown document '{PROBE_DOCUMENT}'"


class _AbortingCloseRpc:
    """A typed session whose ``close_document`` fails the way GRK-P3-099 does."""

    session_token = "stub-session-token"
    mcp_instance_id = "stub-mcp-instance"

    def __init__(self) -> None:
        self.calls: list[str] = []

    def call(self, method: str, params: Any = None, timeout: float = 30.0) -> Any:
        self.calls.append(method)
        if method == "list_documents":
            return {"documents": [{"name": PROBE_DOCUMENT}]}
        if method == "close_document":
            raise RuntimeError(PROBE_RPC_ERROR)
        return {}


def _drive_ordered_shutdown(
    monkeypatch: pytest.MonkeyPatch,
    workdir: Path,
    *,
    rpc: Any,
    local_driver: Any = None,
) -> tuple[dict[str, Any], dict[str, Any], list[Any]]:
    """Drive the REAL ordered shutdown offline over a caller-chosen shape.

    Offline: no FreeCAD, no sockets, no launcher. Only the actors are stubbed;
    ``graceful_shutdown_owned_session`` itself is the real frozen WP09 helper.
    The owned process double always exits inside the deadline, which is the
    trigger every finding in this family shares. Returns the helper's result,
    the shutdown block it actually persisted, and every process the force-kill
    helper was asked to kill.
    """

    from tests.gui.part3 import stress_coordinator as module

    evidence_path = workdir / "evidence.json"
    write_evidence(evidence_path, empty_evidence(stage="A"))
    killed: list[Any] = []
    launcher_module = SimpleNamespace(
        JsonRpcClient=lambda **_kwargs: SimpleNamespace(),
        JsonRpcError=RuntimeError,
        JsonRpcTransportError=RuntimeError,
    )
    with monkeypatch.context() as patched:
        patched.setattr(module, "authenticate_json_rpc", lambda *_a, **_k: rpc)
        patched.setattr(
            module,
            "_force_kill_owned_process_tree",
            lambda process: killed.append(process),
        )
        result = module.graceful_shutdown_owned_session(
            process=_StubProcess(),
            profile_root=workdir / "profile",
            launcher_module=launcher_module,
            repo_root=REPO_ROOT,
            mcp_port=0,
            local_driver=local_driver,
            evidence_path=evidence_path,
            deadline_seconds=1,
        )
    persisted = json.loads(evidence_path.read_text(encoding="utf-8"))["shutdown"]
    return result, persisted, killed


def _drive_aborted_ordered_shutdown(
    monkeypatch: pytest.MonkeyPatch,
    workdir: Path,
) -> tuple[dict[str, Any], dict[str, Any], list[str], list[Any]]:
    """Drive the REAL ordered shutdown: close_document aborts, process exits.

    Offline: no FreeCAD, no sockets, no launcher. Returns the helper's result,
    the shutdown block it actually persisted, the RPC verbs it issued, and
    every process the force-kill helper was asked to kill.
    """

    rpc = _AbortingCloseRpc()
    result, persisted, killed = _drive_ordered_shutdown(
        monkeypatch, workdir, rpc=rpc, local_driver=None
    )
    return result, persisted, rpc.calls, killed


def test_an_aborted_rpc_phase_can_never_record_a_completed_shutdown(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """GRK-P3-106: the most load-bearing check in Part 3 had a green bypass.

    ``graceful_shutdown_owned_session`` clears ``forced`` and ``stalled_stage``
    and returns ``success: True`` whenever the owned process exits inside the
    deadline - including when the ordered RPC phase aborted at
    ``close_document`` and the shutdown verb was never sent. The acceptance
    computation read only those two fields, so
    ``graceful_shutdown_completed_without_forced_termination`` went GREEN on a
    shutdown that never happened, with all three phase timestamps null and no
    forced kill anywhere in the run - which bypassed, rather than defeated,
    every "a forced kill is never green" guard in the harness.

    Two halves, chained so neither is assumed. The first drives the REAL
    ordered shutdown and captures what it actually persists. The second feeds
    that captured block - not a hand-written one - through the real
    ``run_stage`` acceptance path and requires the check to be RED.
    """

    result, persisted, calls, killed = _drive_aborted_ordered_shutdown(
        monkeypatch, tmp_path / "ordered"
    )

    # Half 1: what the real ordered shutdown really does. This half is the
    # helper's frozen WP09 contract, unchanged by this fix, and is asserted so
    # the second half cannot be reading a shape that no longer occurs.
    assert calls == ["list_documents", "close_document"], calls
    assert killed == [], killed
    assert result["success"] is True, result
    assert result["forced"] is False, result
    assert result["stalled_stage"] is None, result
    assert persisted["failed_step"] == "document_close", persisted
    assert PROBE_RPC_ERROR in persisted["rpc_error"], persisted
    assert persisted["documents_closed_utc"] is None, persisted
    assert persisted["rpc_admission_closed_utc"] is None, persisted
    assert persisted["window_closed_utc"] is None, persisted
    assert persisted["process_exit_utc"], persisted

    # Half 2: the acceptance path must refuse to call that a completed
    # shutdown, whether or not the process exited inside the deadline.
    def _record_the_aborted_shutdown(coordinator: Any) -> None:
        payload = json.loads(coordinator.evidence_path.read_text(encoding="utf-8"))
        payload["shutdown"] = persisted
        write_evidence(coordinator.evidence_path, payload)

    run = _run_stubbed_stage(
        monkeypatch,
        tmp_path / "stage",
        on_shutdown=_record_the_aborted_shutdown,
    )
    checks = {entry["name"]: entry for entry in run.evidence["checks"]}
    name = "graceful_shutdown_completed_without_forced_termination"
    assert name in checks, sorted(checks)
    check = checks[name]
    assert check["passed"] is False, check

    # The recorded detail keeps the abort visible, so no artifact can ever
    # again read "completed" and "failed_step: document_close" at once with a
    # green check between them.
    assert check["detail"]["failed_step"] == "document_close", check
    assert PROBE_RPC_ERROR in check["detail"]["rpc_error"], check
    assert check["detail"]["forced"] is False, check
    assert run.evidence["shutdown"]["failed_step"] == "document_close"
    assert run.evidence["verdict"] == "FAILED", run.evidence["verdict"]
    assert run.exit_code == 1, run.exit_code


def test_an_aborted_stage_persists_its_counts_even_when_the_walk_raises(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """GRK-P3-107: the count checks must reach DISK on their own.

    ``_record_stage_counts`` only mutates the in-memory payload. The abort path
    persisted it solely inside a second suppressed block whose first statement
    is ``_record_artifact_snapshot`` - a filesystem walk that runs while the
    owned FreeCAD is still creating and removing lock anchors, ``.FCStd1``
    backups and temporaries under ``documents_dir``. A raise out of that walk
    was suppressed, ``_persist`` never ran, and ``run_stage`` re-reads the
    artifact FROM DISK - so GRK-P3-103's defect returned, silently, in the
    exact scenario it was filed about.
    """

    from tests.gui.part3 import stress_coordinator as module

    executed_cycles = 3

    def _abort_after_three_cycles(context: Any) -> None:
        for index in range(executed_cycles):
            context.payload.setdefault("cycles", []).append({"index": index})
        context.payload.setdefault("saves", []).append({"index": 0})
        raise module.StageCheckFailed("history_action_refuses_mismatched_head")

    def _walk_loses_a_race(_context: Any) -> None:
        raise OSError("artifact walk lost a race with the live owned process")

    monkeypatch.setattr(module, "_record_artifact_snapshot", _walk_loses_a_race)
    run = _run_stubbed_stage(
        monkeypatch,
        tmp_path / "walk-raises",
        on_shutdown=lambda _c: None,
        program=_abort_after_three_cycles,
    )

    # Read the artifact back from disk: what a reader gets is the file, and the
    # in-memory payload is not it. That distinction is the whole finding.
    on_disk = json.loads(run.coordinator.evidence_path.read_text(encoding="utf-8"))
    checks = {entry["name"]: entry for entry in on_disk["checks"]}
    assert "cycle_count_matches_stage_definition" in checks, sorted(checks)
    cycles = checks["cycle_count_matches_stage_definition"]
    assert cycles["passed"] is False, cycles
    assert cycles["detail"] == {"expected": 10, "observed": executed_cycles}, cycles
    saves = checks["save_count_matches_stage_definition"]
    assert saves["passed"] is False, saves
    assert saves["detail"] == {"expected": 5, "observed": 1}, saves

    # The snapshot really did raise, so this is not a vacuous pass, and the
    # snapshot's own failure is still contained: it did not take the counts
    # with it and did not stop the stage from failing.
    assert "artifacts_in_stage" not in on_disk, sorted(on_disk)
    assert on_disk["verdict"] == "FAILED", on_disk["verdict"]
    assert run.exit_code == 1, run.exit_code


# ---------------------------------------------------------------------------
# GRK-P3-109 / GRK-P3-110 / GRK-P3-112: one predicate, asked at every gate.
#
# The same defect class - a shutdown that did NOT complete being recordable as
# green - was found at four sites across three iterations: the RPC phase
# (GRK-P3-106), the window-close branch (GRK-P3-109), the preflight CLI path
# (GRK-P3-110) and a route that records NO marker field at all (GRK-P3-112).
# Adding one ``and X is None`` conjunct per reviewed route cannot terminate,
# because the last of those has nothing to deny. These tests pin the allowlist
# that replaces the denylist, and the single helper every gate now asks.
# ---------------------------------------------------------------------------

SHARED_SHUTDOWN_PREDICATE = "ordered_shutdown_completed"
SHUTDOWN_COMPLETED_CHECK = "graceful_shutdown_completed_without_forced_termination"
PROBE_WINDOW_ERROR = "close_main_window failed: driver channel closed"


class _CleanRpc:
    """A typed session on which every ordered-shutdown verb succeeds.

    ``documents`` is what ``list_documents`` reports, so a caller can also
    drive the zero-open-documents shape. That shape matters on its own:
    ``documents_closed_utc`` is stamped unconditionally AFTER the close loop,
    so it is stamped even when the loop never ran, and GRK-P3-113 asks for
    that asymmetry to be pinned rather than assumed.
    """

    session_token = "stub-session-token"
    mcp_instance_id = "stub-mcp-instance"

    def __init__(self, documents: tuple[str, ...] = (PROBE_DOCUMENT,)) -> None:
        self.calls: list[str] = []
        self.documents = tuple(documents)

    def call(self, method: str, params: Any = None, timeout: float = 30.0) -> Any:
        self.calls.append(method)
        if method == "list_documents":
            return {"documents": [{"name": name} for name in self.documents]}
        return {}


class _RefusingWindowDriver:
    """A local driver whose ``close_main_window`` fails after a clean RPC phase."""

    def __init__(self) -> None:
        self.invoked: list[str] = []

    def invoke(self, action: str, params: Any = None, **kwargs: Any) -> dict[str, Any]:
        self.invoked.append(action)
        raise RuntimeError(PROBE_WINDOW_ERROR)


class _AcceptingWindowDriver:
    """A local driver whose ``close_main_window`` SUCCEEDS.

    The clean counterpart of ``_RefusingWindowDriver``. Every other driver
    double in this family refuses, which is why the allowlist had no landed
    proof that it ADMITS a shutdown that really completed (GRK-P3-113).
    """

    def __init__(self) -> None:
        self.invoked: list[str] = []

    def invoke(self, action: str, params: Any = None, **kwargs: Any) -> dict[str, Any]:
        self.invoked.append(action)
        return {"ok": True}


def _assert_the_acceptance_path_refuses(
    monkeypatch: pytest.MonkeyPatch,
    workdir: Path,
    persisted: dict[str, Any],
) -> _StubStageRun:
    """Feed a CAPTURED shutdown block through the real ``run_stage`` gate.

    The block is one a real ``graceful_shutdown_owned_session`` wrote, never a
    hand-written shape, so neither half of these tests assumes the other.
    """

    def _record_the_captured_shutdown(coordinator: Any) -> None:
        payload = json.loads(coordinator.evidence_path.read_text(encoding="utf-8"))
        payload["shutdown"] = persisted
        write_evidence(coordinator.evidence_path, payload)

    run = _run_stubbed_stage(
        monkeypatch, workdir, on_shutdown=_record_the_captured_shutdown
    )
    checks = {entry["name"]: entry for entry in run.evidence["checks"]}
    assert SHUTDOWN_COMPLETED_CHECK in checks, sorted(checks)
    check = checks[SHUTDOWN_COMPLETED_CHECK]
    assert check["passed"] is False, check
    assert run.evidence["verdict"] == "FAILED", run.evidence["verdict"]
    assert run.exit_code == 1, run.exit_code
    return run


def test_a_failed_window_close_can_never_record_a_completed_shutdown(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """GRK-P3-109: the window-close branch reached the same false green.

    The window-close arm runs only when the RPC phase SUCCEEDED, so this route
    can carry ``stage_ok`` True and produce a real ``PART3_RESULT: PASSED``.
    The arm records ``window_error`` and ``stalled_stage``; the deadline branch
    then erases ``stalled_stage`` again and returns ``success: True``. Neither
    ``failed_step`` nor ``rpc_error`` is written anywhere on this route, so the
    GRK-P3-106 conjuncts evaluated it GREEN with ``window_closed_utc`` null.

    Closed WITHOUT a fourth ``and X is None`` clause: a window close that
    failed cannot stamp ``window_closed_utc``, and the allowlist requires it.
    """

    driver = _RefusingWindowDriver()
    rpc = _CleanRpc()
    result, persisted, killed = _drive_ordered_shutdown(
        monkeypatch, tmp_path / "ordered", rpc=rpc, local_driver=driver
    )

    # Half 1: what the real helper does. Its frozen WP09 return contract is
    # asserted here so the second half cannot be reading a shape that no
    # longer occurs.
    assert rpc.calls == ["list_documents", "close_document", "shutdown_rpc_server"], (
        rpc.calls
    )
    assert driver.invoked == ["close_main_window"], driver.invoked
    assert killed == [], killed
    assert result["success"] is True, result
    assert result["forced"] is False, result
    assert result["stalled_stage"] is None, result
    assert PROBE_WINDOW_ERROR in persisted["window_error"], persisted
    assert persisted["window_closed_utc"] is None, persisted
    assert persisted["documents_closed_utc"], persisted
    assert persisted["rpc_admission_closed_utc"], persisted
    # The two GRK-P3-106 conjuncts have nothing to catch on this route.
    assert "failed_step" not in persisted, persisted
    assert "rpc_error" not in persisted, persisted

    # Half 2: the acceptance path must refuse to call that a completed
    # shutdown, and the abort must stay visible in the recorded detail.
    run = _assert_the_acceptance_path_refuses(monkeypatch, tmp_path / "stage", persisted)
    checks = {entry["name"]: entry for entry in run.evidence["checks"]}
    detail = checks[SHUTDOWN_COMPLETED_CHECK]["detail"]
    assert PROBE_WINDOW_ERROR in detail["window_error"], detail
    assert detail["window_closed_utc"] is None, detail


def test_a_shutdown_that_records_no_marker_at_all_is_still_not_completed(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """GRK-P3-112: the route that proves a denylist cannot terminate.

    With ``local_driver`` None and no skip flags, every window-close arm is
    False - arm 1 wants a driver, arms 2 and 3 want a skip flag - so control
    falls straight to the deadline branch having recorded NOTHING: no
    ``window_error``, no ``window_skipped``, no ``failed_step``, no
    ``rpc_error``, ``stalled_stage`` None and ``forced`` False. There is no
    field a further conjunct could deny. It is reachable from ``run_stage``:
    ``_handoff`` is assigned only inside ``connect_local_driver``, so anything
    raising between ``launch_freecad`` and that call leaves it None while
    ``_launcher_process`` is set, and the ``finally`` then tears down with
    ``local_driver=None``.

    The allowlist closes it without anyone enumerating it: the window phase
    never ran, so ``window_closed_utc`` was never stamped.
    """

    rpc = _CleanRpc()
    result, persisted, killed = _drive_ordered_shutdown(
        monkeypatch, tmp_path / "ordered", rpc=rpc, local_driver=None
    )

    # Half 1: there is genuinely no marker to deny.
    assert rpc.calls == ["list_documents", "close_document", "shutdown_rpc_server"], (
        rpc.calls
    )
    assert killed == [], killed
    assert result["success"] is True, result
    assert result["forced"] is False, result
    assert result["stalled_stage"] is None, result
    assert persisted["stalled_stage"] is None, persisted
    assert persisted["forced"] is False, persisted
    for marker in (
        "failed_step",
        "rpc_error",
        "window_error",
        "window_skipped",
        "rpc_shutdown_skipped",
    ):
        assert marker not in persisted, (marker, persisted)

    # ...and the phase that did not run left its transition unstamped, which
    # is the only thing that distinguishes this block from a clean one.
    assert persisted["window_closed_utc"] is None, persisted
    assert persisted["documents_closed_utc"], persisted
    assert persisted["rpc_admission_closed_utc"], persisted
    assert persisted["worker_shutdown_utc"], persisted
    assert persisted["listener_shutdown_utc"], persisted
    assert persisted["process_exit_utc"], persisted

    # Half 2: the acceptance path records it RED anyway.
    _assert_the_acceptance_path_refuses(monkeypatch, tmp_path / "stage", persisted)


def test_preflight_only_can_never_report_passed_over_an_aborted_shutdown(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
    capsys: pytest.CaptureFixture[str],
) -> None:
    """GRK-P3-110: the operator-facing CLI carried the unfixed GRK-P3-106.

    ``_run_preflight_only`` gated on the returned ``success`` alone and wrote
    nothing after its ``finally``, so the mid-shutdown verdict stamp was the
    one that survived: ``PART3_RESULT: PASSED``, exit 0 and ``verdict:
    "PASSED"`` in an artifact carrying ``failed_step: "document_close"`` with
    every phase timestamp null. ``run_stage`` had been fixed; this sibling had
    not, because the two paths were fixed independently.

    Driven end to end: the REAL ``_run_preflight_only``, the REAL
    ``graceful_shutdown_owned_session``, stub actors only. No GUI is launched,
    no socket is opened, and both verdict lines are captured by capsys.
    """

    from tests.gui.part3 import stress_coordinator as module

    real_coordinator = module.StressCoordinator
    rpc = _AbortingCloseRpc()
    killed: list[Any] = []
    launcher_module = SimpleNamespace(
        JsonRpcClient=lambda **_kwargs: SimpleNamespace(),
        JsonRpcError=RuntimeError,
        JsonRpcTransportError=RuntimeError,
    )

    class _AbortingShutdownCoordinator(real_coordinator):  # type: ignore[misc, valid-type]
        def __init__(self) -> None:
            real_coordinator.__init__(
                self, run_root=tmp_path / "preflight", repo_root=REPO_ROOT
            )

        def provision(self) -> str:
            self.evidence_dir.mkdir(parents=True, exist_ok=True)
            write_evidence(self.evidence_path, empty_evidence(stage=None))
            return "stub-control-token"

        def launch_freecad(self, freecad_exe: Path | None = None) -> Any:
            self._launcher_process = _StubProcess()
            return self._launcher_process

        def wait_for_launcher_ready(self, timeout_s: float = 0.0) -> None:
            return None

        def spawn_remote_agent_child(self, **kwargs: Any) -> Any:
            return SimpleNamespace(returncode=0, stdout="{}", stderr="")

        def connect_local_driver(self) -> Any:
            return SimpleNamespace(endpoint={}, local_driver=None)

        def run_preflight(self, handoff: Any) -> dict[str, Any]:
            return {"pause_checkbox_visible": True, "pause_checkbox_wired": True}

        def shutdown_launcher(self, *, success_verdict: str = "PASSED") -> dict[str, Any]:
            # The REAL ordered shutdown, not a stub of it: this test is about
            # what the CLI does with what that helper really returns.
            return module.graceful_shutdown_owned_session(
                process=self._launcher_process,
                profile_root=self.profile_root,
                launcher_module=launcher_module,
                repo_root=REPO_ROOT,
                mcp_port=0,
                local_driver=None,
                evidence_path=self.evidence_path,
                deadline_seconds=1,
                success_verdict=success_verdict,
            )

    monkeypatch.setattr(module, "authenticate_json_rpc", lambda *_a, **_k: rpc)
    monkeypatch.setattr(
        module,
        "_force_kill_owned_process_tree",
        lambda process: killed.append(process),
    )
    monkeypatch.setattr(
        module, "default_freecad_exe", lambda *_args, **_kwargs: tmp_path / "FreeCAD.exe"
    )
    monkeypatch.setattr(module, "StressCoordinator", _AbortingShutdownCoordinator)

    exit_code = module._run_preflight_only()
    captured = capsys.readouterr()
    artifact = json.loads(
        (tmp_path / "preflight" / "evidence" / "evidence.json").read_text(
            encoding="utf-8"
        )
    )
    shutdown = artifact["shutdown"]

    # Half 1: the ordered shutdown really did abort, really was not forced,
    # and really did hand back success: True - the false return contract this
    # caller inherited.
    assert rpc.calls == ["list_documents", "close_document"], rpc.calls
    assert killed == [], killed
    assert shutdown["failed_step"] == "document_close", shutdown
    assert PROBE_RPC_ERROR in shutdown["rpc_error"], shutdown
    assert shutdown["forced"] is False, shutdown
    assert shutdown["documents_closed_utc"] is None, shutdown
    assert shutdown["rpc_admission_closed_utc"] is None, shutdown
    assert shutdown["window_closed_utc"] is None, shutdown

    # Half 2: the operator-facing gate refuses it, on every channel a reader
    # or a CI job could believe - the printed line, the exit code and the
    # artifact that outlives both.
    assert exit_code != 0, captured.out
    assert "PART3_RESULT: PASSED" not in captured.out, captured.out
    assert "PART3_RESULT: FAILED" in captured.out, captured.out
    assert artifact["verdict"] != "PASSED", artifact["verdict"]
    assert artifact["mode"] == "preflight_only", artifact
    failed = [entry["name"] for entry in artifact["failed_checks"]]
    assert SHUTDOWN_COMPLETED_CHECK in failed, failed


def test_a_clean_ordered_shutdown_stamps_every_transition_and_is_accepted(
    monkeypatch: pytest.MonkeyPatch,
    tmp_path: Path,
) -> None:
    """GRK-P3-113: the over-fire control the tightened allowlist never had.

    Every other test in this family asserts the REFUSAL direction, and the one
    positive assertion is made over a hand-built record - true by construction,
    and still true if ``graceful_shutdown_owned_session`` stopped stamping a
    transition tomorrow. This is the other direction, driven through the REAL
    frozen WP09 helper: a shutdown that genuinely completed must stamp every
    key in ``SHUTDOWN_TIMESTAMP_KEYS`` and must be ACCEPTED.

    It matters because the predicate went from two failure-marker conjuncts to
    a seven-key allowlist, which strictly increases over-fire risk, at the same
    time as GRK-P3-099 makes a live green stage unreachable. If a transition
    stopped being stamped on the clean path, every Stage A/B run would fail
    ``graceful_shutdown_completed_without_forced_termination`` with no offline
    test failing and no live green available to notice it - a permanently red
    acceptance gate with no reproducer, which on this work stream is the
    symptom that has cost the most time. Offline is the only place that guard
    can live, so it lives here.

    Both document shapes are driven: ``documents_closed_utc`` is stamped
    unconditionally AFTER the close loop, so the zero-document run stamps it
    without closing anything, and that asymmetry is pinned rather than assumed.
    """

    from tests.gui.part3 import stress_coordinator as module

    shapes = (
        (
            "one-open-document",
            (PROBE_DOCUMENT,),
            ["list_documents", "close_document", "shutdown_rpc_server"],
        ),
        ("zero-open-documents", (), ["list_documents", "shutdown_rpc_server"]),
    )
    for label, documents, expected_calls in shapes:
        rpc = _CleanRpc(documents=documents)
        driver = _AcceptingWindowDriver()
        result, persisted, killed = _drive_ordered_shutdown(
            monkeypatch, tmp_path / label, rpc=rpc, local_driver=driver
        )

        # The clean path really is clean: every ordered phase ran and nothing
        # was force-killed. A forced kill is never success (ADR §9), so a
        # positive test that tolerated one would be worse than no test at all.
        assert rpc.calls == expected_calls, (label, rpc.calls)
        assert driver.invoked == ["close_main_window"], (label, driver.invoked)
        assert killed == [], (label, killed)
        assert result["success"] is True, (label, result)
        assert result["forced"] is False, (label, result)
        assert result["stalled_stage"] is None, (label, result)
        assert persisted["forced"] is False, (label, persisted)
        assert persisted["stalled_stage"] is None, (label, persisted)
        for marker in (
            "failed_step",
            "rpc_error",
            "window_error",
            "window_skipped",
            "rpc_shutdown_skipped",
        ):
            assert marker not in persisted, (label, marker, persisted)

        # (1) Every transition the allowlist requires was stamped by the REAL
        # helper, in the block it really PERSISTED. Collected rather than
        # asserted one key at a time so a regression NAMES the transition that
        # stopped being stamped instead of pointing at a line number.
        unstamped = [key for key in SHUTDOWN_TIMESTAMP_KEYS if not persisted.get(key)]
        assert unstamped == [], (label, unstamped, persisted)

        # (2) ...and the shared predicate ADMITS it. This is the assertion the
        # tightening never had: the allowlist does not over-fire on the only
        # path a green Stage A or Stage B can take.
        assert module.ordered_shutdown_completed(result, persisted) is True, (
            label,
            persisted,
        )


# ---------------------------------------------------------------------------
# GRK-P3-114/115/116: the shared-predicate guard, enforced NAME-AGNOSTICALLY,
# fail-CLOSED on what it cannot analyse, and controlled against over-fire.
#
# The guard below used to key on assignments to the literal name
# ``shutdown_ok`` and on ``ast.Assign`` alone, while its docstring claimed to
# catch any gate that computes shutdown success its own way. Three evasions
# were OBSERVED walking through it: a result bound to another name and
# truth-tested (the GRK-P3-110 shape, one rename later), the annotated
# fail-OPEN form ``shutdown_ok: bool = True``, and a second renamed decision
# inside ``run_stage``. The standing correction on this work stream is "grep
# for the PREDICATE, not the line"; a name-keyed guard is the line.
#
# GRK-P3-115 then found the name-agnostic guard still keyed on ONE literal -
# ``SHUTDOWN_ENTRYPOINT`` - so a gate calling ``graceful_shutdown_owned_session``
# around the wrapper was invisible to it, and ``_assigned_name`` answered None,
# read everywhere as "nothing to reason about", for every binding form but
# ``ast.Assign`` and ``ast.AnnAssign``: the augmented, walrus and tuple-unpack
# bindings of ``shutdown_ok`` all failed OPEN. GRK-P3-116 found the mirror
# defect - the guard REJECTING the correct gate written with keyword arguments,
# which is the shape that creates pressure to weaken the gate itself.
# ---------------------------------------------------------------------------

SHUTDOWN_ENTRYPOINT = "shutdown_launcher"
SHUTDOWN_OWNED_SESSION = "graceful_shutdown_owned_session"


def _owned_session_callers(tree: ast.AST) -> list[tuple[int, str | None]]:
    """``(lineno, innermost enclosing function)`` per call to the frozen helper.

    The taint half keys on the single literal ``SHUTDOWN_ENTRYPOINT``, so it
    only ever sees results that came through the ``shutdown_launcher``
    wrapper. A gate calling ``graceful_shutdown_owned_session`` directly walked
    around the entire analysis (GRK-P3-115). ``None`` means module or class
    scope, which is a bypass too.
    """

    calls: list[tuple[int, str | None]] = []

    def _descend(node: ast.AST, enclosing: str | None) -> None:
        for child in ast.iter_child_nodes(node):
            if isinstance(child, ast.Call) and (
                _called_entrypoint_name(child) == SHUTDOWN_OWNED_SESSION
            ):
                calls.append((child.lineno, enclosing))
            if isinstance(child, (ast.FunctionDef, ast.AsyncFunctionDef)):
                _descend(child, child.name)
            else:
                _descend(child, enclosing)

    _descend(tree, None)
    return calls


def _use_parent(node: ast.AST, parents: dict[int, ast.AST]) -> ast.AST | None:
    """Where ``node`` is USED, seeing through ``ast.keyword``.

    For ``f(result=shutdown)`` the tainted Name's parent is the ``ast.keyword``
    node rather than the ``ast.Call``, so the "did it ask the shared
    predicate?" exemption never matched and the CORRECT gate written in
    keyword form was reported as a violation (GRK-P3-116). Keyword style is
    already the idiom in the gate's own function - ``stage_verdict`` and both
    ``shutdown_launcher`` call sites are written that way - so this was a
    red-on-correct-code trap in the one test that has to stay trustworthy.
    Resolving through the keyword and applying the SAME predicate check to the
    enclosing call keeps every genuine evasion refused: the result passed by
    keyword to anything that is not the predicate still lands as a violation.
    """

    parent = parents.get(id(node))
    if isinstance(parent, ast.keyword):
        return parents.get(id(parent), parent)
    return parent


def _assigned_name(node: ast.AST) -> str | None:
    """The single plain name ``node`` binds, through ``ast.AnnAssign`` too.

    The guard handled ``ast.Assign`` only, so ``shutdown_ok: bool = True`` - a
    fail-OPEN initialiser - matched neither of its branches and passed
    silently. Every assignment this gate reasons about now goes through one
    function, so the annotated form cannot be handled at one site and
    forgotten at another.
    """

    if isinstance(node, ast.Assign):
        if len(node.targets) == 1 and isinstance(node.targets[0], ast.Name):
            return node.targets[0].id
        return None
    if isinstance(node, ast.AnnAssign) and isinstance(node.target, ast.Name):
        return node.target.id
    return None


def _scope_nodes(scope: ast.AST, *, enter_functions: bool) -> list[ast.AST]:
    """Descendants of ``scope``; module level does not descend into defs.

    The binding analysis has to be per-scope.
    ``graceful_shutdown_owned_session`` has its own local named ``shutdown``,
    loaded from the evidence file rather than returned by
    ``shutdown_launcher``, and subscripts it throughout; a whole-module walk
    would confuse the two names and report the frozen helper as a violation.
    """

    owned: list[ast.AST] = []

    def _descend(node: ast.AST) -> None:
        for child in ast.iter_child_nodes(node):
            if not enter_functions and isinstance(
                child, (ast.FunctionDef, ast.AsyncFunctionDef, ast.ClassDef)
            ):
                continue
            owned.append(child)
            _descend(child)

    _descend(scope)
    return owned


def _parent_map(scope: ast.AST, nodes: list[ast.AST]) -> dict[int, ast.AST]:
    """Where each node in ``nodes`` sits, so a USE can be classified."""

    parents: dict[int, ast.AST] = {}
    for node in [scope, *nodes]:
        for child in ast.iter_child_nodes(node):
            parents.setdefault(id(child), node)
    return parents


def _shutdown_gate_violations(source: str) -> list[str]:
    """The ways ``source`` decides shutdown success without asking the predicate.

    NAME-AGNOSTIC, which is the entire point. This resolves what each
    ``shutdown_launcher`` call is BOUND to and requires that binding to reach
    ``ordered_shutdown_completed`` and nothing else: not a truth test, not
    ``.get("success")``, not a subscript, and not a laundering alias. A call
    whose result is never bound at all - the inline
    ``shutdown_launcher(...).get("success")`` shape - is a violation too, and
    so is a call to ``graceful_shutdown_owned_session`` from anywhere but the
    ``shutdown_launcher`` wrapper this analysis keys on.

    Bindings of ``shutdown_ok`` in a form this gate cannot analyse are REFUSED
    rather than trusted. ``_assigned_name`` answers None for every form but
    ``ast.Assign`` and ``ast.AnnAssign``, and each caller reads None as "nothing
    to reason about", so the augmented, walrus and tuple-unpack bindings failed
    OPEN (GRK-P3-115). Function parameters are ``ast.arg`` rather than
    ``ast.Name`` and stay exempt by construction, which is what keeps
    ``stage_verdict``'s legitimate ``shutdown_ok`` parameter clean.

    Returns one entry per offending site, so ``[]`` is the only clean answer
    and a failure names the line rather than only the fact. This is NOT a
    universal claim; the two shapes deliberately outside it are named in
    ``test_every_shutdown_gate_asks_the_one_shared_predicate``.
    """

    tree = ast.parse(source)
    found: set[tuple[int, str]] = set()

    scopes: list[tuple[ast.AST, bool]] = [(tree, False)]
    scopes.extend(
        (node, True)
        for node in ast.walk(tree)
        if isinstance(node, (ast.FunctionDef, ast.AsyncFunctionDef))
    )

    for scope, enter_functions in scopes:
        nodes = _scope_nodes(scope, enter_functions=enter_functions)
        parents = _parent_map(scope, nodes)

        # Which local names hold a shutdown result. Iterated to a fixpoint so
        # an alias chain cannot launder the taint away.
        tainted: set[str] = set()
        while True:
            grown = False
            for node in nodes:
                if not isinstance(node, (ast.Assign, ast.AnnAssign)):
                    continue
                name = _assigned_name(node)
                value = node.value
                if name is None or value is None or name in tainted:
                    continue
                if isinstance(value, ast.Call) and (
                    _called_entrypoint_name(value) == SHUTDOWN_ENTRYPOINT
                ):
                    tainted.add(name)
                    grown = True
                elif isinstance(value, ast.Name) and value.id in tainted:
                    tainted.add(name)
                    grown = True
            if not grown:
                break

        for node in nodes:
            if isinstance(node, ast.Call) and (
                _called_entrypoint_name(node) == SHUTDOWN_ENTRYPOINT
            ):
                parent = _use_parent(node, parents)
                bound = isinstance(parent, (ast.Assign, ast.AnnAssign)) and bool(
                    _assigned_name(parent)
                )
                asked = isinstance(parent, ast.Call) and (
                    _called_entrypoint_name(parent) == SHARED_SHUTDOWN_PREDICATE
                )
                if not bound and not asked:
                    found.add(
                        (
                            node.lineno,
                            f"{SHUTDOWN_ENTRYPOINT}() is consumed by "
                            f"{type(parent).__name__} instead of "
                            f"{SHARED_SHUTDOWN_PREDICATE}",
                        )
                    )
                continue
            if not isinstance(node, ast.Name) or node.id not in tainted:
                continue
            if not isinstance(node.ctx, ast.Load):
                continue
            parent = _use_parent(node, parents)
            if isinstance(parent, ast.Call) and (
                _called_entrypoint_name(parent) == SHARED_SHUTDOWN_PREDICATE
            ):
                continue
            if isinstance(parent, (ast.Assign, ast.AnnAssign)) and _assigned_name(
                parent
            ):
                continue  # a plain alias; the alias itself carries the taint
            found.add(
                (
                    node.lineno,
                    f"shutdown result {node.id!r} reaches "
                    f"{type(parent).__name__} instead of "
                    f"{SHARED_SHUTDOWN_PREDICATE}",
                )
            )

    # The named gate itself, now including the annotated form.
    for node in ast.walk(tree):
        if not isinstance(node, (ast.Assign, ast.AnnAssign)):
            continue
        value = node.value
        if _assigned_name(node) != "shutdown_ok" or value is None:
            continue
        if isinstance(value, ast.Constant):
            if value.value is not False:
                found.add(
                    (
                        node.lineno,
                        f"shutdown_ok initialiser {value.value!r} is not "
                        f"fail-CLOSED",
                    )
                )
        elif not (
            isinstance(value, ast.Call)
            and _called_entrypoint_name(value) == SHARED_SHUTDOWN_PREDICATE
        ):
            found.add(
                (
                    node.lineno,
                    f"shutdown_ok is assigned from {type(value).__name__}, "
                    f"not from {SHARED_SHUTDOWN_PREDICATE}",
                )
            )

    # The wrapper cannot be bypassed. Everything above keys on the single
    # literal ``shutdown_launcher``, so a gate calling the frozen owned-session
    # helper directly evaded the whole analysis (GRK-P3-115).
    for lineno, enclosing in _owned_session_callers(tree):
        if enclosing != SHUTDOWN_ENTRYPOINT:
            found.add(
                (
                    lineno,
                    f"{SHUTDOWN_OWNED_SESSION}() is called by "
                    f"{enclosing or 'module or class scope'}, bypassing the "
                    f"{SHUTDOWN_ENTRYPOINT}() wrapper this gate analyses",
                )
            )

    # Bindings of ``shutdown_ok`` this gate cannot analyse fail CLOSED.
    # ``_assigned_name`` answers None for anything but ``ast.Assign`` and
    # ``ast.AnnAssign`` and every caller reads None as "nothing to reason
    # about", so ``shutdown_ok |= ...``, ``(shutdown_ok := True)`` and
    # ``shutdown_ok, _x = True, None`` all passed silently. Parameters are
    # ``ast.arg``, not ``ast.Name``, so ``stage_verdict``'s legitimate
    # ``shutdown_ok`` parameter is exempt by construction rather than by an
    # allowlist that could drift.
    whole_parents: dict[int, ast.AST] = {}
    for holder in ast.walk(tree):
        for child in ast.iter_child_nodes(holder):
            whole_parents.setdefault(id(child), holder)
    for node in ast.walk(tree):
        if not isinstance(node, ast.Name) or node.id != "shutdown_ok":
            continue
        if not isinstance(node.ctx, ast.Store):
            continue
        binder = whole_parents.get(id(node))
        if isinstance(binder, (ast.Assign, ast.AnnAssign)) and (
            _assigned_name(binder) == "shutdown_ok"
        ):
            continue  # the analysable forms, already decided above
        found.add(
            (
                node.lineno,
                f"shutdown_ok is bound by {type(binder).__name__}, a form this "
                f"gate cannot analyse - refused rather than trusted",
            )
        )

    return [f"line {line}: {why}" for line, why in sorted(found)]


# This guard's own falsification set, replayed on every run rather than
# asserted in prose. ``_GATE_CONTROL_SOURCE`` is a miniature of the real
# two-gate shape and must be ACCEPTED - without it, a guard that had simply
# broken and started rejecting everything would read as a pass. Every entry in
# ``_GATE_EVASION_SOURCES`` was OBSERVED passing the name-keyed guard this one
# replaces.

_GATE_CONTROL_SOURCE = '''
def ordered_shutdown_completed(result, shutdown_record):
    return True


def run_stage(stage):
    shutdown = coordinator.shutdown_launcher(success_verdict="PASSED")
    shutdown_record = _load_shutdown_payload(coordinator.evidence_path)
    shutdown_ok = ordered_shutdown_completed(shutdown, shutdown_record)
    return 0 if shutdown_ok else 1


def _run_preflight_only():
    shutdown_ok = False
    try:
        pass
    finally:
        shutdown_result = coordinator.shutdown_launcher(success_verdict="PASSED")
        shutdown_record = _load_shutdown_payload(coordinator.evidence_path)
        shutdown_ok = ordered_shutdown_completed(shutdown_result, shutdown_record)
    return 0 if shutdown_ok else 1
'''

# GRK-P3-116: the SAME correct gate, written with keyword arguments. It has
# to be ACCEPTED. Rejecting it turned the section 10 loop-breaker red with a
# message pointing at correct code, and the path of least resistance from
# there is to weaken the guard - which is how this defect family started.
_GATE_KEYWORD_CONTROL_SOURCE = '''
def ordered_shutdown_completed(result, shutdown_record):
    return True


def run_stage(stage):
    shutdown = coordinator.shutdown_launcher(success_verdict="PASSED")
    shutdown_record = _load_shutdown_payload(coordinator.evidence_path)
    shutdown_ok = ordered_shutdown_completed(
        result=shutdown, shutdown_record=shutdown_record
    )
    return 0 if shutdown_ok else 1


def _run_preflight_only():
    shutdown_ok = False
    try:
        pass
    finally:
        shutdown_result = coordinator.shutdown_launcher(success_verdict="PASSED")
        shutdown_record = _load_shutdown_payload(coordinator.evidence_path)
        shutdown_ok = ordered_shutdown_completed(
            result=shutdown_result, shutdown_record=shutdown_record
        )
    return 0 if shutdown_ok else 1
'''

_GATE_PLAIN_TRUE_SOURCE = _GATE_CONTROL_SOURCE.replace(
    "    shutdown_ok = False\n", "    shutdown_ok = True\n"
)

_GATE_SECOND_DECISION_INSIDE_RUN_STAGE = '''
def ordered_shutdown_completed(result, shutdown_record):
    return True


def run_stage(stage):
    shutdown = coordinator.shutdown_launcher(success_verdict="PASSED")
    shutdown_record = _load_shutdown_payload(coordinator.evidence_path)
    shutdown_ok = ordered_shutdown_completed(shutdown, shutdown_record)
    teardown_ok = bool(shutdown.get("success"))
    return 0 if (shutdown_ok or teardown_ok) else 1


def _run_preflight_only():
    shutdown_ok = False
    try:
        pass
    finally:
        shutdown_result = coordinator.shutdown_launcher(success_verdict="PASSED")
        shutdown_record = _load_shutdown_payload(coordinator.evidence_path)
        shutdown_ok = ordered_shutdown_completed(shutdown_result, shutdown_record)
    return 0 if shutdown_ok else 1
'''

_GATE_EVASION_SOURCES: tuple[tuple[str, str], ...] = (
    (
        "a shutdown result bound to another name and truth-tested",
        _GATE_CONTROL_SOURCE
        + '''

def _run_teardown_only():
    res = coordinator.shutdown_launcher(success_verdict="PASSED")
    teardown_ok = bool(res.get("success"))
    return 0 if teardown_ok else 1
''',
    ),
    (
        "a fail-OPEN annotated initialiser: shutdown_ok: bool = True",
        _GATE_CONTROL_SOURCE.replace(
            "    shutdown_ok = False\n", "    shutdown_ok: bool = True\n"
        ),
    ),
    (
        "a second, renamed shutdown decision inside run_stage",
        _GATE_SECOND_DECISION_INSIDE_RUN_STAGE,
    ),
    (
        "the inline GRK-P3-110 shape, never bound to a name at all",
        _GATE_CONTROL_SOURCE
        + '''

def _run_teardown_only():
    if coordinator.shutdown_launcher().get("success"):
        return 0
    return 1
''',
    ),
    (
        "an alias that launders the result before it is truth-tested",
        _GATE_CONTROL_SOURCE
        + '''

def _run_teardown_only():
    res = coordinator.shutdown_launcher(success_verdict="PASSED")
    laundered = res
    if laundered:
        return 0
    return 1
''',
    ),
    (
        "a gate calling graceful_shutdown_owned_session around the wrapper",
        _GATE_CONTROL_SOURCE
        + '''

def _run_teardown_only():
    res = graceful_shutdown_owned_session(process=None)
    return 0 if res.get("success") else 1
''',
    ),
    (
        "a fail-OPEN augmented binding: shutdown_ok |= <expr>",
        _GATE_CONTROL_SOURCE.replace(
            "    shutdown_ok = False\n",
            "    shutdown_ok = False\n"
            '    shutdown_ok |= preflight_verdict == "PASSED"\n',
        ),
    ),
    (
        "a fail-OPEN walrus binding: if (shutdown_ok := True):",
        _GATE_CONTROL_SOURCE.replace(
            "    shutdown_ok = False\n",
            "    shutdown_ok = False\n"
            "    if (shutdown_ok := True):\n"
            "        pass\n",
        ),
    ),
    (
        "a fail-OPEN tuple unpack: shutdown_ok, _x = True, None",
        _GATE_CONTROL_SOURCE.replace(
            "    shutdown_ok = False\n",
            "    shutdown_ok = False\n"
            "    shutdown_ok, _x = True, None\n",
        ),
    ),
    (
        "the shutdown result passed by KEYWORD to a non-predicate call",
        _GATE_CONTROL_SOURCE
        + '''

def _run_teardown_only():
    res = coordinator.shutdown_launcher(success_verdict="PASSED")
    return 0 if _teardown_looks_fine(result=res) else 1
''',
    ),
)


def test_every_shutdown_gate_asks_the_one_shared_predicate() -> None:
    """§10 loop-breaker: one predicate, every gate, or this test fails.

    GRK-P3-110 exists because ``run_stage`` and ``_run_preflight_only`` were
    fixed independently, and the orchestrator's own standing correction after
    it was: grep for the PREDICATE, not for the line being fixed. This test is
    that grep, enforced.

    WHAT IT ENFORCES, stated as a bounded claim rather than a universal one.
    A further gate fails here when it takes a shutdown result from
    ``shutdown_launcher`` at module or function scope and sends it anywhere but
    the shared predicate, whatever it renames that result to; when it calls
    ``graceful_shutdown_owned_session`` around the wrapper; or when it binds
    ``shutdown_ok`` as a STORE TARGET from anything but the predicate or the one
    fail-CLOSED literal-False initializer. The store-target rule is exhaustive
    over store targets rather than over a list of forms: the
    augmented, walrus, for-target, with-as, tuple-unpack and multi-target
    bindings the guard cannot analyse are REFUSED rather than passed over, which
    is the fail-CLOSED direction. A ``shutdown_ok`` function PARAMETER is exempt,
    by construction rather than by an allowlist - parameters are ``ast.arg`` and
    never store targets - and ``stage_verdict`` legitimately takes one.

    WHAT IT DOES NOT, named here so no reader trusts more than the code does.
    Two shapes stay outside it. A gate deriving its decision solely from the
    PERSISTED evidence block - the ``_load_shutdown_payload`` return rather
    than the launcher return - is not caught, and is deliberately not closed:
    tainting that read would flag the legitimate ``record_check`` detail
    argument at ``stress_coordinator.py`` lines 2463 and 2546, so widening it
    would report correct code as a violation. And a gate written in a CLASS
    body in two-statement form is not caught, because class bodies are not
    scopes the binding analysis enters; only its inline
    ``shutdown_launcher(...).get("success")`` form is, by the ``direct`` check
    below.

    The claim above was unqualified and false until GRK-P3-114 and GRK-P3-115.
    The guard keyed on the literal name ``shutdown_ok`` and on ``ast.Assign``,
    so the GRK-P3-110 shape with one identifier renamed, the annotated
    fail-OPEN form ``shutdown_ok: bool = True``, a second renamed decision
    inside ``run_stage``, a direct call to the frozen owned-session helper, and
    the augmented, walrus and tuple-unpack bindings of ``shutdown_ok`` all
    walked through it untouched - a gate writing a false premise into its own
    comment, which is the GRK-P3-111 defect shape one level up and has now
    been filed against this gate three times. ``_shutdown_gate_violations``
    refuses each of those shapes, its falsification set is replayed here on
    every run, and the mirror defect GRK-P3-116 - the guard REJECTING the
    correct gate written with keyword arguments - is held down by a second
    ACCEPTED control rather than by the absence of anyone trying it.
    """

    from tests.gui.part3 import stress_coordinator as module

    source = COORDINATOR.read_text(encoding="utf-8")
    tree = ast.parse(source, filename=str(COORDINATOR))

    defined = [
        node.name
        for node in ast.walk(tree)
        if isinstance(node, ast.FunctionDef) and node.name == SHARED_SHUTDOWN_PREDICATE
    ]
    assert defined == [SHARED_SHUTDOWN_PREDICATE], defined

    call_gates: list[tuple[int, str | None]] = []
    constant_gates: list[tuple[int, Any]] = []
    for node in ast.walk(tree):
        if not isinstance(node, ast.Assign):
            continue
        if not any(
            isinstance(target, ast.Name) and target.id == "shutdown_ok"
            for target in node.targets
        ):
            continue
        if isinstance(node.value, ast.Constant):
            constant_gates.append((node.lineno, node.value.value))
        elif isinstance(node.value, ast.Call):
            call_gates.append((node.lineno, _called_entrypoint_name(node.value)))
        else:
            call_gates.append((node.lineno, ast.dump(node.value)))

    assert len(call_gates) >= 2, call_gates
    assert all(name == SHARED_SHUTDOWN_PREDICATE for _line, name in call_gates), (
        call_gates
    )
    # An initialiser is allowed only in the fail-CLOSED direction.
    assert all(value is False for _line, value in constant_gates), constant_gates

    callers = sorted(
        node.name
        for node in ast.walk(tree)
        if isinstance(node, ast.FunctionDef)
        and any(
            isinstance(inner, ast.Call)
            and _called_entrypoint_name(inner) == SHARED_SHUTDOWN_PREDICATE
            for inner in ast.walk(node)
        )
    )
    assert callers == ["_run_preflight_only", "run_stage"], callers

    # The wrapper cannot be bypassed, and it also still exists: exactly one
    # call to the frozen owned-session helper, from ``shutdown_launcher``
    # alone. The taint analysis keys on that one name, so a second caller
    # would be a shutdown decision the guard never sees (GRK-P3-115).
    owned_session_calls = _owned_session_callers(tree)
    assert [name for _line, name in owned_session_calls] == [
        SHUTDOWN_ENTRYPOINT
    ], owned_session_calls

    # The exact shape GRK-P3-110 was: a shutdown result consumed straight
    # through ``.get("success")`` without the persisted block being read.
    direct = [
        node.lineno
        for node in ast.walk(tree)
        if isinstance(node, ast.Call)
        and isinstance(node.func, ast.Attribute)
        and node.func.attr == "get"
        and isinstance(node.func.value, ast.Call)
        and _called_entrypoint_name(node.func.value) == "shutdown_launcher"
    ]
    assert direct == [], direct

    # Everything above keys on the literal name ``shutdown_ok`` and on
    # ``ast.Assign``. That is the LINE, not the predicate: renaming one
    # identifier walked around it, and so did the annotated form
    # (GRK-P3-114). This is the name-agnostic half - a shutdown result must
    # reach the shared predicate and nothing else, whatever it is called.
    violations = _shutdown_gate_violations(source)
    assert violations == [], violations

    # ...and this guard is falsifiable, demonstrated rather than claimed.
    # The control must be ACCEPTED - without it, a guard that had simply
    # broken and started rejecting everything would read as a pass here -
    # and every evasion must be REJECTED.
    control = _shutdown_gate_violations(_GATE_CONTROL_SOURCE)
    assert control == [], control
    # GRK-P3-116: the same gate in keyword form is correct code and must be
    # ACCEPTED. Asserted alongside the evasions below, never instead of them,
    # so the over-fire cannot be closed by weakening the under-fire.
    keyword_control = _shutdown_gate_violations(_GATE_KEYWORD_CONTROL_SOURCE)
    assert keyword_control == [], keyword_control
    plain_true = _shutdown_gate_violations(_GATE_PLAIN_TRUE_SOURCE)
    assert plain_true, plain_true
    for label, evasion in _GATE_EVASION_SOURCES:
        assert _shutdown_gate_violations(evasion), label

    # And the predicate itself is an allowlist, not a denylist: a record whose
    # markers are all absent is still refused when a transition is missing.
    complete = empty_shutdown_record()
    for key in SHUTDOWN_TIMESTAMP_KEYS:
        stamp_shutdown_transition(complete, key)
    success = {"success": True, "forced": False, "stalled_stage": None}
    assert module.ordered_shutdown_completed(success, complete) is True
    for key in SHUTDOWN_TIMESTAMP_KEYS:
        missing = dict(complete)
        missing[key] = None
        assert module.ordered_shutdown_completed(success, missing) is False, key
    assert module.ordered_shutdown_completed({"success": False}, complete) is False
    forced = dict(complete)
    forced["forced"] = True
    assert module.ordered_shutdown_completed(success, forced) is False
    for malformed in ("not-a-timestamp", "2026-08-25T00:00:00", "2026-08-25T00:00:00+01:00"):
        invalid = dict(complete)
        invalid["documents_closed_utc"] = malformed
        assert module.ordered_shutdown_completed(success, invalid) is False, malformed
    reversed_order = dict(complete)
    reversed_order["requested_utc"] = "2026-08-25T00:00:01+00:00"
    reversed_order["documents_closed_utc"] = "2026-08-25T00:00:00+00:00"
    assert module.ordered_shutdown_completed(success, reversed_order) is False
