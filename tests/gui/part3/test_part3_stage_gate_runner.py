# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline launch/provenance packet gates for GRK-P3-123."""

from __future__ import annotations

import hashlib
import json
import os
import shutil
import subprocess
import sys
import tempfile
import zipfile
from copy import deepcopy
from pathlib import Path
from typing import Any

import pytest

from tests.gui.part3.evidence import (
    ALPHA_MODEL_KEY,
    ALPHA_PROPERTY_KEY,
    BETA_MODEL_KEY,
    BETA_PROPERTY_KEY,
    stage_revision_vector_is_exact,
)

REPO_ROOT = Path(__file__).resolve().parents[3]
RUNNER = Path(__file__).with_name("stage_gate_runner.py")
ORCHESTRATION_TRACKER = REPO_ROOT / "doc" / "part3-orchestrated-review-fix-test-plan.md"
_FIXTURE_DOCUMENTS = tempfile.TemporaryDirectory(prefix="part3-stage-fcstd-")


def _personal_revision_vector(base_revision: int) -> list[dict[str, int | str]]:
    """Build the exact ordered vector observed by the live personal driver."""

    return [
        {"key": ALPHA_PROPERTY_KEY, "revision": base_revision},
        {"key": BETA_PROPERTY_KEY, "revision": base_revision + 1},
        {"key": ALPHA_MODEL_KEY, "revision": base_revision + 2},
        {"key": BETA_MODEL_KEY, "revision": base_revision + 3},
    ]


def test_luna_snapshot_contract_requires_all_candidates_and_two_binaries() -> None:
    from tests.gui.part3.stage_gate_runner import LUNA_SNAPSHOT_CANDIDATE_PATHS

    assert len(LUNA_SNAPSHOT_CANDIDATE_PATHS) == 14
    assert "doc/document-collaboration-completion-progress.md" in LUNA_SNAPSHOT_CANDIDATE_PATHS
    assert len(set(LUNA_SNAPSHOT_CANDIDATE_PATHS)) == 14
    assert all((REPO_ROOT / relative).is_file() for relative in LUNA_SNAPSHOT_CANDIDATE_PATHS)


def _snapshot_expectations() -> tuple[dict[str, str], dict[str, list[str]], dict[str, str]]:
    from tests.gui.part3.stage_gate_runner import LUNA_SNAPSHOT_CANDIDATE_PATHS

    paths = [*LUNA_SNAPSHOT_CANDIDATE_PATHS, "build/release/bin/FreeCAD.exe", "build/release/bin/FreeCADCmd.exe"]
    return (
        {
            "parent_head": "a" * 40,
            "parent_branch": "fix/cc-wp00",
            "parent_upstream": "a" * 40,
            "parent_log": "a" * 40 + " 0",
            "nested_head": "b" * 40,
            "nested_branch": "main",
        },
        {"parent": [" M tests/gui/part3/evidence.py"], "nested": []},
        {path: f"{index:064x}" for index, path in enumerate(paths, start=1)},
    )


def test_snapshot_qualification_receipt_fails_closed_on_every_frozen_identity() -> None:
    from tests.gui.part3.stage_gate_runner import validate_snapshot_qualification_receipt

    references, statuses, hashes = _snapshot_expectations()
    receipt = {
        "command": ["bash", "snapshot/qualify.sh"], "return_code": 0,
        "references": dict(references), "statuses": deepcopy(statuses),
        "nested_diff_bytes": 0, "hashes": dict(hashes), "imports_pass": True,
    }
    validate_snapshot_qualification_receipt(
        receipt, expected_references=references, expected_statuses=statuses,
        expected_nested_diff_bytes=0, expected_hashes=hashes,
        expected_command=["bash", "snapshot/qualify.sh"],
    )
    mutations = {
        "reference": lambda value: value["references"].__setitem__("parent_head", "c" * 40),
        "status": lambda value: value["statuses"].__setitem__("parent", []),
        "nested_diff": lambda value: value.__setitem__("nested_diff_bytes", 1),
        "hash": lambda value: value["hashes"].__setitem__(next(iter(hashes)), "d" * 64),
    }
    for mutate in mutations.values():
        corrupted = deepcopy(receipt)
        mutate(corrupted)
        with pytest.raises(ValueError):
            validate_snapshot_qualification_receipt(
                corrupted, expected_references=references, expected_statuses=statuses,
                expected_nested_diff_bytes=0, expected_hashes=hashes,
                expected_command=["bash", "snapshot/qualify.sh"],
            )
    for defect in ("extra_key", "command", "expected_nonzero_diff", "arbitrary_paths", "receipt_hash_extra", "receipt_hash_missing"):
        corrupted = deepcopy(receipt)
        kwargs: dict[str, Any] = {"expected_nested_diff_bytes": 0, "expected_hashes": hashes}
        if defect == "extra_key":
            corrupted["unexpected"] = True
        elif defect == "command":
            corrupted["command"] = ["echo", "green"]
        elif defect == "expected_nonzero_diff":
            kwargs["expected_nested_diff_bytes"] = 1
        elif defect == "arbitrary_paths":
            kwargs["expected_hashes"] = {f"arbitrary/{index}": digest for index, digest in enumerate(hashes.values())}
        elif defect == "receipt_hash_extra":
            corrupted["hashes"]["unexpected"] = "f" * 64
        else:
            corrupted["hashes"].pop(next(iter(corrupted["hashes"])))
        with pytest.raises(ValueError):
            validate_snapshot_qualification_receipt(corrupted, expected_references=references, expected_statuses=statuses, expected_command=["bash", "snapshot/qualify.sh"], **kwargs)


def test_captured_snapshot_qualification_binds_copied_bytes_and_completed_commands(
    tmp_path: Path
) -> None:
    from tests.gui.part3.stage_gate_runner import (
        LUNA_SNAPSHOT_CANDIDATE_PATHS,
        SNAPSHOT_GIT_COMMANDS,
        validate_captured_snapshot_qualification,
    )

    paths = [*LUNA_SNAPSHOT_CANDIDATE_PATHS, "build/release/bin/FreeCAD.exe", "build/release/bin/FreeCADCmd.exe"]
    hashes: dict[str, str] = {}
    for index, relative in enumerate(paths):
        target = tmp_path.joinpath(*relative.split("/"))
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(f"snapshot-{index}".encode())
        hashes[relative] = hashlib.sha256(target.read_bytes()).hexdigest()
    git_results = {
        "parent_head": {"argv": SNAPSHOT_GIT_COMMANDS["parent_head"], "return_code": 0, "stdout": "a" * 40 + "\n", "stderr": ""},
        "parent_branch": {"argv": SNAPSHOT_GIT_COMMANDS["parent_branch"], "return_code": 0, "stdout": "fix/cc-wp00\n", "stderr": ""},
        "parent_upstream": {"argv": SNAPSHOT_GIT_COMMANDS["parent_upstream"], "return_code": 0, "stdout": "origin/fix/cc-wp00\n", "stderr": ""},
        "parent_status": {"argv": SNAPSHOT_GIT_COMMANDS["parent_status"], "return_code": 0, "stdout": " M tests/gui/part3/stage_gate_runner.py\n", "stderr": ""},
        "parent_gitlink": {"argv": SNAPSHOT_GIT_COMMANDS["parent_gitlink"], "return_code": 0, "stdout": "160000 b 0\ttools/mcp/freecad-mcp\n", "stderr": ""},
        "nested_head": {"argv": SNAPSHOT_GIT_COMMANDS["nested_head"], "return_code": 0, "stdout": "b" * 40 + "\n", "stderr": ""},
        "nested_branch": {"argv": SNAPSHOT_GIT_COMMANDS["nested_branch"], "return_code": 0, "stdout": "main\n", "stderr": ""},
        "nested_upstream": {"argv": SNAPSHOT_GIT_COMMANDS["nested_upstream"], "return_code": 0, "stdout": "origin/main\n", "stderr": ""},
        "nested_status": {"argv": SNAPSHOT_GIT_COMMANDS["nested_status"], "return_code": 0, "stdout": "", "stderr": ""},
        "nested_diff": {"argv": SNAPSHOT_GIT_COMMANDS["nested_diff"], "return_code": 0, "stdout": "", "stderr": ""},
    }
    git_contract = _write(
        tmp_path / "frozen-git-results.json", json.dumps(git_results, sort_keys=True) + "\n"
    )
    receipt = {
        "hashes": dict(hashes),
        "git_commands": deepcopy(git_results),
        "imports": {"argv": ["python", "-B", "-c", "import tests.gui.part3.evidence"], "return_code": 0, "stdout": "IMPORTS_PASS\n", "stderr": ""},
    }
    kwargs = {"snapshot_root": tmp_path, "expected_hashes": hashes, "frozen_git_result_contract": git_contract, "expected_import_command": receipt["imports"]["argv"], "expected_import_stdout": "IMPORTS_PASS\n"}
    validate_captured_snapshot_qualification(receipt, **kwargs)
    target = tmp_path.joinpath(*paths[0].split("/"))
    target.write_bytes(b"hardcoded-but-wrong")
    with pytest.raises(ValueError, match="bytes"):
        validate_captured_snapshot_qualification(receipt, **kwargs)
    target.write_bytes(b"snapshot-0")
    receipt["git_commands"]["parent_head"]["return_code"] = 1
    with pytest.raises(ValueError, match="Git"):
        validate_captured_snapshot_qualification(receipt, **kwargs)
    receipt["git_commands"]["parent_head"]["return_code"] = 0
    for mutate in (
        lambda value: value["git_commands"]["parent_head"].__setitem__("stdout", "arbitrary-green\n"),
        lambda value: value["git_commands"]["parent_status"].__setitem__("stdout", "clean-but-wrong\n"),
        lambda value: value["git_commands"]["nested_diff"].__setitem__("stdout", "nonempty-diff\n"),
        lambda value: value["git_commands"].pop("nested_upstream"),
        lambda value: value["git_commands"].pop("parent_gitlink"),
    ):
        corrupted = deepcopy(receipt)
        mutate(corrupted)
        with pytest.raises(ValueError, match="Git"):
            validate_captured_snapshot_qualification(corrupted, **kwargs)
    for mutate in (
        lambda value: value["parent_head"].__setitem__("argv", ["echo", "green"]),
        lambda value: value["parent_head"].__setitem__("argv", value["parent_status"]["argv"]),
    ):
        self_confirming = deepcopy(git_results)
        mutate(self_confirming)
        self_confirming_contract = _write(
            tmp_path / f"self-confirming-{len(self_confirming['parent_head']['argv'])}.json",
            json.dumps(self_confirming, sort_keys=True) + "\n",
        )
        corrupted = deepcopy(receipt)
        corrupted["git_commands"] = deepcopy(self_confirming)
        with pytest.raises(ValueError, match="contract"):
            validate_captured_snapshot_qualification(
                corrupted, **(kwargs | {"frozen_git_result_contract": self_confirming_contract})
            )
    changed_contract = tmp_path / "changed-git-results.json"
    changed_contract.write_text(json.dumps(git_results | {"nested_diff": git_results["nested_diff"] | {"stdout": "unexpected\n"}}) + "\n", encoding="utf-8")
    with pytest.raises(ValueError, match="identity"):
        validate_captured_snapshot_qualification(
            receipt, **(kwargs | {"frozen_git_result_contract": {**git_contract, "path": str(changed_contract)}})
        )
    receipt["imports"]["stdout"] = "IMPORTS_PASS\n"  # a hardcoded success is not command output
    receipt["imports"]["return_code"] = 1
    with pytest.raises(ValueError, match="import"):
        validate_captured_snapshot_qualification(receipt, **kwargs)


def test_derived_final_seal_refuses_unasserted_resource_claims(tmp_path: Path) -> None:
    from tests.gui.part3.stage_gate_runner import (
        FINAL_AFTERMATH_KEYS,
        build_derived_final_seal,
    )

    expected: dict[str, bool | int] = {
        "owned_snapshot_volume_absent": True, "owned_volume_users": 0,
        "active_containers": 0, "retained_stopped_containers": 55,
        "general_volumes_preserved": True, "target_processes": 0,
        "target_listeners": 0,
    }
    commands = _resource_commands("cc-wp00-owned-snapshot")
    volumes = [f"cc-wp00-baseline-{index:02}" for index in range(13)]
    stopped = [{"id": f"{index:012x}", "state": "exited"} for index in range(55)]
    contract = _aftermath_resource_contract(tmp_path, commands, volumes, stopped)
    git_contract = _frozen_git_result_contract(tmp_path / "seal-git-contract")
    receipts = _aftermath_receipts(tmp_path, expected, commands, volumes, stopped)
    manifest = {"path": "closeout/final-manifest.json", "sha256": "e" * 64, "size": 1, "entry_count": 1}
    seal = build_derived_final_seal(
        manifest, aftermath_receipts=receipts, expected_aftermath=expected,
        aftermath_resource_contract=contract, snapshot_git_result_contract=git_contract,
    )
    assert seal["aftermath"] == expected
    assert tuple(seal["aftermath"]) == FINAL_AFTERMATH_KEYS
    assert seal["aftermath_receipts"] == receipts
    assert seal["snapshot_git_result_contract"] == git_contract
    for key, command in (
        ("active_containers", ["echo", "[]"]),
        ("active_containers", commands["general_volumes_preserved"]),
        ("owned_volume_users", ["docker", "ps", "-a", "--filter", "volume=wrong", "--format", "{{.ID}}"]),
        ("target_processes", ["pwsh", "-NoProfile", "-File", "closeout/not-target.ps1"]),
    ):
        malformed_commands = deepcopy(commands)
        malformed_commands[key] = command
        malformed_contract = _aftermath_resource_contract(
            tmp_path / f"malformed-{key}", malformed_commands, volumes, stopped
        )
        with pytest.raises(ValueError, match="canonical direct probe"):
            build_derived_final_seal(
                manifest, aftermath_receipts=receipts, expected_aftermath=expected,
                aftermath_resource_contract=malformed_contract,
                snapshot_git_result_contract=git_contract,
            )
    for key in FINAL_AFTERMATH_KEYS:
        corrupted = deepcopy(receipts)
        raw = json.loads(Path(receipts[key]["path"]).read_text(encoding="utf-8"))
        # A self-authored observation cannot turn the raw command receipt green.
        raw["observed"] = expected[key]
        corrupted[key] = _write(tmp_path / f"{key}-fabricated.json", json.dumps(raw) + "\n")
        with pytest.raises(ValueError):
            build_derived_final_seal(
                manifest, aftermath_receipts=corrupted, expected_aftermath=expected,
                aftermath_resource_contract=contract, snapshot_git_result_contract=git_contract,
            )
    for field, value in (("underlying_return_code", 1), ("argv", ["echo", "green"])):
        corrupted = deepcopy(receipts)
        raw = json.loads(Path(receipts["target_processes"]["path"]).read_text(encoding="utf-8"))
        raw[field] = value
        corrupted["target_processes"] = _write(tmp_path / f"target-processes-{field}.json", json.dumps(raw) + "\n")
        with pytest.raises(ValueError):
            build_derived_final_seal(
                manifest, aftermath_receipts=corrupted, expected_aftermath=expected,
                aftermath_resource_contract=contract, snapshot_git_result_contract=git_contract,
            )
    corrupted = deepcopy(receipts)
    raw = json.loads(Path(receipts["owned_snapshot_volume_absent"]["path"]).read_text(encoding="utf-8"))
    raw["stderr"] = "NO_SUCH_VOLUME\n"  # receipt-runner-style sanitized substitution
    corrupted["owned_snapshot_volume_absent"] = _write(tmp_path / "sanitized-volume.json", json.dumps(raw) + "\n")
    with pytest.raises(ValueError, match="exact natural"):
        build_derived_final_seal(manifest, aftermath_receipts=corrupted, expected_aftermath=expected, aftermath_resource_contract=contract, snapshot_git_result_contract=git_contract)
    for field, value in (("stdout", ""), ("stdout", "[]\nextra\n"), ("target", "wrong-volume")):
        corrupted = deepcopy(receipts)
        raw = json.loads(Path(receipts["owned_snapshot_volume_absent"]["path"]).read_text(encoding="utf-8"))
        raw[field] = value
        corrupted["owned_snapshot_volume_absent"] = _write(tmp_path / f"volume-{field}-{len(value)}.json", json.dumps(raw) + "\n")
        with pytest.raises(ValueError, match="exact natural"):
            build_derived_final_seal(manifest, aftermath_receipts=corrupted, expected_aftermath=expected, aftermath_resource_contract=contract, snapshot_git_result_contract=git_contract)
    corrupted = deepcopy(receipts)
    raw = json.loads(Path(receipts["target_listeners"]["path"]).read_text(encoding="utf-8"))
    raw["stdout"] = "COUNT=1\n"
    corrupted["target_listeners"] = _write(tmp_path / "listeners-raw-mutation.json", json.dumps(raw) + "\n")
    with pytest.raises(ValueError):
        build_derived_final_seal(manifest, aftermath_receipts=corrupted, expected_aftermath=expected, aftermath_resource_contract=contract, snapshot_git_result_contract=git_contract)
    for key, field, value in (
        ("active_containers", "stderr", "unexpected diagnostic\n"),
        ("target_processes", "wrapper_return_code", 1),
        ("target_listeners", "stdout", "[\"listener\"]\n"),
    ):
        corrupted = deepcopy(receipts)
        raw = json.loads(Path(receipts[key]["path"]).read_text(encoding="utf-8"))
        raw[field] = value
        corrupted[key] = _write(tmp_path / f"{key}-{field}.json", json.dumps(raw) + "\n")
        with pytest.raises(ValueError):
            build_derived_final_seal(manifest, aftermath_receipts=corrupted, expected_aftermath=expected, aftermath_resource_contract=contract, snapshot_git_result_contract=git_contract)
    for stdout in (
        "PRESERVED=true\n",
        "".join(f"{name}\n" for name in volumes[:-1] + ["cc-wp00-replaced"]),
        "".join(f"{name}\n" for name in volumes[:-1] + [volumes[-2]]),
    ):
        corrupted = deepcopy(receipts)
        raw = json.loads(Path(receipts["general_volumes_preserved"]["path"]).read_text(encoding="utf-8"))
        raw["stdout"] = stdout
        corrupted["general_volumes_preserved"] = _write(
            tmp_path / f"volume-list-{hashlib.sha256(stdout.encode()).hexdigest()}.json",
            json.dumps(raw) + "\n",
        )
        with pytest.raises(ValueError, match="general-volume"):
            build_derived_final_seal(manifest, aftermath_receipts=corrupted, expected_aftermath=expected, aftermath_resource_contract=contract, snapshot_git_result_contract=git_contract)
    corrupted = deepcopy(receipts)
    raw = json.loads(Path(receipts["retained_stopped_containers"]["path"]).read_text(encoding="utf-8"))
    raw["stdout"] = json.dumps(stopped[:-1] + [{"id": stopped[-1]["id"], "state": "running"}]) + "\n"
    corrupted["retained_stopped_containers"] = _write(
        tmp_path / "stopped-container-state.json", json.dumps(raw) + "\n"
    )
    with pytest.raises(ValueError, match="stopped-container"):
        build_derived_final_seal(manifest, aftermath_receipts=corrupted, expected_aftermath=expected, aftermath_resource_contract=contract, snapshot_git_result_contract=git_contract)
    changed_contract = tmp_path / "changed-resource-contract.json"
    changed_contract.write_text("{}\n", encoding="utf-8")
    with pytest.raises(ValueError, match="identity"):
        build_derived_final_seal(
            manifest, aftermath_receipts=receipts, expected_aftermath=expected,
            aftermath_resource_contract={**contract, "path": str(changed_contract)},
            snapshot_git_result_contract=git_contract,
        )
    corrupted = deepcopy(receipts)
    corrupted["target_processes"]["sha256"] = "f" * 64
    with pytest.raises(ValueError):
        build_derived_final_seal(manifest, aftermath_receipts=corrupted, expected_aftermath=expected, aftermath_resource_contract=contract, snapshot_git_result_contract=git_contract)
    for mutate in (lambda value: value.pop("target_listeners"), lambda value: value.__setitem__("extra", {})):
        corrupted = deepcopy(receipts)
        mutate(corrupted)
        with pytest.raises(ValueError):
            build_derived_final_seal(manifest, aftermath_receipts=corrupted, expected_aftermath=expected, aftermath_resource_contract=contract, snapshot_git_result_contract=git_contract)


@pytest.mark.parametrize("platform", ["windows", "linux"])
def test_platform_packet_resource_and_snapshot_contracts_fail_closed(
    tmp_path: Path, platform: str
) -> None:
    """Exercise the same frozen receipt contracts carried by each host packet."""

    from tests.gui.part3.stage_gate_runner import (
        LUNA_SNAPSHOT_CANDIDATE_PATHS,
        SNAPSHOT_GIT_COMMANDS,
        SNAPSHOT_GIT_RESULT_KEYS,
        validate_captured_snapshot_qualification,
        validate_final_resource_proof,
    )

    snapshot_root = tmp_path / platform / "snapshot"
    hashes: dict[str, str] = {}
    paths = [
        *LUNA_SNAPSHOT_CANDIDATE_PATHS,
        "build/release/bin/FreeCAD.exe", "build/release/bin/FreeCADCmd.exe",
    ]
    for index, relative in enumerate(paths):
        path = snapshot_root.joinpath(*relative.split("/"))
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(f"{platform}-snapshot-{index}".encode())
        hashes[relative] = hashlib.sha256(path.read_bytes()).hexdigest()
    git_results = {
        key: {
            "argv": list(SNAPSHOT_GIT_COMMANDS[key]), "return_code": 0,
            "stdout": "" if key in {"nested_status", "nested_diff"} else f"{platform}-{key}\n",
            "stderr": "",
        }
        for key in SNAPSHOT_GIT_RESULT_KEYS
    }
    git_contract = _write(
        tmp_path / platform / "frozen-git.json", json.dumps(git_results, sort_keys=True) + "\n"
    )
    expected: dict[str, bool | int] = {
        "owned_snapshot_volume_absent": True, "owned_volume_users": 0,
        "active_containers": 0, "retained_stopped_containers": 0,
        "general_volumes_preserved": True, "target_processes": 0,
        "target_listeners": 0,
    }
    commands = _resource_commands(f"{platform}-owned")
    volumes = [f"{platform}-baseline-{index:02}" for index in range(13)]
    resource_contract = _aftermath_resource_contract(
        tmp_path / platform, commands, volumes, []
    )
    packet = {
        "platform": platform,
        "snapshot": {
            "hashes": hashes, "git_commands": deepcopy(git_results),
            "imports": {"argv": ["python", "-B", "-c", "import package"], "return_code": 0, "stdout": "OK\n", "stderr": ""},
        },
        "final_resource": _aftermath_receipts(
            tmp_path / platform, expected, commands, volumes, []
        ),
    }
    snapshot_kwargs = {
        "snapshot_root": snapshot_root, "expected_hashes": hashes,
        "frozen_git_result_contract": git_contract,
        "expected_import_command": packet["snapshot"]["imports"]["argv"],
        "expected_import_stdout": "OK\n",
    }
    validate_captured_snapshot_qualification(packet["snapshot"], **snapshot_kwargs)
    validate_final_resource_proof(
        packet["final_resource"], expected=expected,
        frozen_resource_contract=resource_contract,
    )
    corrupted = deepcopy(packet)
    corrupted["snapshot"]["git_commands"]["nested_upstream"]["stdout"] = "arbitrary\n"
    with pytest.raises(ValueError, match="Git"):
        validate_captured_snapshot_qualification(corrupted["snapshot"], **snapshot_kwargs)
    for key, stream, value in (
        ("general_volumes_preserved", "stdout", "".join(f"{name}\n" for name in volumes[:-1] + [f"{platform}-replacement"])),
        ("target_processes", "wrapper_return_code", 1),
        ("target_listeners", "stdout", "[\"listener\"]\n"),
    ):
        corrupted = deepcopy(packet)
        raw = json.loads(Path(packet["final_resource"][key]["path"]).read_text(encoding="utf-8"))
        raw[stream] = value
        corrupted["final_resource"][key] = _write(
            tmp_path / platform / f"{key}-{stream}.json", json.dumps(raw) + "\n"
        )
        with pytest.raises(ValueError):
            validate_final_resource_proof(
                corrupted["final_resource"], expected=expected,
                frozen_resource_contract=resource_contract,
            )


def _write(path: Path, content: str) -> dict[str, Any]:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(content, encoding="utf-8", newline="\n")
    data = path.read_bytes()
    return {
        "path": str(path.resolve()),
        "sha256": hashlib.sha256(data).hexdigest(),
        "size": len(data),
    }


def _write_minimal_fcstd(path: Path, label: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, "w") as archive:
        archive.writestr("Document.xml", f"<Document label={label!r}/>\n")


def _stage_evidence(
    stage: str, *, documents_root: Path | None = None
) -> dict[str, Any]:
    from tests.gui.part3.evidence import (
        ALPHA_MODEL_KEY, ALPHA_OBJECT, ALPHA_PROPERTY_KEY,
        BETA_MODEL_KEY, BETA_OBJECT, BETA_PROPERTY, BETA_PROPERTY_KEY,
        file_fingerprint, git_state, sha256_file,
    )
    from tests.gui.part3.scenarios import COVERAGE_ITEMS, resolve_executable_stage
    from tests.gui.part3.stress_coordinator import (
        STAGE_SOURCE_FILES,
        _commit_history,
        _epoch_to_utc_iso,
    )

    definition = resolve_executable_stage(stage)
    fixture_binary = REPO_ROOT / "tests" / "gui" / "part3" / "stage_gate_runner.py"
    fixture_fingerprint = file_fingerprint(fixture_binary)
    fixture_git = git_state(REPO_ROOT)
    fixture_history = _commit_history(REPO_ROOT)
    fixture_missing = [
        commit for commit, epoch in fixture_history if epoch > fixture_binary.stat().st_mtime
    ]
    primary = f"Part3Stage{stage}Primarycafebabe"
    secondary = f"Part3Stage{stage}Secondarycafebabe"
    snapshot = {
        "observed_document": primary,
        "identity_selector": {
            "document_uid": "uid-primary",
            "document_instance_id": 1,
            "lifecycle_epoch": 1,
            "document_name": primary,
        },
        "file_change_state": {"pending_changes": [], "has_pending_file_changes": False},
        "semantic_revisions": _personal_revision_vector(10),
    }
    secondary_snapshot = {
        **snapshot,
        "observed_document": secondary,
        "identity_selector": {
            **snapshot["identity_selector"],
            "document_uid": "uid-secondary",
            "document_name": secondary,
        },
        "semantic_revisions": _personal_revision_vector(20),
    }
    checks = [
        "cycle_count_matches_stage_definition",
        "save_count_matches_stage_definition",
        "adr_section_13_coverage_complete",
        "every_save_cycle_is_truthful",
        "every_personal_action_has_exact_clean_revision_proof",
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
    ]
    cycles = []
    proofs = []
    local_schedule = ["set_active_document", "set_active_document", "rotate_camera", "pan_view", "zoom_view", "fit_all", "select_object", "expand_tree", "collapse_tree", "clear_selection"]
    for index in range(definition.view_mutation_cycles):
        document = primary if index % 2 == 0 else secondary
        other = secondary if document == primary else primary
        selector = dict(
            snapshot["identity_selector"]
            if document == primary
            else secondary_snapshot["identity_selector"]
        )
        operation_id = f"local-personal-{index}"
        commit_id = f"remote-commit-{index}"
        commit_result = {"success": True, "committed": True, "value": 100 + index}
        revisions = [{"key": BETA_PROPERTY_KEY, "revision": index + 1}]
        before_revisions = [{"key": ALPHA_PROPERTY_KEY, "revision": 1}, *revisions, {"key": ALPHA_MODEL_KEY, "revision": 1}, {"key": BETA_MODEL_KEY, "revision": 1}]
        after_revisions = [{"key": ALPHA_PROPERTY_KEY, "revision": 1}, {"key": BETA_PROPERTY_KEY, "revision": index + 2}, {"key": ALPHA_MODEL_KEY, "revision": 1}, {"key": BETA_MODEL_KEY, "revision": 1}]
        clean_state = {"pending_changes": [], "has_pending_file_changes": False}
        cycles.append(
            {
                "index": index,
                "checks": {
                    "personal_view_state_inert": True,
                    "personal_view_state_not_dirtying": True,
                    "typed_mutation_committed_once": True,
                    "camera_changed": True,
                },
                "document": document,
                "revisions_before": before_revisions,
                "revisions_after_personal_view": before_revisions,
                "revisions_after": after_revisions,
                "file_change_state_before": dict(clean_state),
                "file_change_state_after_personal_view": dict(clean_state),
                "file_change_state_after": dict(clean_state),
                "readiness": {"success": True, "documents": [{"document": document, "ready": True, "quarantined": False, "collaboration_poisoned": False}]},
                "typed_mutation": {"operation_id": commit_id, "revision_before": index + 1, "revision_after": index + 2, "expected_value": 100 + index, "landed_value": 100 + index, "first_result": commit_result, "replay_result": commit_result, "revisions_before": before_revisions, "revisions_after": after_revisions},
                "local_actions": [],
                "remote_actions": [{
                    "operation_id": f"remote-begin-{index}", "method": "begin_checked_edit", "parameters": {"doc_selector": selector, "revision_keys": [{"kind": "ObjectProperty", "subject": BETA_OBJECT, "property_name": BETA_PROPERTY}], "operation_id": f"remote-begin-{index}"}, "ack_utc": "2026-08-25T00:00:00+00:00", "result_envelope": {"success": True, "session_id": f"session-{index}"},
                }, {
                    "operation_id": commit_id,
                    "method": "commit_checked_property",
                    "parameters": {"session_id": f"session-{index}", "doc_selector": selector, "object_name": BETA_OBJECT, "property_name": BETA_PROPERTY, "value_type": "integer", "value": str(100 + index), "operation_id": commit_id},
                    "ack_utc": "2026-08-25T00:00:00+00:00",
                    "result_envelope": commit_result,
                }, {
                    "operation_id": commit_id,
                    "method": "commit_checked_property",
                    "parameters": {"session_id": f"session-{index}", "doc_selector": selector, "object_name": BETA_OBJECT, "property_name": BETA_PROPERTY, "value_type": "integer", "value": str(100 + index), "operation_id": commit_id},
                    "ack_utc": "2026-08-25T00:00:00+00:00",
                    "result_envelope": commit_result, "committed_once": True,
                }, {
                    "operation_id": f"remote-recompute-{index}", "method": "recompute_document",
                    "parameters": [document],
                    "ack_utc": "2026-08-25T00:00:00+00:00", "result_envelope": {"success": True},
                }],
            }
        )
        for action_index, action_name in enumerate(local_schedule):
            action_id = f"{operation_id}-{action_index}"
            proof_index = len(proofs)
            parameters = [
                {"document": other}, {"document": document},
                {"yaw": 7.0 + index, "pitch": 5.0 + (index % 7), "roll": 0.0},
                {"dx": 0.1 + (index % 5) * 0.05, "dy": 0.02, "dz": 0.0},
                {"direction": "in" if index % 2 == 0 else "out"}, {"factor": 1.0},
                {"document": document, "object": ALPHA_OBJECT}, {"document": document, "object": ALPHA_OBJECT},
                {"document": document, "object": ALPHA_OBJECT}, {},
            ][action_index]
            documents = [primary, secondary] if action_name == "set_active_document" else [document]
            snapshots = {primary: snapshot, secondary: secondary_snapshot}
            observed = (
                {"active_document": parameters["document"]} if action_name == "set_active_document" else
                {"camera_orientation": "rotation"} if action_name == "rotate_camera" else
                {"view_position": "placement"} if action_name == "pan_view" else
                {"direction": parameters["direction"]} if action_name == "zoom_view" else
                {"factor": parameters["factor"]} if action_name == "fit_all" else
                {"selection_count": 1, "selected_object": ALPHA_OBJECT} if action_name == "select_object" else
                {**parameters, "mod": 2 if action_name == "expand_tree" else 1} if action_name in {"expand_tree", "collapse_tree"} else
                {"selection_count": 0}
            )
            cycles[-1]["local_actions"].append({"operation_id": action_id, "action": action_name, "parameters": parameters, "ack_utc": "2026-08-25T00:00:00+00:00", "observed": observed, "personal_action_proof_index": proof_index})
            proofs.append({"index": proof_index, "operation_id": action_id, "action": action_name, "documents": documents, "before": {name: deepcopy(snapshots[name]) for name in documents}, "after": {name: deepcopy(snapshots[name]) for name in documents}, "left_document": document if action_name == "set_active_document" else None, "activated_document": parameters.get("document") if action_name == "set_active_document" else None, "clean_before": True, "clean_after": True, "semantic_revisions_unchanged": True, "passed": True})
    root = documents_root or (Path(_FIXTURE_DOCUMENTS.name) / stage.lower())
    canonical_path = root / f"{primary}.FCStd"
    secondary_path = root / f"{secondary}.FCStd"
    copy_paths = [
        root / "copies" / f"{primary}-copy-{index}.FCStd"
        for index in range(definition.save_cycles)
    ]
    for index, copy_path in enumerate(copy_paths):
        _write_minimal_fcstd(copy_path, f"{stage}-save-{index}")
    shutil.copy2(copy_paths[-1], canonical_path)
    _write_minimal_fcstd(secondary_path, f"{stage}-secondary")
    copy_hashes = [hashlib.sha256(path.read_bytes()).hexdigest() for path in copy_paths]
    final_canonical_hash = hashlib.sha256(canonical_path.read_bytes()).hexdigest()
    secondary_hash = hashlib.sha256(secondary_path.read_bytes()).hexdigest()
    saves = []
    for index in range(definition.save_cycles):
        before = (
            hashlib.sha256(f"{stage}-initial".encode("utf-8")).hexdigest()
            if index == 0
            else copy_hashes[index - 1]
        )
        after = copy_hashes[index]
        copy = str(copy_paths[index])
        written_result = {"saved": True, "save_disposition": "written", "file_written": True, "durability_verified": True}
        unchanged_result = {"unchanged": True, "save_disposition": "unchanged", "file_written": False}
        copy_result = {"saved": True, "save_disposition": "copy_written", "file_written": True}
        saves.append({
            "index": index, "document": primary, "truthful": True,
            "disposition": "written", "file_written": True,
            "durability_verified": True, "sha256_before": before,
            "sha256_after": after, "canonical_path": str(canonical_path),
            "canonical_artifact_sha256": final_canonical_hash,
            "written_result": written_result,
            "unchanged_save": {"file_written": False, "disposition": "unchanged", "sha256_after": after, "result": unchanged_result},
            "save_copy": {"destination": copy, "readable_archive": True, "canonical_unchanged": True, "sha256_after": after, "artifact_sha256": after, "result": copy_result},
            "actual_save_operations": [
                {"kind": "canonical_written_save", "document": primary, "canonical_path": str(canonical_path), "sha256_before": before, "sha256_after": after, "disposition": "written", "file_written": True, "durability_verified": True, "truthful": True, "result": written_result},
                {"kind": "canonical_unchanged_save", "document": primary, "canonical_path": str(canonical_path), "sha256_before": after, "sha256_after": after, "disposition": "unchanged", "file_written": False, "truthful": True, "result": unchanged_result},
                {"kind": "save_copy", "document": primary, "canonical_path": str(canonical_path), "destination": copy, "canonical_sha256_before": after, "canonical_sha256_after": after, "sha256_after": after, "disposition": "copy_written", "file_written": True, "truthful": True, "result": copy_result},
            ],
        })
    payload = {
        "schema_version": 2,
        "stage": stage,
        "mode": "stage",
        "verdict": "PASSED",
        "checks": [{"name": name, "passed": True} for name in checks],
        "failed_checks": [],
        "cycles": cycles,
        "personal_action_proofs": proofs,
        "saves": saves,
        "coverage": {"required": list(COVERAGE_ITEMS), "observed": list(COVERAGE_ITEMS), "missing": []},
        "artifacts": {
            "documents": [
                {"path": str(canonical_path), "size": canonical_path.stat().st_size, "readable_archive": True, "sha256": final_canonical_hash},
                *[
                    {"path": str(copy_path), "size": copy_path.stat().st_size, "readable_archive": True, "sha256": copy_hashes[index]}
                    for index, copy_path in enumerate(copy_paths)
                ],
                {"path": str(secondary_path), "size": secondary_path.stat().st_size, "readable_archive": True, "sha256": secondary_hash},
            ],
            "lock_anchors": [], "unexplained": [],
        },
        "shutdown": {
            "forced": False, "stalled_stage": None, "failed_step": None,
            "rpc_error": None, "deadline_seconds": 60,
            "requested_utc": "2026-08-25T00:00:00+00:00",
            "documents_closed_utc": "2026-08-25T00:00:01+00:00",
            "rpc_admission_closed_utc": "2026-08-25T00:00:02+00:00",
            "worker_shutdown_utc": "2026-08-25T00:00:03+00:00",
            "listener_shutdown_utc": "2026-08-25T00:00:04+00:00",
            "window_closed_utc": "2026-08-25T00:00:05+00:00",
            "process_exit_utc": "2026-08-25T00:00:06+00:00",
        },
        "environment": {
            "binary_fingerprint": {
                "FreeCAD.exe": fixture_fingerprint
            },
            "git": {
                "parent_commit": fixture_git["parent_commit"],
                "nested_commit": fixture_git["nested_commit"],
                "recorded_gitlink": fixture_git["recorded_gitlink"],
                "branch": fixture_git["branch"],
            },
            "isolation_verified": True,
            "reported_user_app_data": "isolated-profile",
            "auth": {
                "v2_session": True, "session_token_present": True,
                "session_token_length": 32, "mcp_instance_id": "instance",
                "profile_instance_id": "profile", "protocol_version": 2,
            },
            "remote_actor": {
                "mode": "in_process_typed_session", "child_token_absence_proved": True,
                "adr_deviation": "section 1.1",
                "holds_rpc_session_in_coordinator_process": True,
            },
            "session_ttl": {
                "environment_variable": "FREECAD_MCP_SESSION_TTL_SECONDS",
                "override_present": False, "override_value": None,
                "effective_seconds": 300.0, "default_seconds": 300.0, "source": "default",
            },
            "build_provenance": {
                "head_commit": fixture_git["parent_commit"],
                "head_committed_utc": _epoch_to_utc_iso(fixture_history[0][1]),
                "history_depth": len(fixture_history),
                "binaries": {
                    "FreeCAD.exe": {
                        "mtime_utc": _epoch_to_utc_iso(fixture_binary.stat().st_mtime),
                        "commits_not_in_binary": fixture_missing,
                        "predates_head": bool(fixture_missing),
                    }
                },
                "binaries_predating_head": ["FreeCAD.exe"] if fixture_missing else [],
                "binary_commit_binding_enforced": False,
                "provenance_caveat": "Recorded, not enforced.",
                "stage_sources": {
                    relative: sha256_file(REPO_ROOT.joinpath(*relative.split("/")))
                    for relative in STAGE_SOURCE_FILES
                },
            },
        },
        "conflicts": {
            "same_property": {
                "document": primary, "targeted": True, "write_lane_healthy": True,
                "refusal": {"success": False, "error_code": "DOCUMENT_CONFLICT", "data": {"changed_semantic_keys": [ALPHA_PROPERTY_KEY], "expected_revisions": {ALPHA_PROPERTY_KEY: 1}, "current_revisions": {ALPHA_PROPERTY_KEY: 2}}},
                "changed_semantic_keys": [ALPHA_PROPERTY_KEY],
                "expected_revisions": {ALPHA_PROPERTY_KEY: 1},
                "current_revisions": {ALPHA_PROPERTY_KEY: 2},
                "readiness": {"success": True, "documents": [{"document": primary, "ready": True, "quarantined": False, "collaboration_poisoned": False}]},
            },
            "independent_property": {
                "document": secondary, "both_landed": True, "committed_once": True,
                "commit": {"success": True, "committed": True}, "replay": {"success": True, "committed": True},
                "alpha_revision_before": 1, "alpha_revision_after": 2,
                "beta_revision_before": 4, "beta_revision_after": 5,
                "alpha_value": 11, "beta_value": 30,
            },
        },
        "pause_resume": {
            "pause": {"observed": {"paused": True}},
            "refused": {
                "success": False, "error_code": "AUTOMATION_PAUSED",
                "error": "paused new MCP writes", "data": {},
            },
            "readiness_while_paused": {
                "success": True,
                "documents": [{"document": primary, "automation_paused": True, "active_write_count": 0}],
                "automation_pause": {"paused": True, "active_write_count": 0},
            },
            "resume": {"observed": {"paused": False}},
            "after": {"commit": {"success": True}, "value": 55},
        },
        "history": {
            "undo_head": {"count": 1, "head": "undo"},
            "undo_head_refusal": {
                "success": False, "error_code": "HISTORY_HEAD_REJECTED",
                "current_undo_count": 1, "current_undo_head": "undo",
            },
            "undo_result": {"success": True}, "value_after_edit": 77, "value_after_undo": 0,
            "redo_head": {"count": 1, "head": "redo"},
            "redo_head_refusal": {
                "success": False, "error_code": "HISTORY_HEAD_REJECTED",
                "current_redo_count": 1, "current_redo_head": "redo",
            },
            "redo_result": {"success": True}, "value_after_redo": 77,
            "mismatched_head_refused": True,
        },
    }
    def operation(method: str, document: str, selector: dict[str, Any], operation_id: str, result: dict[str, Any], **parameters: Any) -> dict[str, Any]:
        return {
            "operation_id": operation_id, "method": method,
            "parameters": {"doc_selector": selector, "operation_id": operation_id, **parameters},
            "result": result,
        }

    primary_selector = snapshot["identity_selector"]
    secondary_selector = secondary_snapshot["identity_selector"]
    same = payload["conflicts"]["same_property"]
    alpha_key = {"kind": "ObjectModel", "subject": "StressBox"}
    beta_key = {"kind": "ObjectProperty", "subject": "SecondBox", "property_name": "BetaValue"}
    def local_edit(operation_id: str, document: str, value: int) -> dict[str, Any]:
        params = {"document": document, "object": "StressBox", "property": "AlphaValue", "value": value, "stage_prepared": True}
        return {"operation_id": operation_id, "action": "local_property_edit", "parameters": params, "observed": {key: params[key] for key in ("document", "object", "property", "value")}}
    same_operations = {
        "selector": primary_selector,
        "begin": operation("begin_checked_edit", primary, primary_selector, "same-begin", {"success": True, "session_id": "same-session"}, revision_keys=[alpha_key]),
        "local_edit": local_edit("same-local", primary, 42),
        "refused_commit": operation("commit_checked_property", primary, primary_selector, "same-commit", same["refusal"], session_id="same-session", object_name="StressBox", property_name="AlphaValue", value_type="integer", value="10"),
        "observed": {"document": primary, "alpha_value": 42, "expected_revisions": same["expected_revisions"], "current_revisions": same["current_revisions"]},
    }
    same["stage_operations"] = same_operations
    independent = payload["conflicts"]["independent_property"]
    independent_operations = {
        "selector": secondary_selector,
        "begin": operation("begin_checked_edit", secondary, secondary_selector, "independent-begin", {"success": True, "session_id": "independent-session"}, revision_keys=[beta_key]),
        "local_edit": local_edit("independent-local", secondary, 11),
        "commit_operation": operation("commit_checked_property", secondary, secondary_selector, "independent-commit", independent["commit"], session_id="independent-session", object_name="SecondBox", property_name="BetaValue", value_type="integer", value="30"),
        "observed": {"document": secondary, **{key: independent[key] for key in ("alpha_revision_before", "alpha_revision_after", "beta_revision_before", "beta_revision_after", "alpha_value", "beta_value")}},
    }
    independent_operations["commit_operation_replay"] = {
        **independent_operations["commit_operation"],
        "replay_of_operation_id": "independent-commit",
        "result": independent["replay"],
    }
    independent["stage_operations"] = independent_operations
    history = payload["history"]
    history["document"] = primary
    history["stage_operations"] = {
        "selector": primary_selector,
        "local_edit": local_edit("history-local", primary, 77),
        "undo_probe": operation("undo", primary, primary_selector, "history-undo-probe", history["undo_head_refusal"], expected_undo_count=-1, expected_undo_head="part3-history-head-probe"),
        "undo": operation("undo", primary, primary_selector, "history-undo", history["undo_result"], expected_undo_count=1, expected_undo_head="undo"),
        "redo_probe": operation("redo", primary, primary_selector, "history-redo-probe", history["redo_head_refusal"], expected_redo_count=-1, expected_redo_head="part3-history-head-probe"),
        "redo": operation("redo", primary, primary_selector, "history-redo", history["redo_result"], expected_redo_count=1, expected_redo_head="redo"),
        "observed": {key: history[key] for key in ("value_after_edit", "value_after_undo", "value_after_redo")},
    }
    pause = payload["pause_resume"]
    pause["stage_operations"] = {
        "document": primary, "selector": primary_selector,
        "pause": {"operation_id": "pause-local", "action": "pause_writes", "parameters": {}, "observed": {"paused": True}},
        "resume": {"operation_id": "resume-local", "action": "resume_writes", "parameters": {}, "observed": {"paused": False}},
        "refused_write": {"method": "edit_object", "parameters": {"doc_name": primary, "obj_name": "SecondBox", "properties": {"Properties": {"BetaValue": 7}}}, "result": pause["refused"]},
        "paused_read": {"operation_id": None, "method": "get_semantic_revisions", "parameters": {"doc_selector": primary_selector, "revision_keys": [beta_key]}, "result": {"revisions": [{"key": BETA_PROPERTY_KEY, "revision": 5}]}},
        "paused_read_result": {"revisions": [{"key": BETA_PROPERTY_KEY, "revision": 5}]}, "readiness": pause["readiness_while_paused"],
        "begin": operation("begin_checked_edit", primary, primary_selector, "pause-begin", {"success": True, "session_id": "pause-session"}, revision_keys=[beta_key]),
        "after_commit": operation("commit_checked_property", primary, primary_selector, "pause-commit", pause["after"]["commit"], session_id="pause-session", object_name="SecondBox", property_name="BetaValue", value_type="integer", value="55"),
        "value_after_resume": 55,
    }
    pause["pause"] = pause["stage_operations"]["pause"]
    pause["resume"] = pause["stage_operations"]["resume"]
    # The producer records every cycle=None local action in this ordered stream;
    # retain one real-shaped personal action/proof pair so packet validation
    # exercises ownership beyond cycle-local actions.
    out_action = dict(cycles[0]["local_actions"][0])
    out_action["operation_id"] = "out-of-cycle-personal-0"
    out_action["out_of_cycle_index"] = 0
    out_proof = deepcopy(proofs[0])
    out_proof["index"] = len(proofs)
    out_proof["operation_id"] = out_action["operation_id"]
    out_action["personal_action_proof_index"] = out_proof["index"]
    payload["out_of_cycle_local_actions"] = [out_action]
    payload["personal_action_proofs"].append(out_proof)
    return payload


def _record_file(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    return {
        "path": str(path.resolve()),
        "sha256": hashlib.sha256(data).hexdigest(),
        "size": len(data),
    }


def _frozen_git_result_contract(root: Path) -> dict[str, Any]:
    from tests.gui.part3.stage_gate_runner import SNAPSHOT_GIT_COMMANDS, SNAPSHOT_GIT_RESULT_KEYS

    results = {
        key: {
            "argv": list(SNAPSHOT_GIT_COMMANDS[key]), "return_code": 0,
            "stdout": "" if key in {"nested_status", "nested_diff"} else f"frozen-{key}\n",
            "stderr": "",
        }
        for key in SNAPSHOT_GIT_RESULT_KEYS
    }
    return _write(root / "frozen-git-results.json", json.dumps(results, sort_keys=True) + "\n")


def _resource_commands(owned_volume: str) -> dict[str, list[str]]:
    return {
        "owned_snapshot_volume_absent": ["docker", "volume", "inspect", owned_volume],
        "owned_volume_users": [
            "docker", "ps", "-a", "--filter", f"volume={owned_volume}", "--format", "{{.ID}}",
        ],
        "active_containers": ["docker", "ps", "-q"],
        "retained_stopped_containers": [
            "docker", "ps", "-a", "--filter", "status=exited", "--format", "{{.ID}}|{{.State}}",
        ],
        "general_volumes_preserved": ["docker", "volume", "ls", "--format", "{{.Name}}"],
        "target_processes": ["pwsh", "-NoProfile", "-File", "closeout/target-processes.ps1"],
        "target_listeners": ["pwsh", "-NoProfile", "-File", "closeout/target-listeners.ps1"],
    }


def _aftermath_receipts(
    root: Path, expected: dict[str, bool | int], commands: dict[str, list[str]],
    volumes: list[str], stopped: list[dict[str, str]],
) -> dict[str, dict[str, Any]]:
    """Write Luna-shaped immutable raw command receipts for the seal helper."""

    receipts: dict[str, dict[str, Any]] = {}
    for key in expected:
        if key == "owned_snapshot_volume_absent":
            raw = {
                "argv": commands[key], "target": commands[key][-1],
                "wrapper_return_code": 0, "underlying_return_code": 1,
                "stdout": "[]\n", "stderr": f"Error response from daemon: get {commands[key][-1]}: no such volume\n",
            }
        elif key == "general_volumes_preserved":
            raw = {
                "argv": commands[key], "target": key, "wrapper_return_code": 0,
                "underlying_return_code": 0, "stdout": "".join(f"{name}\n" for name in volumes), "stderr": "",
            }
        elif key == "retained_stopped_containers":
            raw = {
                "argv": commands[key], "target": key, "wrapper_return_code": 0,
                "underlying_return_code": 0,
                "stdout": "".join(f"{item['id']}|{item['state']}\n" for item in stopped), "stderr": "",
            }
        elif key in {"owned_volume_users", "active_containers"}:
            raw = {
                "argv": commands[key],
                "target": commands["owned_snapshot_volume_absent"][-1] if key == "owned_volume_users" else key,
                "wrapper_return_code": 0, "underlying_return_code": 0,
                "stdout": "", "stderr": "",
            }
        else:
            raw = {
                "argv": commands[key], "target": commands[key][-1], "wrapper_return_code": 0,
                "underlying_return_code": 0, "stdout": "[]\n", "stderr": "",
            }
        receipts[key] = _write(root / f"{key}.json", json.dumps(raw) + "\n")
    return receipts


def _aftermath_resource_contract(
    root: Path, commands: dict[str, list[str]], volumes: list[str], stopped: list[dict[str, str]]
) -> dict[str, Any]:
    return _write(
        root / "aftermath-resource-contract.json",
        json.dumps(
            {"commands": commands, "general_volumes": volumes, "stopped_containers": stopped},
            separators=(",", ":"), sort_keys=True,
        ) + "\n",
    )


def _artifacts(tmp_path: Path, stage: str) -> dict[str, Any]:
    from tests.gui.part3 import stage_gate_runner as runner

    evidence_payload = _stage_evidence(stage, documents_root=tmp_path / "source-documents")
    evidence = json.dumps(evidence_payload)
    function_name = (
        "test_stage_a_runs_ten_view_cycles_and_five_save_cycles"
        if stage == "A" else "test_stage_b_runs_fifty_view_cycles_and_twenty_save_cycles"
    )
    junit = (
        '<testsuite tests="1" failures="0" errors="0" skipped="0">'
        '<testcase classname="tests.gui.part3.test_part3_stage_acceptance" '
        f'name="{function_name}" /></testsuite>\n'
    )
    fcstd: dict[str, Any] = {}
    for index, entry in enumerate(evidence_payload["artifacts"]["documents"]):
        source_path = entry["path"]
        if not source_path.lower().endswith(".fcstd"):
            continue
        source = Path(source_path)
        target = runner._retained_fcstd_target(tmp_path, index, source_path)
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(source, target)
        fcstd[os.path.normcase(os.path.normpath(source_path)).casefold()] = {
            "source_path": source_path,
            "artifact": _record_file(target),
        }
    return {
        "runner_log": _write(tmp_path / "runner.log", "1 passed\n"),
        "evidence": _write(tmp_path / "evidence.json", evidence + "\n"),
        "launcher_log": _write(tmp_path / "launcher.log", "launcher graceful\n"),
        "junit": _write(tmp_path / "junit.xml", junit),
        "fcstd": fcstd,
    }


def _junit(stage: str) -> str:
    name = (
        "test_stage_a_runs_ten_view_cycles_and_five_save_cycles"
        if stage.upper() == "A" else "test_stage_b_runs_fifty_view_cycles_and_twenty_save_cycles"
    )
    return (
        '<testsuite tests="1" failures="0" errors="0" skipped="0">'
        '<testcase classname="tests.gui.part3.test_part3_stage_acceptance" '
        f'name="{name}" /></testsuite>\n'
    )


def _ttl() -> dict[str, Any]:
    from tests.gui.part3.evidence import session_ttl_provenance

    return session_ttl_provenance(REPO_ROOT, environ={})


def _windows_packet(tmp_path: Path, stage: str = "a") -> dict[str, Any]:
    from tests.gui.part3 import stage_gate_runner as runner

    stage_upper = stage.upper()
    artifacts = _artifacts(tmp_path, stage_upper)
    artifacts["handoff"] = _write(
        tmp_path / "handoff.json",
        json.dumps(
            {
                "schema_version": 1,
                "stage": stage_upper,
                "coordinator_return_code": 0,
                "evidence_path": "owned/evidence.json",
                "launcher_path": "owned/launcher.log",
            }
        )
        + "\n",
    )
    python_executable = tmp_path / "python.exe"
    return {
        "schema_version": 1,
        "platform": "windows",
        "stage": stage_upper,
        "repo_root": str(REPO_ROOT),
        "prepared_before_execution": True,
        "exact_command": runner._windows_stage_command(
            python_executable, tmp_path, stage
        ),
        "environment": {
            "PART3_STAGE_LIVE": "1",
            "FREECAD_MCP_SESSION_TTL_SECONDS": None,
            "PART3_STAGE_EVIDENCE_HANDOFF": str(tmp_path / "handoff.json"),
        },
        "ttl": _ttl(),
        "execution": {"status": "completed", "return_code": 0},
        "artifacts": artifacts,
    }


def _inspect(
    container_id: str,
    *,
    image_id: str = "sha256:" + "a" * 64,
    container_name: str = "part3-b-synthetic",
) -> dict[str, Any]:
    return {
        "Id": container_id,
        "Image": image_id,
        "Name": f"/{container_name}",
        "State": {"Running": True},
        "HostConfig": {"NetworkMode": "none", "ReadonlyRootfs": True},
        "Mounts": [
            {"Source": str(REPO_ROOT), "Destination": "/repo", "RW": False, "Mode": "ro"},
            {
                "Source": str(REPO_ROOT / "build"),
                "Destination": "/workspace/build",
                "RW": False,
                "Mode": "ro",
            },
        ],
    }


def _linux_packet(tmp_path: Path, stage: str = "b") -> dict[str, Any]:
    from tests.gui.part3 import stage_gate_runner as runner

    image_id = "sha256:" + "a" * 64
    container_id = "b" * 64
    container_name = f"part3-{stage}-synthetic"
    stage_upper = stage.upper()
    artifacts = _artifacts(tmp_path, stage_upper)
    artifacts["aftermath"] = _write(
        tmp_path / "aftermath.txt",
        "pytest_rc=0\nfreecad_or_launcher_leftovers_begin\n\nfreecad_or_launcher_leftovers_end\nport_9875_listeners_begin\n\nport_9875_listeners_end\n",
    )
    artifacts["container_script"] = _write(
        tmp_path / f"stage-{stage}-container.sh",
        runner._linux_stage_script(stage),
    )
    artifacts["release_barrier"] = _write(
        tmp_path / f"stage-{stage}.release", "release\n"
    )
    artifacts["handoff"] = _write(
        tmp_path / f"stage-{stage}-handoff.json",
        json.dumps(
            {
                "schema_version": 1,
                "stage": stage_upper,
                "coordinator_return_code": 0,
                "evidence_path": "owned/evidence.json",
                "launcher_path": "owned/launcher.log",
            }
        )
        + "\n",
    )
    attempts = []
    for destination in ("/repo", "/workspace/build"):
        target = f"{destination}/.part3-write-probe"
        stdout = json.dumps(
            {
                "path": destination,
                "target": target,
                "errno": 30,
                "errno_name": "EROFS",
                "strerror": "Read-only file system",
            }
        ) + "\n"
        attempts.append(
            {
                "path": destination,
                "container_id": container_id,
                "command": runner._erofs_probe_command(container_id, destination),
                "return_code": 73,
                "errno": 30,
                "errno_name": "EROFS",
                "strerror": "Read-only file system",
                "target": target,
                "stdout": stdout,
                "refused": True,
                "pre_inspect": _inspect(container_id, image_id=image_id, container_name=container_name),
                "post_inspect": _inspect(container_id, image_id=image_id, container_name=container_name),
            }
        )
    return {
        "schema_version": 1,
        "platform": "linux-docker",
        "stage": stage_upper,
        "repo_root": str(REPO_ROOT),
        "build_root": str(REPO_ROOT / "build"),
        "prepared_before_execution": True,
        "exact_command": runner._linux_stage_command(
            repo_root=REPO_ROOT,
            build_root=REPO_ROOT / "build",
            output=tmp_path,
            image_id=image_id,
            container_name=container_name,
            stage=stage,
        ),
        "environment": {
            "PART3_STAGE_LIVE": "1",
            "FREECAD_MCP_SESSION_TTL_SECONDS": None,
            "PART3_STAGE_EVIDENCE_HANDOFF": f"/out/stage-{stage}-handoff.json",
        },
        "ttl": _ttl(),
        "execution": {"status": "completed", "return_code": 0},
        "artifacts": artifacts,
        "docker": {
            "image": {
                "requested": "freecad-collaboration-ci:ubuntu24.04-20260801",
                "id": image_id,
                "inspect": {"Id": image_id},
            },
            "container_id": container_id,
            "live_inspect": _inspect(container_id, image_id=image_id, container_name=container_name),
            "barrier": {
                "ready": True,
                "ready_inspect_utc": "2026-08-23T00:00:00+00:00",
                "probes_completed_utc": "2026-08-23T00:00:01+00:00",
                "released_after_probes": True,
                "released_utc": "2026-08-23T00:00:02+00:00",
            },
            "write_probe": {
                "attempted": True,
                "paths": ["/repo", "/workspace/build"],
                "refused": True,
                "exit_code": 73,
                "attempts": attempts,
            },
            "cleanup": {
                "command": ["docker", "rm", "-f", container_id],
                "return_code": 0,
                "timed_out": False,
                "post_cleanup_absence": {
                    "id": {
                        "command": ["docker", "inspect", container_id],
                        "return_code": 1,
                        "timed_out": False,
                        "stdout": "",
                        "stderr": f"Error: No such object: {container_id}",
                    },
                    "name": {
                        "command": ["docker", "inspect", f"part3-{stage}-synthetic"],
                        "return_code": 1,
                        "timed_out": False,
                        "stdout": "",
                        "stderr": f"Error: No such object: part3-{stage}-synthetic",
                    },
                },
            },
        },
    }


def _replace_container_script(packet: dict[str, Any], content: str) -> None:
    script_path = Path(packet["artifacts"]["container_script"]["path"])
    packet["artifacts"]["container_script"] = _write(script_path, content)


def _output_mount_option_index(packet: dict[str, Any]) -> int:
    command = packet["exact_command"]
    expected = (
        "type=bind,src="
        f"{Path(packet['artifacts']['container_script']['path']).parent.resolve()}"
        ",dst=/out"
    )
    value_index = command.index(expected)
    assert command[value_index - 1] == "--mount"
    return value_index - 1


def _insert_before_entrypoint(packet: dict[str, Any], *values: str) -> None:
    command = packet["exact_command"]
    entrypoint_index = command.index("--entrypoint")
    command[entrypoint_index:entrypoint_index] = list(values)


def test_windows_stage_a_rejects_appended_stage_b_nodeid(tmp_path: Path) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    packet = _windows_packet(tmp_path, "a")
    packet["exact_command"].append(runner.STAGE_NODEIDS["b"])
    with pytest.raises(ValueError):
        runner.validate_packet(packet)


def test_windows_stage_rejects_extra_arbitrary_pytest_nodeid(tmp_path: Path) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    packet = _windows_packet(tmp_path, "b")
    packet["exact_command"].append(
        "tests/gui/part3/test_part3_stage_gate_runner.py::"
        "test_runner_cli_is_directly_main_agent_runnable"
    )
    with pytest.raises(ValueError):
        runner.validate_packet(packet)


def test_linux_stage_rejects_retained_script_without_pytest(tmp_path: Path) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    packet = _linux_packet(tmp_path, "a")
    _replace_container_script(
        packet,
        "#!/usr/bin/env bash\n"
        "release=/out/stage-a.release\n"
        'while [ ! -f "$release" ]; do sleep 0.1; done\n'
        "export PART3_STAGE_LIVE=1\n"
        "unset FREECAD_MCP_SESSION_TTL_SECONDS\n",
    )
    with pytest.raises(ValueError):
        runner.validate_packet(packet)


def test_linux_stage_rejects_other_stage_command_appended(tmp_path: Path) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    packet = _linux_packet(tmp_path, "a")
    _replace_container_script(
        packet,
        runner._linux_stage_script("a")
        + f"python3 -m pytest {runner.STAGE_NODEIDS['b']}\n",
    )
    with pytest.raises(ValueError):
        runner.validate_packet(packet)


def test_linux_stage_rejects_outer_command_targeting_other_script(
    tmp_path: Path,
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    packet = _linux_packet(tmp_path, "b")
    packet["exact_command"][-1] = "/out/not-the-retained-stage-script.sh"
    with pytest.raises(ValueError):
        runner.validate_packet(packet)


@pytest.mark.parametrize("stage", ["a", "b"])
def test_exact_windows_constructor_commands_validate(
    tmp_path: Path, stage: str
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    packet = _windows_packet(tmp_path / stage, stage)
    assert runner.validate_packet(packet)["platform"] == "windows"


@pytest.mark.parametrize("stage", ["a", "b"])
def test_exact_linux_constructor_scripts_and_commands_validate(
    tmp_path: Path, stage: str
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    packet = _linux_packet(tmp_path / stage, stage)
    assert runner.validate_packet(packet)["platform"] == "linux-docker"


@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize(
    "mutation",
    [
        "mount_option_becomes_env",
        "mount_option_becomes_label",
        "orphaned_mount_spec",
        "missing_output_mount",
        "duplicated_output_mount",
        "conflicting_output_mount",
        "substituted_source",
        "substituted_destination",
    ],
)
def test_linux_output_mount_is_one_exact_ordered_option_value_pair(
    tmp_path: Path, stage: str, mutation: str
) -> None:
    """REVIEW-P3-WP26-005: a bare mount-spec string is never provenance."""

    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path / f"{stage}-{mutation}", stage)
    command = packet["exact_command"]
    option_index = _output_mount_option_index(packet)
    mount_spec = command[option_index + 1]
    if mutation == "mount_option_becomes_env":
        command[option_index] = "--env"
    elif mutation == "mount_option_becomes_label":
        command[option_index] = "--label"
    elif mutation == "orphaned_mount_spec":
        command.pop(option_index)
    elif mutation == "missing_output_mount":
        del command[option_index : option_index + 2]
    elif mutation == "duplicated_output_mount":
        command[option_index:option_index] = ["--mount", mount_spec]
    elif mutation == "conflicting_output_mount":
        command[option_index:option_index] = [
            "--mount",
            "type=bind,src=/different-output,dst=/out",
        ]
    elif mutation == "substituted_source":
        command[option_index + 1] = "type=bind,src=/different-output,dst=/out"
    else:
        command[option_index + 1] = (
            f"type=bind,src={tmp_path.resolve()},dst=/not-out"
        )
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("value", [None, False, 1, "true"])
def test_linux_live_inspect_requires_boolean_true_readonly_rootfs(
    tmp_path: Path, stage: str, value: object
) -> None:
    """REVIEW-P3-WP26-006: live root read-only state is exact and typed."""

    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path / f"{stage}-{value!s}", stage)
    host_config = packet["docker"]["live_inspect"]["HostConfig"]
    if value is None:
        host_config.pop("ReadonlyRootfs")
    else:
        host_config["ReadonlyRootfs"] = value
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize(
    "override",
    [
        ("--read-only=false",),
        ("--read-only",),
        ("--read-only=true",),
        ("--read-only=false", "--read-only=true"),
    ],
)
def test_linux_command_rejects_duplicate_or_conflicting_root_read_only_flags(
    tmp_path: Path, stage: str, override: tuple[str, ...]
) -> None:
    """REVIEW-P3-WP26-006: one constructor-owned root flag is authoritative."""

    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path / f"{stage}-{'-'.join(override)}", stage)
    _insert_before_entrypoint(packet, *override)
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("inspect_name", ["pre_inspect", "post_inspect"])
@pytest.mark.parametrize("value", [None, False, 0, "true"])
def test_linux_erofs_probe_inspects_preserve_readonly_rootfs(
    tmp_path: Path, inspect_name: str, value: object
) -> None:
    """A mount EROFS probe cannot conceal root-filesystem isolation drift."""

    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path / f"{inspect_name}-{value!s}")
    host_config = packet["docker"]["write_probe"]["attempts"][0][inspect_name][
        "HostConfig"
    ]
    if value is None:
        host_config.pop("ReadonlyRootfs")
    else:
        host_config["ReadonlyRootfs"] = value
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize(
    "path",
    [
        ("exact_command",),
        ("prepared_before_execution",),
        ("ttl", "effective_seconds"),
        ("ttl", "override_present"),
        ("artifacts", "evidence"),
        ("artifacts", "launcher_log"),
        ("artifacts", "junit"),
    ],
)
def test_windows_packet_rejects_missing_execution_proof(
    tmp_path: Path, path: tuple[str, ...]
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _windows_packet(tmp_path)
    cursor = packet
    for name in path[:-1]:
        cursor = cursor[name]
    cursor.pop(path[-1])
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("value", [999, float("nan"), float("inf"), -float("inf")])
def test_default_ttl_rejects_unequal_or_nonfinite_effective(
    tmp_path: Path, value: float
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _windows_packet(tmp_path)
    packet["ttl"]["effective_seconds"] = value
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("field", ["default_seconds", "constants_file"])
def test_default_ttl_rejects_missing_provenance(tmp_path: Path, field: str) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _windows_packet(tmp_path)
    packet["ttl"].pop(field, None)
    with pytest.raises(ValueError):
        validate_packet(packet)


def test_default_ttl_rejects_mismatched_constants_fingerprint(tmp_path: Path) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _windows_packet(tmp_path)
    packet["ttl"].setdefault("constants_file", {})["sha256"] = "0" * 64
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize(
    "path",
    [
        ("exact_command",),
        ("docker", "image", "id"),
        ("docker", "image", "inspect"),
        ("docker", "container_id"),
        ("docker", "live_inspect"),
        ("docker", "barrier"),
        ("docker", "write_probe"),
        ("ttl", "effective_seconds"),
        ("artifacts", "evidence"),
        ("artifacts", "aftermath"),
    ],
)
def test_linux_packet_rejects_missing_execution_proof(
    tmp_path: Path, path: tuple[str, ...]
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path)
    cursor = packet
    for name in path[:-1]:
        cursor = cursor[name]
    cursor.pop(path[-1])
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize(
    "mutation",
    [
        "container_exited",
        "daemon_error",
        "missing_shell",
        "permission",
        "malformed",
        "aggregate_only",
    ],
)
def test_linux_packet_rejects_non_erofs_probe_shapes(
    tmp_path: Path, mutation: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path)
    if mutation == "aggregate_only":
        packet["docker"]["write_probe"].pop("attempts")
        with pytest.raises(ValueError):
            validate_packet(packet)
        return
    attempt = packet["docker"]["write_probe"]["attempts"][0]
    if mutation == "container_exited":
        attempt["post_inspect"]["State"]["Running"] = False
    elif mutation == "daemon_error":
        attempt.update(return_code=1, errno=None, errno_name=None, strerror="daemon unavailable")
    elif mutation == "missing_shell":
        attempt.update(return_code=127, errno=None, errno_name=None, strerror="not found")
    elif mutation == "permission":
        attempt.update(return_code=1, errno=13, errno_name="EACCES", strerror="Permission denied")
    else:
        attempt.pop("post_inspect")
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("defect", ["command", "target", "stdout"])
def test_linux_packets_reject_unbound_erofs_command_target_or_stdout(
    tmp_path: Path, stage: str, defect: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path, stage)
    attempt = packet["docker"]["write_probe"]["attempts"][0]
    if defect == "command":
        attempt["command"] = ["docker", "exec", attempt["container_id"], "true"]
    elif defect == "target":
        attempt["target"] = "/tmp/.part3-write-probe"
    else:
        attempt["stdout"] = json.dumps(
            {
                "path": attempt["path"],
                "target": attempt["target"],
                "errno": 13,
                "errno_name": "EACCES",
                "strerror": "Permission denied",
            }
        )
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("timestamp", ["2026-08-25T05:00:00+05:00", "2026-08-25T00:00:00"])
def test_completed_evidence_rejects_non_utc_acknowledgements(timestamp: str) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence("A")
    evidence["cycles"][0]["local_actions"][0]["ack_utc"] = timestamp
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage="A", repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("timestamp", ["2026-08-25T05:00:00+05:00", "2026-08-25T00:00:00"])
def test_packets_reject_non_utc_acknowledgements(
    tmp_path: Path, platform: str, stage: str, timestamp: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    evidence["cycles"][0]["remote_actions"][0]["ack_utc"] = timestamp
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


def test_linux_packet_rejects_false_isolation_claims(tmp_path: Path) -> None:
    """Preserve the iteration-001 network/running/read-only-mount assertions."""

    from tests.gui.part3.stage_gate_runner import validate_packet

    mutations = []
    for network_mode in ("bridge", "default"):
        packet = _linux_packet(tmp_path / network_mode)
        packet["docker"]["live_inspect"]["HostConfig"]["NetworkMode"] = network_mode
        mutations.append(packet)
    packet = _linux_packet(tmp_path / "stopped")
    packet["docker"]["live_inspect"]["State"]["Running"] = False
    mutations.append(packet)
    for mount_index in (0, 1):
        packet = _linux_packet(tmp_path / f"rw-{mount_index}")
        packet["docker"]["live_inspect"]["Mounts"][mount_index]["RW"] = True
        mutations.append(packet)
    packet = _linux_packet(tmp_path / "not-refused")
    packet["docker"]["write_probe"]["refused"] = False
    mutations.append(packet)
    for candidate in mutations:
        with pytest.raises(ValueError):
            validate_packet(candidate)


def test_docker_desktop_empty_mount_mode_is_accepted_when_rw_false(
    tmp_path: Path,
) -> None:
    """Preserve the iteration-001 Docker Desktop live-inspect compatibility."""

    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path)
    for mount in packet["docker"]["live_inspect"]["Mounts"]:
        mount["Mode"] = ""
    for attempt in packet["docker"]["write_probe"]["attempts"]:
        for inspect_key in ("pre_inspect", "post_inspect"):
            for mount in attempt[inspect_key]["Mounts"]:
                mount["Mode"] = ""
    assert validate_packet(packet)["platform"] == "linux-docker"


def test_packet_rejects_nonzero_execution_return_code(tmp_path: Path) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    for packet in (_windows_packet(tmp_path / "w"), _linux_packet(tmp_path / "l")):
        packet["execution"]["return_code"] = 1
        with pytest.raises(ValueError):
            validate_packet(packet)


def test_packet_rejects_artifact_hash_mismatch(tmp_path: Path) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _windows_packet(tmp_path)
    packet["artifacts"]["evidence"]["sha256"] = "f" * 64
    with pytest.raises(ValueError):
        validate_packet(packet)


def test_packet_rejects_skipped_junit_even_with_zero_rc(tmp_path: Path) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _windows_packet(tmp_path)
    packet["artifacts"]["junit"] = _write(
        tmp_path / "junit.xml",
        '<testsuite tests="1" failures="0" errors="0" skipped="1" />\n',
    )
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("child", ["failure", "error", "skipped"])
def test_junit_rejects_hidden_child_outcome(tmp_path: Path, child: str) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _windows_packet(tmp_path)
    junit_path = Path(packet["artifacts"]["junit"]["path"])
    name = "test_stage_a_runs_ten_view_cycles_and_five_save_cycles"
    packet["artifacts"]["junit"] = _write(
        junit_path,
        '<testsuite tests="1" failures="0" errors="0" skipped="0">'
        '<testcase classname="tests.gui.part3.test_part3_stage_acceptance" '
        f'name="{name}"><{child} /></testcase></testsuite>\n',
    )
    with pytest.raises(ValueError, match="JUnit"):
        validate_packet(packet)


@pytest.mark.parametrize(
    "junit",
    [
        '<testsuite tests="2" failures="0" errors="0" skipped="0">'
        '<testcase classname="tests.gui.part3.test_part3_stage_acceptance" '
        'name="test_stage_a_runs_ten_view_cycles_and_five_save_cycles" /></testsuite>\n',
        '<testsuites tests="2" failures="0" errors="0" skipped="0">'
        '<testsuite tests="1" failures="0" errors="0" skipped="0">'
        '<testcase classname="tests.gui.part3.test_part3_stage_acceptance" '
        'name="test_stage_a_runs_ten_view_cycles_and_five_save_cycles" />'
        '</testsuite></testsuites>\n',
    ],
)
def test_junit_rejects_declared_count_disagreement(tmp_path: Path, junit: str) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _windows_packet(tmp_path)
    junit_path = Path(packet["artifacts"]["junit"]["path"])
    packet["artifacts"]["junit"] = _write(junit_path, junit)
    with pytest.raises(ValueError, match="JUnit"):
        validate_packet(packet)


def test_junit_accepts_pytest_wrapper_without_root_aggregates(tmp_path: Path) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _windows_packet(tmp_path)
    junit_path = Path(packet["artifacts"]["junit"]["path"])
    packet["artifacts"]["junit"] = _write(
        junit_path,
        '<testsuites name="pytest tests"><testsuite tests="1" failures="0" '
        'errors="0" skipped="0"><testcase '
        'classname="tests.gui.part3.test_part3_stage_acceptance" '
        'name="test_stage_a_runs_ten_view_cycles_and_five_save_cycles" />'
        '</testsuite></testsuites>\n',
    )
    assert validate_packet(packet)["platform"] == "windows"


@pytest.mark.parametrize("aggregate", ["tests", "failures", "errors", "skipped"])
def test_junit_rejects_concrete_suite_missing_each_required_aggregate(
    tmp_path: Path, aggregate: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _windows_packet(tmp_path)
    junit_path = Path(packet["artifacts"]["junit"]["path"])
    attributes = {
        "tests": "1", "failures": "0", "errors": "0", "skipped": "0",
    }
    attributes.pop(aggregate)
    declared = " ".join(f'{key}="{value}"' for key, value in attributes.items())
    packet["artifacts"]["junit"] = _write(
        junit_path,
        '<testsuites name="pytest tests"><testsuite '
        f'{declared}><testcase '
        'classname="tests.gui.part3.test_part3_stage_acceptance" '
        'name="test_stage_a_runs_ten_view_cycles_and_five_save_cycles" />'
        '</testsuite></testsuites>\n',
    )
    with pytest.raises(ValueError, match="missing required"):
        validate_packet(packet)


@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("field", ["Image", "Name"])
@pytest.mark.parametrize("inspect_path", ["live", "pre", "post"])
def test_linux_packet_rejects_image_or_constructor_name_mutation(
    tmp_path: Path, stage: str, field: str, inspect_path: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path, stage)
    if inspect_path == "live":
        inspect = packet["docker"]["live_inspect"]
    else:
        inspect = packet["docker"]["write_probe"]["attempts"][0][f"{inspect_path}_inspect"]
    inspect[field] = "sha256:" + "c" * 64 if field == "Image" else "/other-container"
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
def test_packet_rejects_boolean_coordinator_return_code(
    tmp_path: Path, platform: str, stage: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    handoff_path = Path(packet["artifacts"]["handoff"]["path"])
    handoff = json.loads(handoff_path.read_text(encoding="utf-8"))
    handoff["coordinator_return_code"] = False
    packet["artifacts"]["handoff"] = _write(handoff_path, json.dumps(handoff) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


def _corrupt_final6_cycle(evidence: dict[str, Any], defect: str) -> None:
    cycle = evidence["cycles"][1]
    if defect == "readiness":
        cycle["readiness"]["documents"][0].pop("collaboration_poisoned")
    elif defect == "observed":
        cycle["local_actions"][0]["observed"] = None
    elif defect == "ack":
        cycle["remote_actions"][0]["ack_utc"] = "not-a-time"
    elif defect == "camera":
        cycle["checks"]["camera_changed"] = False
    elif defect == "file_state":
        cycle["file_change_state_after"] = {"pending_changes": [], "has_pending_file_changes": True}
    elif defect == "value":
        cycle["typed_mutation"]["expected_value"] = 999
    elif defect == "operation":
        cycle["local_actions"][0]["operation_id"] = evidence["cycles"][0]["local_actions"][0]["operation_id"]
    else:
        cycle["document"] = "Primary"


def _substitute_literal_stage_identity(evidence: dict[str, Any], defect: str) -> None:
    """Replace one producer-shaped identity lane with the obsolete literals."""

    cycle = evidence["cycles"][0]
    if defect == "cycle":
        cycle["document"] = "Primary"
        return
    if defect == "action":
        action = cycle["local_actions"][0]
        action["parameters"] = {"document": "Secondary"}
        action["observed"] = {"active_document": "Secondary"}
        return

    action = cycle["local_actions"][0]
    proof = evidence["personal_action_proofs"][action["personal_action_proof_index"]]
    original_documents = proof["documents"]
    literal_by_document = dict(zip(original_documents, ["Primary", "Secondary"]))

    def literal_snapshot(snapshot: dict[str, Any], document: str) -> dict[str, Any]:
        copied = deepcopy(snapshot)
        copied["observed_document"] = document
        copied["identity_selector"]["document_name"] = document
        return copied

    proof["documents"] = [literal_by_document[document] for document in original_documents]
    proof["before"] = {
        literal_by_document[document]: literal_snapshot(snapshot, literal_by_document[document])
        for document, snapshot in proof["before"].items()
    }
    proof["after"] = {
        literal_by_document[document]: literal_snapshot(snapshot, literal_by_document[document])
        for document, snapshot in proof["after"].items()
    }
    proof["left_document"] = literal_by_document[proof["left_document"]]
    proof["activated_document"] = literal_by_document[proof["activated_document"]]


@pytest.mark.parametrize("stage", ["A", "B"])
def test_completed_evidence_accepts_producer_shaped_document_identities(stage: str) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    primary, secondary = (evidence["cycles"][index]["document"] for index in (0, 1))
    assert (primary, secondary) == (
        f"Part3Stage{stage}Primarycafebabe",
        f"Part3Stage{stage}Secondarycafebabe",
    )
    validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
def test_packets_accept_producer_shaped_document_identities(
    tmp_path: Path, platform: str, stage: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    assert all(
        document.startswith(f"Part3Stage{stage.upper()}")
        for document in (evidence["cycles"][0]["document"], evidence["cycles"][1]["document"])
    )
    assert validate_packet(packet)["platform"] == platform


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("defect", ["cycle", "action", "proof"])
def test_completed_evidence_rejects_literal_document_identity_substitution(
    stage: str, defect: str
) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    _substitute_literal_stage_identity(evidence, defect)
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("defect", ["cycle", "action", "proof"])
def test_packets_reject_literal_document_identity_substitution(
    tmp_path: Path, platform: str, stage: str, defect: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    _substitute_literal_stage_identity(evidence, defect)
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("defect", ["readiness", "observed", "ack", "camera", "file_state", "value", "operation", "schedule"])
def test_completed_evidence_rejects_final6_exact_cycle_mutations(defect: str) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence("A")
    _corrupt_final6_cycle(evidence, defect)
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage="A", repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("defect", ["readiness", "observed", "ack", "camera", "file_state", "value", "operation", "schedule"])
def test_packets_reject_final6_exact_cycle_mutations(
    tmp_path: Path, platform: str, stage: str, defect: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    _corrupt_final6_cycle(evidence, defect)
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


def _corrupt_final6_save(evidence: dict[str, Any], defect: str) -> None:
    if defect == "artifact_binding":
        save = evidence["saves"][-1]
        replacement = "f" * 64
        save["sha256_after"] = replacement
        save["written_result"] = dict(save["written_result"])
        save["unchanged_save"]["sha256_after"] = replacement
        save["save_copy"]["sha256_after"] = replacement
        save["actual_save_operations"][0]["sha256_after"] = replacement
        save["actual_save_operations"][1]["sha256_before"] = replacement
        save["actual_save_operations"][1]["sha256_after"] = replacement
        save["actual_save_operations"][2]["canonical_sha256_before"] = replacement
        save["actual_save_operations"][2]["canonical_sha256_after"] = replacement
        save["actual_save_operations"][2]["sha256_after"] = replacement
    else:
        evidence["saves"][1]["sha256_before"] = "e" * 64


@pytest.mark.parametrize("defect", ["artifact_binding", "continuity"])
def test_completed_evidence_rejects_final6_save_artifact_and_continuity_mutations(defect: str) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence("A")
    _corrupt_final6_save(evidence, defect)
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage="A", repo_root=REPO_ROOT)


def _add_producer_shaped_cleaning_save(evidence: dict[str, Any]) -> None:
    """Model the real Primary-to-Secondary active-document cleaning stream."""

    save = evidence["saves"][0]
    secondary = evidence["cycles"][1]["document"]
    clean_path = Path(save["canonical_path"]).with_name("cleaning-write.FCStd")
    _write_minimal_fcstd(clean_path, "personal-view-cleaning")
    clean_hash = hashlib.sha256(clean_path.read_bytes()).hexdigest()
    dirty = {"pending_changes": ["model"], "has_pending_file_changes": True}
    clean = {"pending_changes": [], "has_pending_file_changes": False}
    result = {
        "saved": True,
        "save_disposition": "written",
        "file_written": True,
        "durability_verified": True,
    }
    save["actual_save_operations"].append(
        {
            "kind": "pre_personal_view_clean_save",
            "document": save["document"],
            "canonical_path": save["canonical_path"],
            "before_file_change_state": dirty,
            "after_file_change_state": clean,
            "sha256_before": save["sha256_after"],
            "sha256_after": clean_hash,
            "disposition": "written",
            "file_written": True,
            "durability_verified": True,
            "truthful": True,
            "result": result,
        }
    )
    secondary_entry = next(
        entry
        for entry in evidence["artifacts"]["documents"]
        if Path(entry["path"]).name == f"{secondary}.FCStd"
    )
    secondary_before = hashlib.sha256(b"Secondary-before-cleaning").hexdigest()
    secondary_first_path = Path(secondary_entry["path"]).with_name("secondary-cleaning-first.FCStd")
    _write_minimal_fcstd(secondary_first_path, "secondary-first-cleaning")
    secondary_first_hash = hashlib.sha256(secondary_first_path.read_bytes()).hexdigest()
    save["actual_save_operations"].append(
        {
            "kind": "pre_personal_view_clean_save",
            "document": secondary,
            "canonical_path": secondary_entry["path"],
            "before_file_change_state": dirty,
            "after_file_change_state": clean,
            "sha256_before": secondary_before,
            "sha256_after": secondary_first_hash,
            "disposition": "written",
            "file_written": True,
            "durability_verified": True,
            "truthful": True,
            "result": result,
        }
    )
    next_save = evidence["saves"][1]
    next_save["sha256_before"] = clean_hash
    next_save["actual_save_operations"][0]["sha256_before"] = clean_hash
    next_save["actual_save_operations"].append(
        {
            "kind": "pre_personal_view_clean_save",
            "document": secondary,
            "canonical_path": secondary_entry["path"],
            "before_file_change_state": dirty,
            "after_file_change_state": clean,
            "sha256_before": secondary_first_hash,
            "sha256_after": secondary_entry["sha256"],
            "disposition": "written",
            "file_written": True,
            "durability_verified": True,
            "truthful": True,
            "result": result,
        }
    )


@pytest.mark.parametrize("stage", ["A", "B"])
def test_completed_evidence_accepts_producer_shaped_cleaning_save_schedule(
    stage: str,
) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    _add_producer_shaped_cleaning_save(evidence)
    assert [operation["document"] for operation in evidence["saves"][0]["actual_save_operations"][3:]] == [
        evidence["cycles"][0]["document"], evidence["cycles"][1]["document"]
    ]
    validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
def test_packets_accept_producer_shaped_cleaning_save_schedule(
    tmp_path: Path, platform: str, stage: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    _add_producer_shaped_cleaning_save(evidence)
    assert [operation["document"] for operation in evidence["saves"][0]["actual_save_operations"][3:]] == [
        evidence["cycles"][0]["document"], evidence["cycles"][1]["document"]
    ]
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    assert validate_packet(packet)["platform"] == platform


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("document_index", [0, 1])
def test_completed_evidence_rejects_per_document_cleaning_discontinuity(
    stage: str, document_index: int
) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    _add_producer_shaped_cleaning_save(evidence)
    document = evidence["cycles"][document_index]["document"]
    operation = [
        item
        for save in evidence["saves"]
        for item in save["actual_save_operations"][3:]
        if item["document"] == document
    ][-1]
    operation["sha256_before"] = "f" * 64
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("document_index", [0, 1])
def test_packets_reject_per_document_cleaning_discontinuity(
    tmp_path: Path, platform: str, stage: str, document_index: int
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    _add_producer_shaped_cleaning_save(evidence)
    document = evidence["cycles"][document_index]["document"]
    operation = [
        item
        for save in evidence["saves"]
        for item in save["actual_save_operations"][3:]
        if item["document"] == document
    ][-1]
    operation["sha256_before"] = "f" * 64
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("defect", ["missing", "corrupt", "swapped", "substituted"])
def test_completed_evidence_rejects_unbound_fcstd_bytes(defect: str) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence("A")
    canonical = Path(evidence["saves"][-1]["canonical_path"])
    first_copy = Path(evidence["saves"][0]["save_copy"]["destination"])
    canonical_entry = next(
        entry for entry in evidence["artifacts"]["documents"]
        if entry["path"] == str(canonical)
    )
    if defect == "missing":
        canonical.unlink()
    elif defect == "corrupt":
        canonical.write_bytes(b"not an FCStd archive")
    elif defect == "swapped":
        canonical.write_bytes(first_copy.read_bytes())
        canonical_entry.update(_record_file(canonical))
        canonical_entry["readable_archive"] = True
    else:
        canonical_entry["path"] = str(first_copy)
        canonical_entry.update(_record_file(first_copy))
        canonical_entry["readable_archive"] = True
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage="A", repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("defect", ["missing", "corrupt", "swapped", "substituted"])
def test_packets_reject_missing_or_substituted_retained_fcstd(
    tmp_path: Path, platform: str, stage: str, defect: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    retained = packet["artifacts"]["fcstd"]
    identities = list(retained)
    first = retained[identities[0]]
    second = retained[identities[1]]
    first_path = Path(first["artifact"]["path"])
    second_path = Path(second["artifact"]["path"])
    if defect == "missing":
        retained.pop(identities[0])
    elif defect == "corrupt":
        first_path.write_bytes(b"not an FCStd archive")
    elif defect == "swapped":
        first_path.write_bytes(second_path.read_bytes())
        first["artifact"] = _record_file(first_path)
    else:
        first["source_path"] = second["source_path"]
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("defect", ["artifact_binding", "continuity"])
def test_packets_reject_final6_save_artifact_and_continuity_mutations(
    tmp_path: Path, platform: str, stage: str, defect: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    _corrupt_final6_save(evidence, defect)
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("stage", ["a", "b"])
def test_linux_packet_validates_producer_shaped_binary_through_retained_copy(
    tmp_path: Path, stage: str
) -> None:
    from tests.gui.part3.evidence import file_fingerprint
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path, stage)
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    source = Path(evidence["environment"]["binary_fingerprint"]["FreeCAD.exe"]["path"])
    retained = tmp_path / "retained-FreeCAD"
    shutil.copy2(source, retained)
    evidence["environment"]["binary_fingerprint"]["FreeCAD.exe"] = file_fingerprint(retained)
    evidence["environment"]["binary_fingerprint"]["FreeCAD.exe"]["path"] = "/workspace/build/debug/bin/FreeCAD"
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    packet["binary_copies"] = {
        "FreeCAD.exe": {
            "container_path": "/workspace/build/debug/bin/FreeCAD",
            "artifact": {
                "path": str(retained.resolve()),
                "sha256": hashlib.sha256(retained.read_bytes()).hexdigest(),
                "size": retained.stat().st_size,
            },
        }
    }
    assert validate_packet(packet)["platform"] == "linux-docker"


def test_windows_aftermath_uses_one_fixed_deadline_for_large_inventory(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    clock = [0.0]
    calls: list[float] = []
    monkeypatch.setattr(runner.os, "name", "nt")
    monkeypatch.setattr(runner.time, "monotonic", lambda: clock[0])

    def late_query(command: list[str], **kwargs: object) -> subprocess.CompletedProcess[str]:
        del command
        calls.append(float(kwargs["timeout"]))
        clock[0] += 29.5
        return subprocess.CompletedProcess([], 0, "[]", "")

    monkeypatch.setattr(runner.subprocess, "run", late_query)
    aftermath = runner._windows_existing_owned_pids(range(1, 101), deadline=30.0)
    assert calls == [30.0, 0.5]
    assert aftermath["complete"] is False
    assert aftermath["diagnostics"][-1]["diagnostic"] == "aftermath_query_deadline"


@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("wrapper", ["orphan", "direct-wrapper"])
def test_junit_rejects_stage_testcase_without_concrete_suite_ownership(
    tmp_path: Path, stage: str, wrapper: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _windows_packet(tmp_path, stage)
    junit_path = Path(packet["artifacts"]["junit"]["path"])
    node = (
        "test_stage_a_runs_ten_view_cycles_and_five_save_cycles"
        if stage == "a" else "test_stage_b_runs_fifty_view_cycles_and_twenty_save_cycles"
    )
    testcase = (
        '<testcase classname="tests.gui.part3.test_part3_stage_acceptance" '
        f'name="{node}" />'
    )
    if wrapper == "orphan":
        junit = (
            '<testsuites><testsuite tests="0" failures="0" errors="0" skipped="0" />'
            f"{testcase}</testsuites>\n"
        )
    else:
        junit = f'<testsuites>{testcase}</testsuites>\n'
    packet["artifacts"]["junit"] = _write(junit_path, junit)
    with pytest.raises(ValueError, match="owned|no test suite"):
        validate_packet(packet)


@pytest.mark.parametrize("aggregate", ["tests", "failures", "errors", "skipped"])
def test_junit_rejects_present_mismatched_root_aggregate(
    tmp_path: Path, aggregate: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _windows_packet(tmp_path)
    junit_path = Path(packet["artifacts"]["junit"]["path"])
    value = 2 if aggregate == "tests" else 1
    packet["artifacts"]["junit"] = _write(
        junit_path,
        f'<testsuites {aggregate}="{value}"><testsuite tests="1" failures="0" '
        'errors="0" skipped="0"><testcase '
        'classname="tests.gui.part3.test_part3_stage_acceptance" '
        'name="test_stage_a_runs_ten_view_cycles_and_five_save_cycles" />'
        '</testsuite></testsuites>\n',
    )
    with pytest.raises(ValueError, match="JUnit"):
        validate_packet(packet)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("defect", ["missing", "corrupt"])
@pytest.mark.parametrize(
    "family",
    [
        "counts", "checks", "coverage", "personal", "saves", "artifacts", "shutdown",
        "environment", "conflicts", "pause_resume", "history",
        "post_shutdown", "provenance", "paused_refusal", "paused_readiness",
        "post_resume", "undo_result", "redo_result", "redo_head_binding",
        "history_value_binding", "provenance_commit", "provenance_binaries",
        "provenance_predating", "artifact_backup", "artifact_wrong_bucket", "artifact_malformed",
    ],
)
def test_packet_rejects_minimal_pass_marker_and_each_missing_stage_semantic(
    tmp_path: Path, platform: str, stage: str, defect: str, family: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / f"{platform}-{stage}-{family}", stage
    )
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    if defect == "missing":
        paths = {
            "counts": ("cycles",), "checks": ("checks",), "coverage": ("coverage",),
            "personal": ("personal_action_proofs",), "saves": ("saves",),
            "artifacts": ("artifacts",), "shutdown": ("shutdown",),
            "environment": ("environment",), "conflicts": ("conflicts",),
            "pause_resume": ("pause_resume",), "history": ("history",),
            "post_shutdown": ("artifacts", "lock_anchors"),
            "provenance": ("environment", "build_provenance"),
            "paused_refusal": ("pause_resume", "refused"),
            "paused_readiness": ("pause_resume", "readiness_while_paused"),
            "post_resume": ("pause_resume", "after"),
            "undo_result": ("history", "undo_result"),
            "redo_result": ("history", "redo_result"),
            "redo_head_binding": ("history", "redo_head_refusal", "current_redo_head"),
            "history_value_binding": ("history", "value_after_edit"),
            "provenance_commit": ("environment", "build_provenance", "head_commit"),
            "provenance_binaries": ("environment", "build_provenance", "binaries"),
            "provenance_predating": ("environment", "build_provenance", "binaries_predating_head"),
            "artifact_backup": ("artifacts", "documents"),
            "artifact_wrong_bucket": ("artifacts", "documents"),
            "artifact_malformed": ("artifacts", "documents"),
        }[family]
        target: dict[str, Any] = evidence
        for key in paths[:-1]:
            target = target[key]
        target.pop(paths[-1])
    elif family == "counts":
        evidence["cycles"] = []
    elif family == "checks":
        evidence["checks"][0]["passed"] = False
    elif family == "coverage":
        evidence["coverage"]["missing"] = ["save"]
    elif family == "personal":
        evidence["personal_action_proofs"] = []
    elif family == "saves":
        evidence["saves"][0]["truthful"] = False
    elif family == "artifacts":
        evidence["artifacts"]["unexplained"] = [{"path": "surprise"}]
    elif family == "shutdown":
        evidence["shutdown"]["process_exit_utc"] = None
    elif family == "environment":
        evidence["environment"]["auth"]["session_token_present"] = False
    elif family == "conflicts":
        evidence["conflicts"]["same_property"]["targeted"] = False
    elif family == "pause_resume":
        evidence["pause_resume"]["pause"]["observed"]["paused"] = False
    elif family == "history":
        evidence["history"]["undo_head_refusal"]["error_code"] = "OTHER"
    elif family == "post_shutdown":
        evidence["artifacts"]["documents"][0]["readable_archive"] = False
    elif family == "provenance":
        evidence["environment"]["build_provenance"]["stage_sources"] = {"source": "bad"}
    elif family == "paused_refusal":
        evidence["pause_resume"]["refused"]["success"] = True
    elif family == "paused_readiness":
        evidence["pause_resume"]["readiness_while_paused"] = {
            "mutation_readiness": [{"ready": True}],
        }
    elif family == "post_resume":
        evidence["pause_resume"]["after"]["value"] = 0
    elif family == "undo_result":
        evidence["history"]["undo_result"]["success"] = False
    elif family == "redo_result":
        evidence["history"]["redo_result"]["success"] = False
    elif family == "history_value_binding":
        evidence["history"]["value_after_undo"] = evidence["history"]["value_after_edit"]
        evidence["history"]["value_after_redo"] = 0
    elif family == "provenance_commit":
        evidence["environment"]["build_provenance"]["head_commit"] = "f" * 40
    elif family == "provenance_binaries":
        evidence["environment"]["build_provenance"]["binaries"] = {"other": {}}
    elif family == "provenance_predating":
        evidence["environment"]["build_provenance"]["binaries_predating_head"] = ["FreeCAD.exe"]
    elif family == "artifact_backup":
        evidence["artifacts"]["documents"] = [
            {"path": "legacy.fcbak", "size": 1, "readable_archive": False},
        ]
    elif family == "artifact_wrong_bucket":
        evidence["artifacts"]["documents"] = [
            {"path": "model.FCStd.FreeCAD-save.lock", "size": 1},
        ]
    elif family == "artifact_malformed":
        evidence["artifacts"]["documents"] = [
            {"path": "model.FCStd", "readable_archive": True},
        ]
    else:
        evidence["history"]["redo_head_refusal"]["current_redo_head"] = "other"
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)

    minimal = {"schema_version": 2, "stage": stage.upper(), "mode": "stage", "verdict": "PASSED"}
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(minimal) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
def test_packet_accepts_producer_shaped_backup_artifacts(
    tmp_path: Path, platform: str, stage: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / f"{platform}-{stage}-backups", stage
    )
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    evidence["artifacts"]["documents"].extend(
        [{"path": "legacy.fcbak", "size": 2}, {"path": "numbered.FCStd1", "size": 3}]
    )
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    assert validate_packet(packet)["platform"] == platform


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize(
    "defect",
    [
        "orphan_canonical", "orphan_copy", "source_digest", "commit_relation",
        "gitlink", "readiness_active", "readiness_missing_count", "readiness_extra_document",
    ],
)
def test_packet_rejects_exact_evidence_bindings(
    tmp_path: Path, platform: str, stage: str, defect: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / f"{platform}-{stage}-{defect}", stage
    )
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = _stage_evidence(stage.upper())
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
        evidence["environment"]["git"]["recorded_gitlink"] = "d" * 40
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
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize(
    "defect",
    ["binary_digest", "save_digest", "active_bool", "ordinary_size_bool", "backup_size_bool", "provenance_bool", "fingerprint_size_bool", "fingerprint_mtime_bool"],
)
def test_packet_rejects_boolean_and_nonhex_decisive_evidence(
    tmp_path: Path, platform: str, stage: str, defect: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / f"{platform}-{stage}-{defect}", stage
    )
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = _stage_evidence(stage.upper())
    if defect == "binary_digest":
        evidence["environment"]["binary_fingerprint"]["FreeCAD.exe"]["sha256"] = "Z" * 64
    elif defect == "save_digest":
        evidence["saves"][0]["sha256_after"] = "not-a-digest"
    elif defect == "active_bool":
        evidence["pause_resume"]["readiness_while_paused"]["documents"][0]["active_write_count"] = False
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
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("defect", ["local_identity", "orphan_proof", "save_operation", "copy_collision", "conflict_child"])
def test_completed_evidence_rejects_unbound_cycle_save_and_conflict_children(
    stage: str, defect: str
) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    if defect == "local_identity":
        evidence["cycles"][0]["local_actions"][0].pop("operation_id")
    elif defect == "orphan_proof":
        evidence["personal_action_proofs"].append(
            dict(evidence["personal_action_proofs"][0], index=len(evidence["personal_action_proofs"]))
        )
    elif defect == "save_operation":
        evidence["saves"][0]["actual_save_operations"] = [{"truthful": True}]
    elif defect == "copy_collision":
        evidence["saves"][1]["save_copy"]["destination"] = evidence["saves"][0]["save_copy"]["destination"]
    else:
        evidence["conflicts"]["same_property"].pop("refusal")
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("defect", ["personal_proof", "save_bind", "conflict_bind"])
def test_packets_reject_unbound_personal_save_and_conflict_evidence(
    tmp_path: Path, platform: str, stage: str, defect: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / f"{platform}-{stage}-{defect}", stage
    )
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = _stage_evidence(stage.upper())
    if defect == "personal_proof":
        evidence["cycles"][0]["local_actions"][0]["personal_action_proof_index"] = 1
    elif defect == "save_bind":
        evidence["saves"][0]["actual_save_operations"][2]["destination"] = "other.FCStd"
    else:
        evidence["conflicts"]["independent_property"]["beta_revision_after"] += 1
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("defect", ["cycle_field", "remote_null", "remote_failure", "replay", "save_result", "conflict_revision", "conflict_readiness", "independent_values"])
def test_completed_evidence_rejects_remaining_cycle_conflict_and_save_false_greens(
    stage: str, defect: str
) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    cycle = evidence["cycles"][0]
    if defect == "cycle_field":
        cycle.pop("revisions_after")
    elif defect == "remote_null":
        cycle["remote_actions"][0]["operation_id"] = None
    elif defect == "remote_failure":
        cycle["remote_actions"][2]["result_envelope"] = {"success": False}
    elif defect == "replay":
        cycle["remote_actions"][1]["operation_id"] = "other"
    elif defect == "save_result":
        evidence["saves"][0]["actual_save_operations"][0].pop("result")
    elif defect == "conflict_revision":
        same = evidence["conflicts"]["same_property"]
        same["current_revisions"] = dict(same["expected_revisions"])
    elif defect == "conflict_readiness":
        evidence["conflicts"]["same_property"]["readiness"] = {"success": True}
    else:
        evidence["conflicts"]["independent_property"]["beta_value"] = 29
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("defect", ["cycle", "save", "conflict"])
def test_packets_reject_remaining_bound_semantic_children(
    tmp_path: Path, platform: str, stage: str, defect: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / f"{platform}-{stage}-{defect}", stage
    )
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    if defect == "cycle":
        evidence["cycles"][0]["typed_mutation"]["revision_after"] += 1
    elif defect == "save":
        evidence["saves"][0]["actual_save_operations"][1].pop("result")
    else:
        evidence["conflicts"]["same_property"]["changed_semantic_keys"] = ["ObjectNonsense"]
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
def test_packet_rejects_temporally_disconnected_binary_provenance(
    tmp_path: Path, platform: str, stage: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / f"{platform}-{stage}", stage
    )
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    provenance = evidence["environment"]["build_provenance"]
    provenance["head_committed_utc"] = "2000-01-01T00:00:00+00:00"
    binary = provenance["binaries"]["FreeCAD.exe"]
    binary["mtime_utc"] = "2030-01-01T00:00:00+00:00"
    binary["commits_not_in_binary"] = [evidence["environment"]["git"]["parent_commit"]]
    binary["predates_head"] = True
    provenance["binaries_predating_head"] = ["FreeCAD.exe"]
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("position", range(10))
def test_completed_evidence_rejects_each_omitted_local_cycle_action(
    stage: str, position: int
) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    evidence["cycles"][0]["local_actions"].pop(position)
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("defect", ["document", "revisions", "readiness", "begin", "reorder", "nested_conflict", "revision_jump"])
def test_completed_evidence_rejects_bound_cycle_and_conflict_corruptions(defect: str) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence("A")
    cycle = evidence["cycles"][0]
    if defect == "document":
        cycle.pop("document")
    elif defect == "revisions":
        cycle["revisions_before"][0]["key"] = "ObjectNonsense"
    elif defect == "readiness":
        cycle["readiness"]["documents"][0]["document"] = "Other"
    elif defect == "begin":
        cycle["remote_actions"].pop(0)
    elif defect == "reorder":
        cycle["remote_actions"][0], cycle["remote_actions"][1] = cycle["remote_actions"][1], cycle["remote_actions"][0]
    elif defect == "nested_conflict":
        evidence["conflicts"]["same_property"]["refusal"]["data"]["current_revisions"] = {"ObjectProperty:Alpha:Length": 99}
    else:
        evidence["conflicts"]["same_property"]["current_revisions"] = {"ObjectProperty:Alpha:Length": 999}
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage="A", repo_root=REPO_ROOT)


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("defect", ["malformed", "naive", "nonzero_offset", "reversed"])
def test_completed_evidence_rejects_noncanonical_or_noncausal_shutdown_timestamps(
    stage: str, defect: str
) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    shutdown = evidence["shutdown"]
    if defect == "malformed":
        for key in shutdown:
            if key.endswith("_utc"):
                shutdown[key] = "not-a-timestamp"
    elif defect == "naive":
        shutdown["documents_closed_utc"] = "2026-08-25T00:00:01"
    elif defect == "nonzero_offset":
        shutdown["documents_closed_utc"] = "2026-08-25T00:00:01+01:00"
    else:
        shutdown["documents_closed_utc"] = "2026-08-25T00:00:03+00:00"
        shutdown["rpc_admission_closed_utc"] = "2026-08-25T00:00:02+00:00"
    with pytest.raises(ValueError, match="shutdown"):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("defect", ["malformed", "reversed"])
def test_packets_reject_noncanonical_or_noncausal_shutdown_timestamps(
    tmp_path: Path, platform: str, stage: str, defect: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / f"{platform}-{stage}-{defect}", stage
    )
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    if defect == "malformed":
        for key in evidence["shutdown"]:
            if key.endswith("_utc"):
                evidence["shutdown"][key] = "not-a-timestamp"
    else:
        evidence["shutdown"]["documents_closed_utc"] = "2026-08-25T00:00:03+00:00"
        evidence["shutdown"]["rpc_admission_closed_utc"] = "2026-08-25T00:00:02+00:00"
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError, match="shutdown"):
        validate_packet(packet)


def _reuse_cycle_identity_in_scenario(evidence: dict[str, Any], kind: str) -> None:
    """Make a scenario lane reuse an otherwise valid cycle identity."""

    cycle_begin = evidence["cycles"][0]["remote_actions"][0]
    scenario_ops = evidence["conflicts"]["same_property"]["stage_operations"]
    scenario_begin = scenario_ops["begin"]
    if kind == "operation":
        operation_id = cycle_begin["operation_id"]
        scenario_begin["operation_id"] = operation_id
        scenario_begin["parameters"]["operation_id"] = operation_id
        return
    session_id = cycle_begin["result_envelope"]["session_id"]
    scenario_begin["result"]["session_id"] = session_id
    scenario_ops["refused_commit"]["parameters"]["session_id"] = session_id


def _reuse_remote_identity_in_local_stream(evidence: dict[str, Any], stream: str) -> None:
    """Reuse a remote cycle ID while preserving the local proof binding."""

    remote_id = evidence["cycles"][0]["remote_actions"][0]["operation_id"]
    if stream == "cycle":
        action = evidence["cycles"][0]["local_actions"][0]
    elif stream == "out_of_cycle":
        action = evidence["out_of_cycle_local_actions"][0]
    else:
        raise AssertionError(stream)
    action["operation_id"] = remote_id
    proof = evidence["personal_action_proofs"][action["personal_action_proof_index"]]
    proof["operation_id"] = remote_id


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("kind", ["operation", "session"])
def test_completed_evidence_rejects_cross_lane_operation_and_session_reuse(
    stage: str, kind: str
) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    _reuse_cycle_identity_in_scenario(evidence, kind)
    with pytest.raises(ValueError, match="scenario|identit"):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("kind", ["operation", "session"])
def test_packets_reject_cross_lane_operation_and_session_reuse(
    tmp_path: Path, platform: str, stage: str, kind: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / f"{platform}-{stage}-{kind}", stage
    )
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    _reuse_cycle_identity_in_scenario(evidence, kind)
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError, match="scenario|identit"):
        validate_packet(packet)


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("stream", ["cycle", "out_of_cycle"])
def test_completed_evidence_rejects_remote_identity_reused_by_local_stream(
    stage: str, stream: str
) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    _reuse_remote_identity_in_local_stream(evidence, stream)
    with pytest.raises(ValueError, match="identit"):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("stream", ["cycle", "out_of_cycle"])
def test_packets_reject_remote_identity_reused_by_local_stream(
    tmp_path: Path, platform: str, stage: str, stream: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / f"{platform}-{stage}-{stream}", stage
    )
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    _reuse_remote_identity_in_local_stream(evidence, stream)
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError, match="identit"):
        validate_packet(packet)


def _advance_exact_revision_targets(
    cycle: dict[str, Any], mutation: dict[str, Any], key: str
) -> None:
    """Advance one canonical key in both observations with a clear fixture error."""

    for observation in (cycle["revisions_after"], mutation["revisions_after"]):
        targets = [item for item in observation if item.get("key") == key]
        assert len(targets) == 1, (
            f"fixture must contain canonical revision key {key!r} exactly once; "
            f"found {[item.get('key') for item in observation]!r}"
        )
        targets[0]["revision"] += 1


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize(
    "defect",
    [
        "scalar_beta_mismatch",
        ALPHA_PROPERTY_KEY,
        ALPHA_MODEL_KEY,
        BETA_MODEL_KEY,
    ],
)
def test_completed_evidence_rejects_unbound_or_advanced_cycle_revisions(
    stage: str, defect: str
) -> None:
    """Typed-edit scalar revisions and all untouched keys bind to observations."""

    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    cycle = evidence["cycles"][0]
    mutation = cycle["typed_mutation"]
    if defect == "scalar_beta_mismatch":
        mutation["revision_before"] = 100
        mutation["revision_after"] = 101
    else:
        _advance_exact_revision_targets(cycle, mutation, defect)
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize(
    "defect",
    [
        "scalar_beta_mismatch",
        ALPHA_PROPERTY_KEY,
        ALPHA_MODEL_KEY,
        BETA_MODEL_KEY,
    ],
)
def test_packets_reject_unbound_or_advanced_cycle_revisions(
    tmp_path: Path, platform: str, stage: str, defect: str
) -> None:
    """The Windows/Linux retained-packet routes share the exact cycle guard."""

    from tests.gui.part3.stage_gate_runner import validate_packet

    safe_defect = defect.replace(":", "-")
    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / f"{platform}-{stage}-{safe_defect}", stage
    )
    evidence = _stage_evidence(stage.upper())
    cycle = evidence["cycles"][0]
    mutation = cycle["typed_mutation"]
    if defect == "scalar_beta_mismatch":
        mutation["revision_before"] = 100
        mutation["revision_after"] = 101
    else:
        _advance_exact_revision_targets(cycle, mutation, defect)
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
def test_packets_reject_each_omitted_local_cycle_action(
    tmp_path: Path, platform: str, stage: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet_factory = _windows_packet if platform == "windows" else _linux_packet
    for position in range(10):
        packet = packet_factory(tmp_path / f"{platform}-{stage}-{position}", stage)
        evidence = _stage_evidence(stage.upper())
        evidence["cycles"][0]["local_actions"].pop(position)
        evidence_path = Path(packet["artifacts"]["evidence"]["path"])
        packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")
        with pytest.raises(ValueError):
            validate_packet(packet)


@pytest.mark.parametrize("stage", ["a", "b"])
def test_linux_packet_rejects_ambiguous_aftermath_sections(tmp_path: Path, stage: str) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path / stage, stage)
    path = Path(packet["artifacts"]["aftermath"]["path"])
    packet["artifacts"]["aftermath"] = _write(
        path,
        "pytest_rc=1\n# pytest_rc=0\nfreecad_or_launcher_leftovers_begin\n"
        "freecad_or_launcher_leftovers_end\nfreecad_or_launcher_leftovers_begin\n"
        "freecad_or_launcher_leftovers_end\nport_9875_listeners_begin\n"
        "port_9875_listeners_end\n",
    )
    with pytest.raises(ValueError, match="aftermath"):
        validate_packet(packet)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
def test_packet_rejects_junit_for_unrelated_passing_test(
    tmp_path: Path, platform: str, stage: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / f"{platform}-{stage}", stage
    )
    junit_path = Path(packet["artifacts"]["junit"]["path"])
    packet["artifacts"]["junit"] = _write(
        junit_path,
        '<testsuite tests="1" failures="0" errors="0" skipped="0">'
        '<testcase classname="unrelated" name="test_passing" /></testsuite>\n',
    )
    with pytest.raises(ValueError, match="JUnit"):
        validate_packet(packet)


def test_linux_runner_uses_current_handoff_when_newer_decoy_run_exists() -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    script = runner._linux_stage_script("a")
    assert "PART3_STAGE_EVIDENCE_HANDOFF" in script
    assert "find /tmp" not in script
    assert "coordinator_return_code" in script
    assert 'type(payload.get("coordinator_return_code")) is int' in script
    assert 'payload.get("coordinator_return_code") == 0' not in script
    assert "shutil.copy2(evidence, evidence_target)" in script
    assert "shutil.copy2(launcher, launcher_target)" in script


@pytest.mark.parametrize("mutation", ["missing", "malformed"])
def test_linux_runner_rejects_missing_or_malformed_handoff(
    tmp_path: Path, mutation: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path)
    handoff_path = Path(packet["artifacts"]["handoff"]["path"])
    if mutation == "missing":
        handoff_path.unlink()
    else:
        packet["artifacts"]["handoff"] = _write(handoff_path, "not-json\n")
    with pytest.raises(ValueError):
        validate_packet(packet)


def test_windows_runner_timeout_is_bounded_and_retained(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    class TimedProcess:
        pid = 41001

        def wait(self, *, timeout: int) -> int:
            raise subprocess.TimeoutExpired("pytest", timeout)

        def kill(self) -> None:
            pass

    cleanup = {
        "root_pid": TimedProcess.pid,
        "owned_pids": [TimedProcess.pid, 41002],
        "command": ["taskkill", "/PID", str(TimedProcess.pid), "/T", "/F"],
        "return_code": 0,
        "timed_out": False,
        "surviving_owned_pids": [],
        "passed": True,
    }
    monkeypatch.setattr(runner.subprocess, "Popen", lambda *_args, **_kwargs: TimedProcess())
    monkeypatch.setattr(runner, "_terminate_owned_windows_tree", lambda _process: cleanup)
    result = runner.run_windows(
        repo_root=REPO_ROOT, output_dir=tmp_path, stage="a", python_executable=Path(sys.executable)
    )
    packet = json.loads((tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    assert result == 124
    assert packet["execution"]["classification"] == "timeout"
    assert packet["execution"]["timeout_seconds"] == runner.WINDOWS_STAGE_TIMEOUT_SECONDS
    assert packet["execution"]["cleanup"] == cleanup
    assert packet["execution"]["aftermath"]["passed"] is True
    assert (tmp_path / "stage-a-runner.log").is_file()


def test_windows_runner_timeout_retains_surviving_owned_descendant(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    class TimedProcess:
        pid = 42001

        def wait(self, *, timeout: int) -> int:
            raise subprocess.TimeoutExpired("pytest", timeout)

        def kill(self) -> None:
            pass

    cleanup = {
        "root_pid": TimedProcess.pid,
        "owned_pids": [TimedProcess.pid, 42002],
        "command": ["taskkill", "/PID", str(TimedProcess.pid), "/T", "/F"],
        "return_code": 0,
        "timed_out": False,
        "surviving_owned_pids": [42002],
        "passed": False,
    }
    monkeypatch.setattr(runner.subprocess, "Popen", lambda *_args, **_kwargs: TimedProcess())
    monkeypatch.setattr(runner, "_terminate_owned_windows_tree", lambda _process: cleanup)
    assert runner.run_windows(
        repo_root=REPO_ROOT, output_dir=tmp_path, stage="a", python_executable=Path(sys.executable)
    ) == 124
    packet = json.loads((tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    assert packet["execution"]["cleanup"]["surviving_owned_pids"] == [42002]
    assert packet["execution"]["aftermath"]["passed"] is False


def test_windows_owned_timeout_helper_terminates_only_owned_tree(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    root_pid = 43001
    descendant_pid = 43002
    commands: list[list[str]] = []

    class Process:
        pid = root_pid

        def wait(self, *, timeout: float) -> int:
            assert 0 < timeout <= runner.WINDOWS_TIMEOUT_CLEANUP_SECONDS
            return 0

        def kill(self) -> None:
            raise AssertionError("Windows taskkill tree termination must be used")

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        commands.append(list(command))
        if command[0] == "powershell":
            if "ParentProcessId" in command[4]:
                return subprocess.CompletedProcess(command, 0, json.dumps([root_pid, descendant_pid]), "")
            return subprocess.CompletedProcess(command, 0, "[]", "")
        if command[0] == "taskkill":
            assert command == ["taskkill", "/PID", str(root_pid), "/T", "/F"]
            return subprocess.CompletedProcess(command, 0, "SUCCESS", "")
        raise AssertionError(f"unexpected command: {command}")

    monkeypatch.setattr(runner.os, "name", "nt")
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    cleanup = runner._terminate_owned_windows_tree(Process())
    assert cleanup["owned_pids"] == [root_pid, descendant_pid]
    assert cleanup["inventory"]["complete"] is True
    assert cleanup["surviving_owned_pids"] == []
    assert cleanup["passed"] is True
    assert ["taskkill", "/PID", str(root_pid), "/T", "/F"] in commands


@pytest.mark.parametrize(
    ("mode", "diagnostic"),
    [("nonzero", "inventory_return_code"), ("malformed", "inventory_JSONDecodeError"), ("timeout", "inventory_timeout")],
)
def test_windows_timeout_inventory_failures_cannot_pass_cleanup(
    monkeypatch: pytest.MonkeyPatch, mode: str, diagnostic: str
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    root_pid = 44001

    class Process:
        pid = root_pid

        def wait(self, *, timeout: int) -> int:
            return 0

        def kill(self) -> None:
            raise AssertionError("taskkill must remain the Windows termination path")

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        if command[0] == "powershell":
            if mode == "timeout":
                raise subprocess.TimeoutExpired(command, runner.WINDOWS_TIMEOUT_CLEANUP_SECONDS)
            if mode == "nonzero":
                return subprocess.CompletedProcess(command, 1, "", "access denied")
            return subprocess.CompletedProcess(command, 0, "not-json", "")
        if command[0] == "taskkill":
            return subprocess.CompletedProcess(command, 0, "SUCCESS", "")
        return subprocess.CompletedProcess(command, 0, "INFO: No tasks are running", "")

    monkeypatch.setattr(runner.os, "name", "nt")
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    cleanup = runner._terminate_owned_windows_tree(Process())
    assert cleanup["inventory"]["complete"] is False
    assert cleanup["inventory"]["diagnostic"] == diagnostic
    assert cleanup["passed"] is False


@pytest.mark.parametrize("mode", ["rc1", "malformed", "localized", "timeout"])
def test_windows_aftermath_query_uncertainty_cannot_pass_cleanup(
    monkeypatch: pytest.MonkeyPatch, mode: str
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    root_pid = 45001

    class Process:
        pid = root_pid

        def wait(self, *, timeout: int) -> int:
            return 0

        def kill(self) -> None:
            raise AssertionError("taskkill must remain the Windows termination path")

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        if command[0] == "taskkill":
            return subprocess.CompletedProcess(command, 0, "SUCCESS", "")
        if "ParentProcessId" in command[4]:
            return subprocess.CompletedProcess(command, 0, json.dumps([root_pid]), "")
        if mode == "timeout":
            raise subprocess.TimeoutExpired(command, runner.WINDOWS_TIMEOUT_CLEANUP_SECONDS)
        if mode == "rc1":
            return subprocess.CompletedProcess(command, 1, "", "access denied")
        if mode == "malformed":
            return subprocess.CompletedProcess(command, 0, "not-json", "")
        return subprocess.CompletedProcess(command, 0, "[]", "localized enumeration warning")

    monkeypatch.setattr(runner.os, "name", "nt")
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    cleanup = runner._terminate_owned_windows_tree(Process())
    assert cleanup["inventory"]["complete"] is True
    assert cleanup["aftermath"]["complete"] is False
    assert cleanup["passed"] is False


def test_windows_initial_inventory_stderr_cannot_pass_cleanup(
    monkeypatch: pytest.MonkeyPatch,
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    class Process:
        pid = 46001

        def wait(self, *, timeout: int) -> int:
            return 0

        def kill(self) -> None:
            raise AssertionError("incomplete inventory must not terminate broadly")

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        if command[0] == "powershell":
            return subprocess.CompletedProcess(command, 0, "[46001]", "enumeration error")
        assert command[0] == "taskkill"
        return subprocess.CompletedProcess(command, 0, "SUCCESS", "")

    monkeypatch.setattr(runner.os, "name", "nt")
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    cleanup = runner._terminate_owned_windows_tree(Process())
    assert cleanup["inventory"]["complete"] is False
    assert cleanup["inventory"]["diagnostic"] == "inventory_stderr"
    assert cleanup["passed"] is False


def test_linux_release_barrier_timeout_never_starts_pytest() -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    script = runner._linux_stage_script("a")
    assert "release_barrier_timeout" in script
    assert script.index("release_deadline") < script.index("python3 -m pytest")
    assert script.index("exit 124") < script.index("python3 -m pytest")


def test_linux_release_barrier_expiry_is_structured_and_skips_artifacts(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    container_id = "c" * 64
    calls: list[list[str]] = []

    def fake_json(command: list[str], *, phase: str) -> dict[str, Any]:
        del phase
        if command[:3] == ["docker", "image", "inspect"]:
            return {"Id": "sha256:" + "a" * 64}
        expired = _inspect(container_id)
        expired["State"] = {"Running": False, "ExitCode": 124}
        return expired

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(list(command))
        if command[:2] == ["docker", "run"]:
            return subprocess.CompletedProcess(command, 0, container_id + "\n", "")
        return subprocess.CompletedProcess(command, 0, "", "")

    def fake_probe(_container: str, destination: str) -> dict[str, Any]:
        return {
            "path": destination, "container_id": container_id,
            "command": ["docker", "exec", container_id, "probe", destination],
            "return_code": 73, "errno": 30, "errno_name": "EROFS",
            "strerror": "Read-only file system", "refused": True,
            "pre_inspect": _inspect(container_id), "post_inspect": _inspect(container_id),
        }

    def fake_result(command: list[str], *, timeout: int | None = None) -> dict[str, Any]:
        calls.append(list(command))
        if command[:3] == ["docker", "rm", "-f"]:
            return {"command": list(command), "return_code": 0, "stdout": "", "stderr": "", "timed_out": False}
        return {"command": list(command), "return_code": 1, "stdout": "", "stderr": f"Error: No such object: {command[-1]}", "timed_out": False}

    monkeypatch.setattr(runner, "_docker_json", fake_json)
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    monkeypatch.setattr(runner, "_run_erofs_probe", fake_probe)
    monkeypatch.setattr(runner, "_docker_result", fake_result)
    assert runner.run_linux_docker(
        repo_root=REPO_ROOT,
        build_root=REPO_ROOT / "build",
        output_dir=tmp_path,
        stage="a",
        image="synthetic",
    ) == 124
    packet = json.loads((tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    assert packet["execution"]["phase"] == "release-barrier"
    assert packet["execution"]["classification"] == "timeout"
    assert packet["execution"]["pytest_started"] is False
    assert packet["execution"]["cleanup_verified"] is True
    assert not (tmp_path / "stage-a-junit.xml").exists()
    assert ["docker", "wait", container_id] not in calls
    assert all("pytest" not in command for command in calls)


def test_linux_probe_preinspect_barrier_expiry_never_executes_probe(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    container_id = "d" * 64
    calls: list[list[str]] = []
    inspections = 0

    def fake_json(command: list[str], *, phase: str) -> dict[str, Any]:
        nonlocal inspections
        if phase == "image-inspect":
            return {"Id": "sha256:" + "a" * 64}
        inspections += 1
        inspect = _inspect(container_id)
        if phase == "probe-pre-inspect":
            inspect["State"] = {"Running": False, "ExitCode": 124}
        return inspect

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(list(command))
        assert command[:2] == ["docker", "run"]
        return subprocess.CompletedProcess(command, 0, container_id + "\n", "")

    def fake_result(command: list[str], *, timeout: int | None = None) -> dict[str, Any]:
        calls.append(list(command))
        if command[:3] == ["docker", "rm", "-f"]:
            return {"command": list(command), "return_code": 0, "stdout": "", "stderr": "", "timed_out": False}
        return {"command": list(command), "return_code": 1, "stdout": "[]\n", "stderr": f"Error: No such object: {command[-1]}", "timed_out": False}

    monkeypatch.setattr(runner, "_docker_json", fake_json)
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    monkeypatch.setattr(runner, "_docker_result", fake_result)
    assert runner.run_linux_docker(
        repo_root=REPO_ROOT, build_root=REPO_ROOT / "build", output_dir=tmp_path,
        stage="a", image="synthetic",
    ) == 124
    packet = json.loads((tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    assert packet["execution"]["phase"] == "release-barrier"
    assert packet["execution"]["pytest_started"] is False
    assert packet["execution"]["cleanup_verified"] is True
    assert inspections >= 2
    assert not any(command[:2] == ["docker", "exec"] for command in calls)


@pytest.mark.parametrize(
    ("mode", "expected_phase"),
    [("inspect", "live-inspect"), ("exec", "probe-exec")],
)
def test_linux_control_timeout_retains_bounded_cleanup(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, mode: str, expected_phase: str
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    container_id = "e" * 64

    def fake_json(command: list[str], *, phase: str) -> dict[str, Any]:
        if phase == "image-inspect":
            return {"Id": "sha256:" + "a" * 64}
        if mode == "inspect" and phase == "live-inspect":
            raise runner.DockerControlTimeout(phase)
        return _inspect(container_id)

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        if command[:2] == ["docker", "run"]:
            return subprocess.CompletedProcess(command, 0, container_id + "\n", "")
        if command[:2] == ["docker", "exec"]:
            raise subprocess.TimeoutExpired(command, runner.LINUX_DOCKER_CONTROL_TIMEOUT_SECONDS)
        raise AssertionError(f"unexpected unbounded command: {command}")

    def fake_result(command: list[str], *, timeout: int | None = None) -> dict[str, Any]:
        if command[:3] == ["docker", "rm", "-f"]:
            return {"command": list(command), "return_code": 0, "stdout": "", "stderr": "", "timed_out": False}
        return {"command": list(command), "return_code": 1, "stdout": "[]\n", "stderr": f"Error: No such object: {command[-1]}", "timed_out": False}

    monkeypatch.setattr(runner, "_docker_json", fake_json)
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    monkeypatch.setattr(runner, "_docker_result", fake_result)
    assert runner.run_linux_docker(
        repo_root=REPO_ROOT, build_root=REPO_ROOT / "build", output_dir=tmp_path,
        stage="a", image="synthetic",
    ) == 124
    packet = json.loads((tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    assert packet["execution"]["classification"] == "timeout"
    assert packet["execution"]["phase"] == expected_phase
    assert packet["execution"]["cleanup_verified"] is True


def test_linux_docker_wait_timeout_forces_cleanup_and_fails(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    calls: list[list[str]] = []
    container_id = "b" * 64

    def fake_result(command, *, timeout=None):
        calls.append(list(command))
        if command[:2] == ["docker", "wait"]:
            return {"command": list(command), "return_code": 124, "stdout": "", "stderr": "", "timed_out": True, "timeout_seconds": timeout}
        if command[:3] == ["docker", "rm", "-f"]:
            return {"command": list(command), "return_code": 0, "stdout": "", "stderr": "", "timed_out": False}
        return {
            "command": list(command), "return_code": 1, "stdout": "",
            "stderr": f"Error: No such object: {command[-1]}", "timed_out": False,
        }

    packet = {"docker": {}}
    monkeypatch.setattr(runner, "_docker_result", fake_result)
    assert runner._cleanup_linux_container(packet, container_id=container_id, container_name="part3-a") is True
    assert packet["docker"]["cleanup"]["command"] == ["docker", "rm", "-f", container_id]
    assert ["docker", "inspect", "part3-a"] in calls


@pytest.mark.parametrize("daemon_accepted", [True, False])
def test_linux_launch_timeout_reconciles_exact_generated_name(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, daemon_accepted: bool
) -> None:
    """A timed-out CLI launch cannot leave its generated --name behind."""

    from tests.gui.part3 import stage_gate_runner as runner

    calls: list[list[str]] = []

    def fake_json(command: list[str], *, phase: str) -> dict[str, Any]:
        assert phase == "image-inspect"
        assert command[:3] == ["docker", "image", "inspect"]
        return {"Id": "sha256:" + "a" * 64}

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(list(command))
        assert command[:2] == ["docker", "run"]
        # In the accepted case this models a daemon that created the named
        # container before the client timed out.  The runner deliberately has
        # no id to trust, so reconciliation must use only --name.
        raise subprocess.TimeoutExpired(command, runner.LINUX_DOCKER_CONTROL_TIMEOUT_SECONDS)

    def fake_result(command: list[str], *, timeout: int | None = None) -> dict[str, Any]:
        del timeout
        calls.append(list(command))
        subject = command[-1]
        if command[:3] == ["docker", "rm", "-f"]:
            if daemon_accepted:
                return {"command": list(command), "return_code": 0, "stdout": subject + "\n", "stderr": "", "timed_out": False}
            return {"command": list(command), "return_code": 1, "stdout": "", "stderr": f"Error: No such container: {subject}", "timed_out": False}
        return {"command": list(command), "return_code": 1, "stdout": "[]\n", "stderr": f"Error: No such object: {subject}", "timed_out": False}

    monkeypatch.setattr(runner, "_docker_json", fake_json)
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    monkeypatch.setattr(runner, "_docker_result", fake_result)
    assert runner.run_linux_docker(
        repo_root=REPO_ROOT, build_root=REPO_ROOT / "build", output_dir=tmp_path,
        stage="a", image="synthetic",
    ) == 124
    packet = json.loads((tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    name = packet["docker"]["launch_timeout"]["container_name"]
    assert packet["execution"]["cleanup_verified"] is True
    assert packet["docker"]["cleanup"]["command"] == ["docker", "rm", "-f", name]
    assert set(packet["docker"]["cleanup"]["post_cleanup_absence"]) == {"name"}
    assert ["docker", "inspect", name] in calls


@pytest.mark.parametrize("failure", ["daemon", "permission", "context", "timeout"])
def test_linux_launch_timeout_unverified_cleanup_cannot_pass(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, failure: str
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    def fake_json(_command: list[str], *, phase: str) -> dict[str, Any]:
        assert phase == "image-inspect"
        return {"Id": "sha256:" + "a" * 64}

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        raise subprocess.TimeoutExpired(command, runner.LINUX_DOCKER_CONTROL_TIMEOUT_SECONDS)

    def fake_result(command: list[str], *, timeout: int | None = None) -> dict[str, Any]:
        subject = command[-1]
        if failure == "timeout":
            return {"command": list(command), "return_code": 124, "stdout": "", "stderr": "", "timed_out": True, "timeout_seconds": timeout}
        message = {
            "daemon": "Cannot connect to the Docker daemon",
            "permission": "permission denied while trying to connect",
            "context": "context deadline exceeded",
        }[failure]
        return {"command": list(command), "return_code": 1, "stdout": "", "stderr": f"{message}: {subject}", "timed_out": False}

    monkeypatch.setattr(runner, "_docker_json", fake_json)
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    monkeypatch.setattr(runner, "_docker_result", fake_result)
    assert runner.run_linux_docker(
        repo_root=REPO_ROOT, build_root=REPO_ROOT / "build", output_dir=tmp_path,
        stage="a", image="synthetic",
    ) == 124
    packet = json.loads((tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    assert packet["execution"]["cleanup_verified"] is False
    assert packet["docker"]["cleanup"] is not None


@pytest.mark.parametrize("return_code", [127, 125])
def test_linux_nonzero_launch_reconciles_generated_name(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, return_code: int
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    calls: list[list[str]] = []

    def fake_json(_command: list[str], *, phase: str) -> dict[str, Any]:
        assert phase == "image-inspect"
        return {"Id": "sha256:" + "a" * 64}

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(list(command))
        return subprocess.CompletedProcess(command, return_code, "", "OCI runtime start failed")

    def fake_result(command: list[str], *, timeout: int | None = None) -> dict[str, Any]:
        del timeout
        calls.append(list(command))
        subject = command[-1]
        if command[:3] == ["docker", "rm", "-f"]:
            # 127 models a daemon-created container that then failed start;
            # 125 models no creation and Docker's exact not-found response.
            if return_code == 127:
                return {"command": list(command), "return_code": 0, "stdout": subject + "\n", "stderr": "", "timed_out": False}
            return {"command": list(command), "return_code": 1, "stdout": "", "stderr": f"Error: No such container: {subject}", "timed_out": False}
        return {"command": list(command), "return_code": 1, "stdout": "[]\n", "stderr": f"Error: No such object: {subject}", "timed_out": False}

    monkeypatch.setattr(runner, "_docker_json", fake_json)
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    monkeypatch.setattr(runner, "_docker_result", fake_result)
    assert runner.run_linux_docker(
        repo_root=REPO_ROOT, build_root=REPO_ROOT / "build", output_dir=tmp_path,
        stage="a", image="synthetic",
    ) == return_code
    packet = json.loads((tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    name = packet["docker"]["launch_failure"]["container_name"]
    assert packet["docker"]["launch_failure"]["return_code"] == return_code
    assert packet["execution"]["cleanup_verified"] is True
    assert packet["docker"]["cleanup"]["command"] == ["docker", "rm", "-f", name]
    assert ["docker", "inspect", name] in calls


@pytest.mark.parametrize("stdout", ["", "B" * 64 + "\n", "a" * 64 + "\nextra\n"])
def test_linux_malformed_success_launch_stdout_reconciles_generated_name(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, stdout: str
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    calls: list[list[str]] = []
    def fake_json(_command: list[str], *, phase: str) -> dict[str, Any]:
        assert phase == "image-inspect"
        return {"Id": "sha256:" + "a" * 64}
    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        calls.append(list(command))
        return subprocess.CompletedProcess(command, 0, stdout, "")
    def fake_result(command: list[str], *, timeout: int | None = None) -> dict[str, Any]:
        del timeout
        calls.append(list(command))
        subject = command[-1]
        if command[:3] == ["docker", "rm", "-f"]:
            return {"command": list(command), "return_code": 0, "stdout": subject + "\n", "stderr": "", "timed_out": False}
        return {"command": list(command), "return_code": 1, "stdout": "", "stderr": f"Error: No such object: {subject}", "timed_out": False}
    monkeypatch.setattr(runner, "_docker_json", fake_json)
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    monkeypatch.setattr(runner, "_docker_result", fake_result)
    assert runner.run_linux_docker(repo_root=REPO_ROOT, build_root=REPO_ROOT / "build", output_dir=tmp_path, stage="a", image="synthetic") == 1
    packet = json.loads((tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    name = packet["docker"]["launch_failure"]["container_name"]
    assert packet["docker"]["cleanup"]["command"] == ["docker", "rm", "-f", name]
    assert ["docker", "inspect", name] in calls


def test_linux_nonzero_launch_cleanup_control_error_is_unverified(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    def fake_json(_command: list[str], *, phase: str) -> dict[str, Any]:
        assert phase == "image-inspect"
        return {"Id": "sha256:" + "a" * 64}

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(command, 127, "", "OCI failure")

    def fake_result(command: list[str], *, timeout: int | None = None) -> dict[str, Any]:
        return {"command": list(command), "return_code": 1, "stdout": "", "stderr": "permission denied", "timed_out": False}

    monkeypatch.setattr(runner, "_docker_json", fake_json)
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    monkeypatch.setattr(runner, "_docker_result", fake_result)
    assert runner.run_linux_docker(
        repo_root=REPO_ROOT, build_root=REPO_ROOT / "build", output_dir=tmp_path,
        stage="a", image="synthetic",
    ) == 127
    packet = json.loads((tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    assert packet["execution"]["cleanup_verified"] is False


def test_linux_normal_launch_still_reaches_successful_cleanup(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """The launch-timeout reconciliation does not alter the normal id lane."""

    from tests.gui.part3 import stage_gate_runner as runner

    container_id = "f" * 64

    def fake_json(_command: list[str], *, phase: str) -> dict[str, Any]:
        if phase == "image-inspect":
            return {"Id": "sha256:" + "a" * 64}
        return _inspect(container_id)

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        if command[:2] == ["docker", "run"]:
            return subprocess.CompletedProcess(command, 0, container_id + "\n", "")
        raise AssertionError(f"unexpected command: {command}")

    def fake_probe(_container: str, destination: str) -> dict[str, Any]:
        return {
            "path": destination, "container_id": container_id,
            "command": ["docker", "exec", container_id, "probe", destination],
            "return_code": 73, "errno": 30, "errno_name": "EROFS",
            "strerror": "Read-only file system", "refused": True,
            "pre_inspect": _inspect(container_id), "post_inspect": _inspect(container_id),
        }

    def fake_result(command: list[str], *, timeout: int | None = None) -> dict[str, Any]:
        del timeout
        if command[:2] == ["docker", "wait"]:
            return {"command": list(command), "return_code": 0, "stdout": "0\n", "stderr": "", "timed_out": False}
        if command[:2] == ["docker", "logs"]:
            return {"command": list(command), "return_code": 0, "stdout": "ok\n", "stderr": "", "timed_out": False}
        if command[:3] == ["docker", "rm", "-f"]:
            return {"command": list(command), "return_code": 0, "stdout": "", "stderr": "", "timed_out": False}
        return {"command": list(command), "return_code": 1, "stdout": "[]\n", "stderr": f"Error: No such object: {command[-1]}", "timed_out": False}

    def create_required_artifacts(*_args: object, **_kwargs: object) -> None:
        for suffix, contents in {
            "evidence.json": "{}\n", "launcher.log": "graceful\n",
            "junit.xml": "<testsuites/>\n", "handoff.json": "{}\n",
            "aftermath.txt": "done\n",
        }.items():
            _write(tmp_path / f"stage-a-{suffix}", contents)

    monkeypatch.setattr(runner, "_docker_json", fake_json)
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    monkeypatch.setattr(runner, "_run_erofs_probe", fake_probe)
    monkeypatch.setattr(runner, "_docker_result", fake_result)
    monkeypatch.setattr(runner, "validate_packet", lambda _packet: None)
    original_record = runner._artifact_record

    def recording_artifact(path: Path) -> dict[str, Any]:
        if path.name == "stage-a-evidence.json":
            create_required_artifacts()
        return original_record(path)

    monkeypatch.setattr(runner, "_artifact_record", recording_artifact)
    assert runner.run_linux_docker(
        repo_root=REPO_ROOT, build_root=REPO_ROOT / "build", output_dir=tmp_path,
        stage="a", image="synthetic",
    ) == 0
    packet = json.loads((tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    assert packet["docker"]["container_id"] == container_id
    assert packet["docker"]["cleanup"]["command"] == ["docker", "rm", "-f", container_id]


def test_linux_runner_retains_nonzero_stage_artifacts_and_cleanup_before_returning_code(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    container_id = "7" * 64
    validation_calls: list[dict[str, Any]] = []

    def fake_json(_command: list[str], *, phase: str) -> dict[str, Any]:
        if phase == "image-inspect":
            return {"Id": "sha256:" + "a" * 64}
        return _inspect(container_id)

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        assert command[:2] == ["docker", "run"]
        return subprocess.CompletedProcess(command, 0, container_id + "\n", "")

    def fake_probe(_container: str, destination: str) -> dict[str, Any]:
        return {
            "path": destination,
            "container_id": container_id,
            "command": ["docker", "exec", container_id, "probe", destination],
            "return_code": 73,
            "errno": 30,
            "errno_name": "EROFS",
            "strerror": "Read-only file system",
            "refused": True,
            "pre_inspect": _inspect(container_id),
            "post_inspect": _inspect(container_id),
        }

    def fake_result(
        command: list[str], *, timeout: int | None = None
    ) -> dict[str, Any]:
        del timeout
        if command[:2] == ["docker", "wait"]:
            return {
                "command": list(command),
                "return_code": 0,
                "stdout": "7\n",
                "stderr": "",
                "timed_out": False,
            }
        if command[:2] == ["docker", "logs"]:
            _write(tmp_path / "stage-a-evidence.json", '{"verdict":"FAILED"}\n')
            _write(tmp_path / "stage-a-launcher.log", "retained failure launcher\n")
            _write(
                tmp_path / "stage-a-junit.xml",
                '<testsuite tests="1" failures="1" errors="0" skipped="0" />\n',
            )
            _write(
                tmp_path / "stage-a-handoff.json",
                json.dumps(
                    {
                        "schema_version": 1,
                        "stage": "A",
                        "coordinator_return_code": 7,
                        "evidence_path": "/tmp/failure-evidence.json",
                        "launcher_path": "/tmp/failure-launcher.log",
                    }
                )
                + "\n",
            )
            _write(tmp_path / "stage-a-aftermath.txt", "pytest_rc=7\n")
            return {
                "command": list(command),
                "return_code": 0,
                "stdout": "stage failed\n",
                "stderr": "",
                "timed_out": False,
            }
        if command[:3] == ["docker", "rm", "-f"]:
            return {
                "command": list(command),
                "return_code": 0,
                "stdout": "",
                "stderr": "",
                "timed_out": False,
            }
        return {
            "command": list(command),
            "return_code": 1,
            "stdout": "[]\n",
            "stderr": f"Error: No such object: {command[-1]}",
            "timed_out": False,
        }

    monkeypatch.setattr(runner, "_docker_json", fake_json)
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    monkeypatch.setattr(runner, "_run_erofs_probe", fake_probe)
    monkeypatch.setattr(runner, "_docker_result", fake_result)
    monkeypatch.setattr(
        runner, "validate_packet", lambda packet: validation_calls.append(packet)
    )

    result = runner.run_linux_docker(
        repo_root=REPO_ROOT,
        build_root=REPO_ROOT / "build",
        output_dir=tmp_path,
        stage="a",
        image="synthetic",
    )

    packet = json.loads(
        (tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8")
    )
    assert result == 7
    assert packet["execution"]["return_code"] == 7
    assert packet["execution"]["cleanup_verified"] is True
    assert packet["docker"]["cleanup"]["command"] == [
        "docker",
        "rm",
        "-f",
        container_id,
    ]
    assert "artifact_collection_error" not in packet
    assert set(packet["artifacts"]) >= {
        "evidence",
        "launcher_log",
        "handoff",
        "junit",
        "runner_log",
    }
    assert (tmp_path / "stage-a-evidence.json").read_text(encoding="utf-8") == (
        '{"verdict":"FAILED"}\n'
    )
    assert (tmp_path / "stage-a-launcher.log").read_text(encoding="utf-8") == (
        "retained failure launcher\n"
    )
    assert validation_calls == []


def test_linux_failed_logs_cannot_return_zero_and_retains_cleanup(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    container_id = "9" * 64

    def fake_json(_command: list[str], *, phase: str) -> dict[str, Any]:
        if phase == "image-inspect":
            return {"Id": "sha256:" + "a" * 64}
        return _inspect(container_id)

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        assert command[:2] == ["docker", "run"]
        return subprocess.CompletedProcess(command, 0, container_id + "\n", "")

    def fake_probe(_container: str, destination: str) -> dict[str, Any]:
        return {
            "path": destination, "container_id": container_id,
            "command": ["docker", "exec", container_id, "probe", destination],
            "return_code": 73, "errno": 30, "errno_name": "EROFS",
            "strerror": "Read-only file system", "refused": True,
            "pre_inspect": _inspect(container_id), "post_inspect": _inspect(container_id),
        }

    def fake_result(command: list[str], *, timeout: int | None = None) -> dict[str, Any]:
        del timeout
        if command[:2] == ["docker", "wait"]:
            return {"command": list(command), "return_code": 0, "stdout": "0\n", "stderr": "", "timed_out": False}
        if command[:2] == ["docker", "logs"]:
            return {"command": list(command), "return_code": 1, "stdout": "", "stderr": "daemon log error", "timed_out": False}
        if command[:3] == ["docker", "rm", "-f"]:
            return {"command": list(command), "return_code": 0, "stdout": "", "stderr": "", "timed_out": False}
        return {"command": list(command), "return_code": 1, "stdout": "[]\n", "stderr": f"Error: No such object: {command[-1]}", "timed_out": False}

    monkeypatch.setattr(runner, "_docker_json", fake_json)
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    monkeypatch.setattr(runner, "_run_erofs_probe", fake_probe)
    monkeypatch.setattr(runner, "_docker_result", fake_result)
    assert runner.run_linux_docker(
        repo_root=REPO_ROOT, build_root=REPO_ROOT / "build", output_dir=tmp_path,
        stage="a", image="synthetic",
    ) == 1
    packet = json.loads((tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    assert packet["execution"]["classification"] == "control-failure"
    assert packet["execution"]["phase"] == "docker-logs"
    assert packet["docker"]["control_failures"]["docker-logs"]["stderr"] == "daemon log error"
    assert packet["execution"]["cleanup_verified"] is True


def test_linux_failed_logs_cleanup_control_error_is_unverified(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    container_id = "8" * 64

    def fake_json(_command: list[str], *, phase: str) -> dict[str, Any]:
        return {"Id": "sha256:" + "a" * 64} if phase == "image-inspect" else _inspect(container_id)

    def fake_run(command: list[str], **_kwargs: object) -> subprocess.CompletedProcess[str]:
        return subprocess.CompletedProcess(command, 0, container_id + "\n", "")

    def fake_probe(_container: str, destination: str) -> dict[str, Any]:
        return {"path": destination, "container_id": container_id, "return_code": 73, "errno": 30, "errno_name": "EROFS", "strerror": "Read-only file system", "refused": True, "pre_inspect": _inspect(container_id), "post_inspect": _inspect(container_id)}

    def fake_result(command: list[str], *, timeout: int | None = None) -> dict[str, Any]:
        if command[:2] == ["docker", "wait"]:
            return {"command": list(command), "return_code": 0, "stdout": "0\n", "stderr": "", "timed_out": False}
        if command[:2] == ["docker", "logs"]:
            return {"command": list(command), "return_code": 1, "stdout": "", "stderr": "log failure", "timed_out": False}
        return {"command": list(command), "return_code": 1, "stdout": "", "stderr": "Cannot connect to Docker", "timed_out": False}

    monkeypatch.setattr(runner, "_docker_json", fake_json)
    monkeypatch.setattr(runner.subprocess, "run", fake_run)
    monkeypatch.setattr(runner, "_run_erofs_probe", fake_probe)
    monkeypatch.setattr(runner, "_docker_result", fake_result)
    assert runner.run_linux_docker(
        repo_root=REPO_ROOT, build_root=REPO_ROOT / "build", output_dir=tmp_path,
        stage="a", image="synthetic",
    ) == 1
    packet = json.loads((tmp_path / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    assert packet["execution"]["cleanup_verified"] is False


@pytest.mark.parametrize("failure", ["daemon", "permission", "timeout", "context"])
def test_linux_cleanup_rejects_non_absence_inspect_failures(
    tmp_path: Path, failure: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path / failure)
    for proof in packet["docker"]["cleanup"]["post_cleanup_absence"].values():
        if failure == "timeout":
            proof.update(return_code=124, timed_out=True, stderr="", stdout="")
        elif failure == "permission":
            proof.update(return_code=1, timed_out=False, stderr="permission denied")
        elif failure == "context":
            proof.update(return_code=1, timed_out=False, stderr="current context unavailable")
        else:
            proof.update(return_code=1, timed_out=False, stderr="Cannot connect to the Docker daemon")
    with pytest.raises(ValueError, match="post-cleanup"):
        validate_packet(packet)


def test_linux_cleanup_accepts_exact_no_such_container_outcomes(tmp_path: Path) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path)
    for proof in packet["docker"]["cleanup"]["post_cleanup_absence"].values():
        subject = proof["command"][-1]
        proof["stderr"] = f"Error: No such container: {subject}"
    assert validate_packet(packet)["platform"] == "linux-docker"


def test_linux_cleanup_accepts_installed_docker_empty_list_stdout(tmp_path: Path) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path)
    for proof in packet["docker"]["cleanup"]["post_cleanup_absence"].values():
        proof["stdout"] = "[]\n"
    assert validate_packet(packet)["platform"] == "linux-docker"


@pytest.mark.parametrize("stdout", ["[{}]\n", "not-json\n", "unexpected\n"])
def test_linux_cleanup_rejects_nonempty_nonempty_array_or_malformed_stdout(
    tmp_path: Path, stdout: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path)
    packet["docker"]["cleanup"]["post_cleanup_absence"]["id"]["stdout"] = stdout
    with pytest.raises(ValueError, match="post-cleanup"):
        validate_packet(packet)


@pytest.mark.parametrize(
    "stderr",
    [
        "Error: No such object: another-container; requested part3-b-synthetic",
        "Error: No such object: part3-b-synthetic; Cannot connect to the Docker daemon",
    ],
)
def test_linux_cleanup_rejects_non_exact_not_found_text(
    tmp_path: Path, stderr: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path)
    packet["docker"]["cleanup"]["post_cleanup_absence"]["name"]["stderr"] = stderr
    with pytest.raises(ValueError, match="post-cleanup"):
        validate_packet(packet)


def test_linux_runner_cleanup_failure_cannot_return_success(tmp_path: Path) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path)
    packet["docker"]["cleanup"]["return_code"] = 1
    with pytest.raises(ValueError, match="cleanup"):
        validate_packet(packet)


def test_linux_runner_success_requires_exact_container_id_and_name_absent_after_cleanup(
    tmp_path: Path,
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    for key in ("id", "name"):
        packet = _linux_packet(tmp_path / key)
        packet["docker"]["cleanup"]["post_cleanup_absence"][key]["return_code"] = 0
        with pytest.raises(ValueError, match="post-cleanup"):
            validate_packet(packet)


def _rewrite_packet_evidence(packet: dict[str, Any], mutate: Any) -> None:
    evidence_path = Path(packet["artifacts"]["evidence"]["path"])
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    mutate(evidence)
    packet["artifacts"]["evidence"] = _write(evidence_path, json.dumps(evidence) + "\n")


def test_completed_evidence_rejects_coordinated_unchanged_save_hash_mutation() -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence("A")
    changed = "d" * 64
    evidence["saves"][0]["unchanged_save"]["sha256_after"] = changed
    evidence["saves"][0]["actual_save_operations"][1]["sha256_after"] = changed
    with pytest.raises(ValueError, match="save proof"):
        validate_completed_stage_evidence(evidence, stage="A", repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
def test_packets_reject_coordinated_unchanged_save_hash_mutation(
    tmp_path: Path, platform: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path)
    def mutate(evidence: dict[str, Any]) -> None:
        changed = "d" * 64
        evidence["saves"][0]["unchanged_save"]["sha256_after"] = changed
        evidence["saves"][0]["actual_save_operations"][1]["sha256_after"] = changed
    _rewrite_packet_evidence(packet, mutate)
    with pytest.raises(ValueError, match="save proof"):
        validate_packet(packet)


def _corrupt_scenario_operation_binding(evidence: dict[str, Any], defect: str) -> None:
    same = evidence["conflicts"]["same_property"]["stage_operations"]
    independent = evidence["conflicts"]["independent_property"]["stage_operations"]
    history = evidence["history"]["stage_operations"]
    pause = evidence["pause_resume"]["stage_operations"]
    primary = evidence["cycles"][0]["document"]
    secondary = evidence["cycles"][1]["document"]
    if defect == "begin_dependency":
        same["begin"]["parameters"]["revision_keys"] = [{"kind": "ObjectProperty", "subject": "Ghost", "property_name": "Width"}]
    elif defect == "same_commit":
        same["refused_commit"]["parameters"].update(object_name="Ghost", property_name="Width", value="999")
    elif defect == "history_request":
        history["undo"]["parameters"].update(expected_undo_count=999, expected_undo_head="ghost")
    elif defect == "pause_refusal":
        pause["refused_write"]["parameters"] = {"doc_name": primary}
    elif defect == "paused_read":
        pause["paused_read"]["parameters"]["revision_keys"] = []
    elif defect == "role_swap":
        evidence["conflicts"]["same_property"]["document"] = secondary
    elif defect == "selector_substitution":
        same["selector"] = independent["selector"]
    elif defect == "duplicate_operation_id":
        independent["local_edit"]["operation_id"] = same["local_edit"]["operation_id"]
    elif defect == "duplicate_session":
        session = same["begin"]["result"]["session_id"]
        independent["begin"]["result"]["session_id"] = session
        independent["commit_operation"]["parameters"]["session_id"] = session
    elif defect == "unproven_replay":
        independent["commit_operation_replay"].pop("replay_of_operation_id")
    elif defect == "synthesized_pause":
        pause["document"] = secondary
        pause["selector"] = independent["selector"]
    else:
        raise AssertionError(defect)


def _coordinated_selector_rewrite(evidence: dict[str, Any], document: str) -> None:
    """Rewrite every RPC/scenario selector while leaving GUI snapshots intact."""

    original = next(
        action["parameters"]["doc_selector"]
        for cycle in evidence["cycles"]
        if cycle["document"] == document
        for action in cycle["remote_actions"][:3]
        if action["method"] == "begin_checked_edit"
    )
    ghost = {**original, "document_uid": f"ghost-{document}-uid"}
    for cycle in evidence["cycles"]:
        if cycle["document"] != document:
            continue
        for action in cycle["remote_actions"][:3]:
            action["parameters"]["doc_selector"] = dict(ghost)
    for operations in (
        evidence["conflicts"]["same_property"]["stage_operations"],
        evidence["conflicts"]["independent_property"]["stage_operations"],
        evidence["history"]["stage_operations"],
        evidence["pause_resume"]["stage_operations"],
    ):
        selector = operations.get("selector")
        if not isinstance(selector, dict) or selector.get("document_name") != document:
            continue
        operations["selector"] = dict(ghost)
        for operation in operations.values():
            params = operation.get("parameters") if isinstance(operation, dict) else None
            if isinstance(params, dict) and "doc_selector" in params:
                params["doc_selector"] = dict(ghost)


def _commit_replay_selector_rewrite(evidence: dict[str, Any], document: str) -> None:
    """Corrupt only cycle commit/replay selectors, not begin or GUI snapshots."""

    for cycle in evidence["cycles"]:
        if cycle["document"] != document:
            continue
        begin_selector = cycle["remote_actions"][0]["parameters"]["doc_selector"]
        ghost = {**begin_selector, "document_uid": "ghost-commit-only"}
        for action in cycle["remote_actions"][1:3]:
            action["parameters"]["doc_selector"] = dict(ghost)


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("role", ["primary", "secondary"])
def test_completed_evidence_rejects_coordinated_selector_rewrite(
    stage: str, role: str
) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    document = evidence["cycles"][0 if role == "primary" else 1]["document"]
    _coordinated_selector_rewrite(evidence, document)
    with pytest.raises(ValueError, match="causally bound"):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("role", ["primary", "secondary"])
def test_packets_reject_coordinated_selector_rewrite(
    tmp_path: Path, platform: str, stage: str, role: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    def mutate(evidence: dict[str, Any]) -> None:
        document = evidence["cycles"][0 if role == "primary" else 1]["document"]
        _coordinated_selector_rewrite(evidence, document)
    _rewrite_packet_evidence(packet, mutate)
    with pytest.raises(ValueError, match="causally bound"):
        validate_packet(packet)


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("role", ["primary", "secondary"])
def test_completed_evidence_rejects_commit_replay_only_selector_rewrite(
    stage: str, role: str
) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    document = evidence["cycles"][0 if role == "primary" else 1]["document"]
    _commit_replay_selector_rewrite(evidence, document)
    with pytest.raises(ValueError, match="causally bound"):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("role", ["primary", "secondary"])
def test_packets_reject_commit_replay_only_selector_rewrite(
    tmp_path: Path, platform: str, stage: str, role: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    def mutate(evidence: dict[str, Any]) -> None:
        document = evidence["cycles"][0 if role == "primary" else 1]["document"]
        _commit_replay_selector_rewrite(evidence, document)
    _rewrite_packet_evidence(packet, mutate)
    with pytest.raises(ValueError, match="causally bound"):
        validate_packet(packet)


def _corrupt_final13_boundary(evidence: dict[str, Any], defect: str) -> None:
    if defect == "out_of_cycle_orphan":
        evidence["out_of_cycle_local_actions"][0].pop("personal_action_proof_index")
    elif defect == "paused_wrong_document":
        evidence["pause_resume"]["readiness_while_paused"]["documents"][0]["document"] = evidence["cycles"][1]["document"]
    elif defect == "paused_unrelated_key":
        read = evidence["pause_resume"]["stage_operations"]["paused_read"]
        read["result"]["revisions"][0]["key"] = "ObjectProperty:Ghost:Width"
        evidence["pause_resume"]["stage_operations"]["paused_read_result"] = read["result"]
    else:
        raise AssertionError(defect)


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("defect", ["out_of_cycle_orphan", "paused_wrong_document", "paused_unrelated_key"])
def test_completed_evidence_rejects_final13_operation_and_paused_read_boundaries(
    stage: str, defect: str
) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    _corrupt_final13_boundary(evidence, defect)
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("defect", ["out_of_cycle_orphan", "paused_wrong_document", "paused_unrelated_key"])
def test_packets_reject_final13_operation_and_paused_read_boundaries(
    tmp_path: Path, platform: str, stage: str, defect: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    _rewrite_packet_evidence(packet, lambda evidence: _corrupt_final13_boundary(evidence, defect))
    with pytest.raises(ValueError):
        validate_packet(packet)


def _personal_proof_for_stream(
    evidence: dict[str, Any], stream: str
) -> dict[str, Any]:
    if stream == "cycle":
        action = evidence["cycles"][0]["local_actions"][0]
    elif stream == "out_of_cycle":
        action = evidence["out_of_cycle_local_actions"][0]
    else:
        raise AssertionError(stream)
    proof_index = action["personal_action_proof_index"]
    proof = evidence["personal_action_proofs"][proof_index]
    assert proof["index"] == proof_index
    assert proof["operation_id"] == action["operation_id"]
    return proof


def _corrupt_personal_revision_vector(
    evidence: dict[str, Any], stream: str, defect: str
) -> None:
    proof = _personal_proof_for_stream(evidence, stream)
    document = proof["documents"][0]
    before_revisions = proof["before"][document]["semantic_revisions"]
    after_revisions = proof["after"][document]["semantic_revisions"]
    assert before_revisions is not after_revisions
    assert all(
        before_item is not after_item
        for before_item, after_item in zip(before_revisions, after_revisions)
    )

    def corrupt(revisions: list[dict[str, Any]]) -> None:
        if defect == "unrelated":
            revisions[0]["key"] = "ObjectProperty:Ghost:Width"
        elif defect == "missing":
            revisions.pop()
        elif defect == "duplicate":
            revisions[-1]["key"] = revisions[-2]["key"]
        elif defect == "reordered":
            revisions[0], revisions[1] = revisions[1], revisions[0]
        elif defect == "malformed":
            revisions[0] = {"key": ALPHA_PROPERTY_KEY}
        elif defect == "boolean":
            revisions[0]["revision"] = True
        else:
            raise AssertionError(defect)

    corrupt(before_revisions)
    corrupt(after_revisions)
    assert before_revisions == after_revisions
    assert not stage_revision_vector_is_exact(before_revisions)


@pytest.mark.parametrize("stage", ["A", "B"])
def test_completed_evidence_accepts_exact_personal_revision_vectors(stage: str) -> None:
    """Producer-shaped personal snapshots carry exact ordered revision vectors."""

    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    for stream in ("cycle", "out_of_cycle"):
        proof = _personal_proof_for_stream(evidence, stream)
        for phase in ("before", "after"):
            for snapshot in proof[phase].values():
                assert snapshot["semantic_revisions"] == _personal_revision_vector(
                    10 if snapshot["identity_selector"]["document_uid"] == "uid-primary" else 20
                )
    validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("stream", ["cycle", "out_of_cycle"])
@pytest.mark.parametrize(
    "defect", ["unrelated", "missing", "duplicate", "reordered", "malformed", "boolean"]
)
def test_completed_evidence_rejects_malformed_personal_revision_vectors(
    stage: str, stream: str, defect: str
) -> None:
    """Cycle and out-of-cycle proofs share the exact revision-vector predicate."""

    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)
    _corrupt_personal_revision_vector(evidence, stream, defect)
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
def test_packets_accept_exact_personal_revision_vectors(
    tmp_path: Path, platform: str, stage: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    assert validate_packet(packet)["platform"] == platform


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("stream", ["cycle", "out_of_cycle"])
@pytest.mark.parametrize(
    "defect", ["unrelated", "missing", "duplicate", "reordered", "malformed", "boolean"]
)
def test_packets_reject_malformed_personal_revision_vectors(
    tmp_path: Path, platform: str, stage: str, stream: str, defect: str
) -> None:
    """Retained Windows/Linux packets reject each malformed proof vector."""

    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    assert validate_packet(packet)["platform"] == platform
    _rewrite_packet_evidence(
        packet,
        lambda evidence: _corrupt_personal_revision_vector(evidence, stream, defect),
    )
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("stage", ["A", "B"])
@pytest.mark.parametrize("defect", [
    "begin_dependency", "same_commit", "history_request", "pause_refusal",
    "paused_read", "role_swap", "selector_substitution", "duplicate_operation_id",
    "duplicate_session", "unproven_replay", "synthesized_pause",
])
def test_completed_evidence_rejects_unbound_scenario_operations(defect: str, stage: str) -> None:
    from tests.gui.part3.evidence import validate_completed_stage_evidence

    evidence = _stage_evidence(stage)
    _corrupt_scenario_operation_binding(evidence, defect)
    with pytest.raises(ValueError):
        validate_completed_stage_evidence(evidence, stage=stage, repo_root=REPO_ROOT)


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
@pytest.mark.parametrize("stage", ["a", "b"])
@pytest.mark.parametrize("defect", [
    "begin_dependency", "same_commit", "history_request", "pause_refusal", "paused_read",
    "role_swap", "selector_substitution", "duplicate_operation_id", "duplicate_session",
    "unproven_replay", "synthesized_pause",
])
def test_packets_reject_unbound_scenario_operations(
    tmp_path: Path, platform: str, stage: str, defect: str
) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(tmp_path, stage)
    def mutate(evidence: dict[str, Any]) -> None:
        _corrupt_scenario_operation_binding(evidence, defect)
    _rewrite_packet_evidence(packet, mutate)
    with pytest.raises(ValueError):
        validate_packet(packet)


@pytest.mark.parametrize("value", ["not-a-timestamp", "2026-08-23T01:00:00+01:00"])
def test_linux_packet_rejects_noncanonical_barrier_timestamps(tmp_path: Path, value: str) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path)
    packet["docker"]["barrier"]["ready_inspect_utc"] = value
    with pytest.raises(ValueError, match="barrier"):
        validate_packet(packet)


def test_linux_packet_rejects_out_of_order_parsed_barrier_timestamps(tmp_path: Path) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = _linux_packet(tmp_path)
    packet["docker"]["barrier"]["probes_completed_utc"] = "2026-08-22T23:59:59+00:00"
    with pytest.raises(ValueError, match="barrier"):
        validate_packet(packet)


def test_diagnostics_034_through_044_have_append_only_zero_credit_inventory() -> None:
    tracker = ORCHESTRATION_TRACKER.read_text(encoding="utf-8")
    for diagnostic in ("034", "035", "036", "037", "038", "039", "040", "041", "042", "043", "044"):
        assert diagnostic in tracker
    assert "unsigned implementer packets" in tracker
    assert "fail `diff_check`" in tracker
    assert "source-only" in tracker
    assert "Diagnostic-033 remains preserved" in tracker


def test_complete_windows_and_linux_packet_shapes_pass(tmp_path: Path) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    assert validate_packet(_windows_packet(tmp_path / "w"))["platform"] == "windows"
    assert validate_packet(_linux_packet(tmp_path / "l"))["platform"] == "linux-docker"


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
def test_packet_validation_uses_one_immutable_read_per_retained_artifact(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, platform: str
) -> None:
    """Identity and semantic gates must consume the same retained byte snapshot."""

    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / platform
    )
    watched = {
        Path(record["path"]).resolve()
        for record in packet["artifacts"].values()
        if isinstance(record, dict) and isinstance(record.get("path"), str)
    }
    watched.update(
        Path(copy["artifact"]["path"]).resolve()
        for copy in packet["artifacts"]["fcstd"].values()
    )
    reads: dict[Path, int] = {}
    original_read_bytes = Path.read_bytes

    def read_once(path: Path) -> bytes:
        resolved = path.resolve()
        if resolved in watched:
            reads[resolved] = reads.get(resolved, 0) + 1
            if reads[resolved] > 1:
                raise AssertionError(f"retained artifact reread after snapshot: {resolved}")
        return original_read_bytes(path)

    monkeypatch.setattr(Path, "read_bytes", read_once)
    assert validate_packet(packet)["platform"] == platform
    assert reads == {path: 1 for path in watched}
    evidence_path = Path(packet["artifacts"]["evidence"]["path"]).resolve()
    junit_path = Path(packet["artifacts"]["junit"]["path"]).resolve()
    assert reads[evidence_path] == reads[junit_path] == 1


@pytest.mark.parametrize("platform", ["windows", "linux-docker"])
def test_packet_validation_rejects_corrupt_first_artifact_snapshot(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch, platform: str
) -> None:
    """A malformed first read fails hash/semantic validation; no swap can rescue it."""

    from tests.gui.part3.stage_gate_runner import validate_packet

    packet = (_windows_packet if platform == "windows" else _linux_packet)(
        tmp_path / f"{platform}-corrupt"
    )
    evidence_path = Path(packet["artifacts"]["evidence"]["path"]).resolve()
    original_read_bytes = Path.read_bytes

    def corrupt_evidence(path: Path) -> bytes:
        if path.resolve() == evidence_path:
            return b'{"corrupt":true}\n'
        return original_read_bytes(path)

    monkeypatch.setattr(Path, "read_bytes", corrupt_evidence)
    with pytest.raises(ValueError, match="evidence (size|hash) does not match"):
        validate_packet(packet)


def test_windows_runner_collects_synthetic_handoff_end_to_end(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    output = tmp_path / "out"

    def fake_run(command, *, cwd, env, log_path):
        del command, cwd
        evidence = tmp_path / "owned" / "evidence" / "evidence.json"
        _write(evidence, json.dumps(_stage_evidence("A")) + "\n")
        launcher = evidence.parents[1] / "launcher.log"
        _write(launcher, "graceful\n")
        _write(Path(env[runner.HANDOFF_ENV]), json.dumps({
            "schema_version": 1,
            "stage": "A",
            "coordinator_return_code": 0,
            "evidence_path": str(evidence),
            "launcher_path": str(launcher),
        }) + "\n")
        _write(Path(log_path), "1 passed\n")
        junit_arg = next(item for item in runner._windows_stage_command(Path(sys.executable), output, "a") if item.startswith("--junitxml="))
        _write(Path(junit_arg.split("=", 1)[1]), _junit("A"))
        return 0

    monkeypatch.setattr(runner, "_run_and_log", fake_run)
    assert runner.run_windows(repo_root=REPO_ROOT, output_dir=output, stage="a", python_executable=Path(sys.executable)) == 0
    packet = json.loads((output / "stage-a-execution-packet.json").read_text(encoding="utf-8"))
    assert runner.validate_packet(packet)["platform"] == "windows"
    assert set(packet["artifacts"]) >= {"evidence", "launcher_log", "junit", "runner_log"}


def test_windows_runner_retains_nonzero_stage_artifacts_before_returning_code(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    from tests.gui.part3 import stage_gate_runner as runner

    output = tmp_path / "out"
    validation_calls: list[dict[str, Any]] = []

    def fake_run(command, *, cwd, env, log_path):
        del command, cwd
        evidence = tmp_path / "owned" / "evidence" / "evidence.json"
        launcher = evidence.parents[1] / "launcher.log"
        _write(evidence, '{"verdict":"FAILED"}\n')
        _write(launcher, "retained failure launcher\n")
        _write(
            Path(env[runner.HANDOFF_ENV]),
            json.dumps(
                {
                    "schema_version": 1,
                    "stage": "A",
                    "coordinator_return_code": 7,
                    "evidence_path": str(evidence),
                    "launcher_path": str(launcher),
                }
            )
            + "\n",
        )
        _write(Path(log_path), "stage failed\n")
        junit_arg = next(
            item
            for item in runner._windows_stage_command(
                Path(sys.executable), output, "a"
            )
            if item.startswith("--junitxml=")
        )
        _write(
            Path(junit_arg.split("=", 1)[1]),
            '<testsuite tests="1" failures="1" errors="0" skipped="0" />\n',
        )
        return 7

    monkeypatch.setattr(runner, "_run_and_log", fake_run)
    monkeypatch.setattr(
        runner, "validate_packet", lambda packet: validation_calls.append(packet)
    )

    result = runner.run_windows(
        repo_root=REPO_ROOT,
        output_dir=output,
        stage="a",
        python_executable=Path(sys.executable),
    )

    packet = json.loads(
        (output / "stage-a-execution-packet.json").read_text(encoding="utf-8")
    )
    assert result == 7
    assert packet["execution"]["return_code"] == 7
    assert "artifact_collection_error" not in packet
    assert set(packet["artifacts"]) >= {
        "evidence",
        "launcher_log",
        "junit",
        "runner_log",
    }
    assert (output / "stage-a-evidence.json").read_text(encoding="utf-8") == (
        '{"verdict":"FAILED"}\n'
    )
    assert (output / "stage-a-launcher.log").read_text(encoding="utf-8") == (
        "retained failure launcher\n"
    )
    assert validation_calls == []


def test_windows_runner_retains_failure_when_handoff_is_absent(
    tmp_path: Path, monkeypatch: pytest.MonkeyPatch
) -> None:
    """REVIEW-P3-WP25-004: pytest skip/no evidence is durable failure, never VALID."""

    from tests.gui.part3 import stage_gate_runner as runner

    output = tmp_path / "out"

    def fake_run(command, *, cwd, env, log_path):
        del command, cwd, env
        _write(Path(log_path), "1 skipped\n")
        _write(
            output / "stage-a-junit.xml",
            '<testsuite tests="1" failures="0" errors="0" skipped="1" />\n',
        )
        return 0

    monkeypatch.setattr(runner, "_run_and_log", fake_run)
    with pytest.raises(ValueError, match="handoff"):
        runner.run_windows(
            repo_root=REPO_ROOT,
            output_dir=output,
            stage="a",
            python_executable=Path(sys.executable),
        )
    packet = json.loads(
        (output / "stage-a-execution-packet.json").read_text(encoding="utf-8")
    )
    assert packet["execution"]["return_code"] == 0
    assert "artifact_collection_error" in packet
    with pytest.raises(ValueError):
        runner.validate_packet(packet)


def test_packet_validator_rejects_non_stage_commands(tmp_path: Path) -> None:
    from tests.gui.part3.stage_gate_runner import validate_packet

    for packet in (_windows_packet(tmp_path / "w"), _linux_packet(tmp_path / "l")):
        packet["exact_command"] = ["echo", "not-a-stage"]
        with pytest.raises(ValueError):
            validate_packet(packet)


def test_runner_cli_is_directly_main_agent_runnable() -> None:
    completed = subprocess.run(
        [sys.executable, str(RUNNER), "--help"],
        cwd=REPO_ROOT,
        capture_output=True,
        text=True,
        check=False,
    )
    assert completed.returncode == 0, completed.stderr
    assert "windows" in completed.stdout
    assert "linux-docker" in completed.stdout
    assert "validate" in completed.stdout
