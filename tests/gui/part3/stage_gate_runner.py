#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Tracked Stage A/B/C launcher, execution provenance recorder and validator."""

from __future__ import annotations

import argparse
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import importlib.util
import json
import math
import os
import posixpath
import re
import shutil
import subprocess
import sys
import time
import uuid
import xml.etree.ElementTree as ET
from pathlib import Path, PurePosixPath
from typing import Any, Mapping, Sequence

REPO_ROOT = Path(__file__).resolve().parents[3]
if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

from tests.gui.part3.evidence import (  # noqa: E402
    session_ttl_provenance,
    utc_now_iso,
    validate_completed_stage_evidence,
)

LIVE_STAGE_ENV = "PART3_STAGE_LIVE"
TTL_ENV = "FREECAD_MCP_SESSION_TTL_SECONDS"
FAULT_HANDLER_ENV = "PYTHONFAULTHANDLER"
IMAGE_ID_PREFIX = "sha256:"
LINUX_REPO_MOUNT = "/repo"
LINUX_BUILD_MOUNT = "/workspace/build"
EVIDENCE_LINE = re.compile(r"^evidence:\s*(?P<path>.+)$", re.MULTILINE)
HANDOFF_ENV = "PART3_STAGE_EVIDENCE_HANDOFF"
PROBE_REFUSAL_EXIT = 73
STAGE_EXECUTION_TIMEOUT_SECONDS = {"a": 3900, "b": 11100, "c": 57900}
LINUX_RELEASE_BARRIER_TIMEOUT_SECONDS = 120
LINUX_DOCKER_CONTROL_TIMEOUT_SECONDS = 30
WINDOWS_TIMEOUT_CLEANUP_SECONDS = 30

# The next Luna native snapshot is byte-bound to these tracked candidates,
# plus the two runtime binaries.  The nested gitlink is an identity, not one
# of the sixteen file entries.
LUNA_SNAPSHOT_CANDIDATE_PATHS = (
    ".gitignore",
    "doc/document-collaboration-completion-progress.md",
    "doc/part3-gui-collaboration-stress-design.md",
    "doc/part3-orchestrated-review-fix-test-plan.md",
    "tests/gui/part3/conftest.py",
    "tests/gui/part3/evidence.py",
    "tests/gui/part3/local_driver/actions.py",
    "tests/gui/part3/scenarios.py",
    "tests/gui/part3/stage_gate_runner.py",
    "tests/gui/part3/stress_coordinator.py",
    "tests/gui/part3/test_part3_architecture.py",
    "tests/gui/part3/test_part3_stage_acceptance.py",
    "tests/gui/part3/test_part3_stage_gate_runner.py",
    "tests/gui/part3/test_part3_stress_coordinator_launcher.py",
)

STAGE_NODEIDS = {
    "a": (
        "tests/gui/part3/test_part3_stage_acceptance.py::"
        "test_stage_a_runs_ten_view_cycles_and_five_save_cycles"
    ),
    "b": (
        "tests/gui/part3/test_part3_stage_acceptance.py::"
        "test_stage_b_runs_fifty_view_cycles_and_twenty_save_cycles"
    ),
    "c": (
        "tests/gui/part3/test_part3_stage_acceptance.py::"
        "test_stage_c_runs_five_hundred_view_cycles_and_one_hundred_save_cycles"
    ),
}


def _require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def _nonempty_command(packet: Mapping[str, Any]) -> list[str]:
    command = packet.get("exact_command")
    _require(
        isinstance(command, list)
        and bool(command)
        and all(isinstance(item, str) and bool(item) for item in command),
        "exact pre-execution command is absent",
    )
    return list(command)


def _validate_ttl(packet: Mapping[str, Any]) -> None:
    ttl = packet.get("ttl")
    _require(isinstance(ttl, dict), "effective TTL proof is absent")
    _require(
        ttl.get("environment_variable") == TTL_ENV,
        "TTL environment-variable identity is absent",
    )
    _require(
        ttl.get("override_present") is False,
        "the required unset TTL override state is not proved",
    )
    _require(ttl.get("override_value") is None, "TTL override value must be absent")
    effective = ttl.get("effective_seconds")
    default = ttl.get("default_seconds")
    _require(
        type(effective) in (int, float)
        and not isinstance(effective, bool)
        and math.isfinite(float(effective))
        and float(effective) > 0,
        "effective TTL value is absent",
    )
    _require(
        type(default) in (int, float)
        and not isinstance(default, bool)
        and math.isfinite(float(default))
        and float(default) > 0,
        "default TTL value is absent",
    )
    _require(ttl.get("source") == "default", "effective TTL is not the default")
    _require(float(effective) == float(default), "effective TTL differs from default")

    provenance = ttl.get("constants_file")
    _require(isinstance(provenance, dict), "tracked TTL constants provenance is absent")
    repo_root_value = packet.get("repo_root")
    _require(isinstance(repo_root_value, str) and repo_root_value, "repo root is absent")
    relative = provenance.get("repo_relative_path")
    _require(
        relative
        == "tools/mcp/freecad-mcp/addon/FreeCADMCP/_shared/protocol/constants.py",
        "TTL constants path is not the tracked protocol constants file",
    )
    constants_path = Path(repo_root_value).resolve() / str(relative)
    _require(constants_path.is_file(), "tracked TTL constants file is unavailable")
    constants_bytes = constants_path.read_bytes()
    _require(
        provenance.get("sha256") == hashlib.sha256(constants_bytes).hexdigest()
        and provenance.get("size") == len(constants_bytes),
        "tracked TTL constants fingerprint does not match",
    )
    _require(
        provenance.get("default_symbol") == "DEFAULT_SESSION_TTL_SECONDS",
        "default TTL symbol provenance is absent",
    )
    spec = importlib.util.spec_from_file_location(
        "_part3_stage_gate_protocol_constants", constants_path
    )
    _require(spec is not None and spec.loader is not None, "cannot load TTL constants")
    constants = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(constants)
    tracked_default = float(constants.DEFAULT_SESSION_TTL_SECONDS)
    _require(
        math.isfinite(tracked_default)
        and tracked_default == float(default)
        and provenance.get("default_value") == tracked_default,
        "packet TTL does not equal the tracked protocol default",
    )


def _artifact_record(path: Path) -> dict[str, Any]:
    data = path.read_bytes()
    return {
        "path": str(path.resolve()),
        "sha256": hashlib.sha256(data).hexdigest(),
        "size": len(data),
    }


@dataclass(frozen=True)
class _ArtifactSnapshot:
    """One immutable retained-artifact read used for identity and semantics."""

    path: Path
    data: bytes

    def text(self) -> str:
        return self.data.decode("utf-8")


def _validate_artifact(record: object, name: str) -> _ArtifactSnapshot:
    _require(isinstance(record, dict), f"{name} artifact identity is absent")
    path_value = record.get("path")
    _require(isinstance(path_value, str) and path_value, f"{name} path is absent")
    path = Path(path_value)
    _require(path.is_file(), f"{name} artifact is unavailable")
    data = path.read_bytes()
    _require(
        type(record.get("size")) is int
        and record.get("size") == len(data)
        and len(data) > 0,
        f"{name} size does not match",
    )
    _require(
        record.get("sha256") == hashlib.sha256(data).hexdigest(),
        f"{name} hash does not match",
    )
    return _ArtifactSnapshot(path=path, data=data)


def _validate_junit(data: bytes, stage: str) -> None:
    try:
        root = ET.fromstring(data)
    except (ET.ParseError, UnicodeDecodeError) as exc:
        raise ValueError(f"JUnit artifact is malformed: {exc}") from exc
    suites = list(root.iter("testsuite"))
    _require(bool(suites), "JUnit contains no test suite")
    cases = list(root.iter("testcase"))
    _require(len(cases) == 1, "JUnit must contain exactly one stage testcase")
    owning_suite: dict[int, ET.Element] = {}

    def record_ownership(node: ET.Element, owner: ET.Element | None = None) -> None:
        for child in node:
            if child.tag == "testsuite":
                record_ownership(child, child)
            elif child.tag == "testcase" and owner is not None:
                owning_suite[id(child)] = owner

    record_ownership(root, root if root.tag == "testsuite" else None)
    _require(
        len(owning_suite) == len(cases),
        "JUnit testcase is not owned by a concrete test suite",
    )
    outcomes = {
        "tests": len(cases),
        "failures": sum(bool(case.findall("failure")) for case in cases),
        "errors": sum(bool(case.findall("error")) for case in cases),
        "skipped": sum(
            bool(case.findall("skipped")) or bool(case.findall("skip"))
            for case in cases
        ),
    }


    def declared_counts(
        node: ET.Element, label: str, *, required: bool
    ) -> dict[str, int]:
        values: dict[str, int] = {}
        for key in outcomes:
            value = node.attrib.get(key)
            if value is None:
                _require(
                    not required,
                    f"JUnit {label} is missing required {key} count",
                )
                continue
            try:
                values[key] = int(value)
            except ValueError as exc:
                raise ValueError(f"JUnit {label} has non-integer {key} count") from exc
            _require(values[key] >= 0, f"JUnit {label} has negative {key} count")
        return values

    for index, suite in enumerate(suites):
        declared = declared_counts(suite, f"suite {index}", required=True)
        suite_cases = list(suite.iter("testcase"))
        suite_outcomes = {
            "tests": len(suite_cases),
            "failures": sum(bool(case.findall("failure")) for case in suite_cases),
            "errors": sum(bool(case.findall("error")) for case in suite_cases),
            "skipped": sum(
                bool(case.findall("skipped")) or bool(case.findall("skip"))
                for case in suite_cases
            ),
        }
        _require(all(declared[key] == suite_outcomes[key] for key in declared),
                 f"JUnit suite {index} declared counts disagree with testcase outcomes")
    if root.tag == "testsuites":
        declared = declared_counts(root, "root", required=False)
        _require(
            all(declared[key] == outcomes[key] for key in declared),
            "JUnit root declared counts disagree with testcase outcomes",
        )
    _require(
        outcomes["failures"] == outcomes["errors"] == outcomes["skipped"] == 0,
        f"JUnit is not an unskipped pass: {outcomes}",
    )
    expected_nodeid = STAGE_NODEIDS[stage.lower()]
    module_name, test_name = expected_nodeid.split("::", 1)
    expected_classname = module_name[:-3].replace("/", ".")
    _require(
        cases[0].attrib.get("classname") == expected_classname
        and cases[0].attrib.get("name") == test_name,
        "JUnit does not prove the exact requested stage testcase",
    )
    _require(
        id(cases[0]) in owning_suite,
        "JUnit exact stage testcase is not owned by a concrete test suite",
    )


SNAPSHOT_REFERENCE_KEYS = (
    "parent_head", "parent_branch", "parent_upstream", "parent_log", "nested_head", "nested_branch",
)
SNAPSHOT_GIT_RESULT_KEYS = (
    "parent_head", "parent_branch", "parent_upstream", "parent_status", "parent_gitlink",
    "nested_head", "nested_branch", "nested_upstream", "nested_status", "nested_diff",
)
SNAPSHOT_GIT_COMMANDS = {
    "parent_head": ["git", "rev-parse", "HEAD"],
    "parent_branch": ["git", "branch", "--show-current"],
    "parent_upstream": ["git", "rev-parse", "@{u}"],
    "parent_status": ["git", "status", "--short"],
    "parent_gitlink": ["git", "ls-tree", "HEAD", "tools/mcp/freecad-mcp"],
    "nested_head": ["git", "-C", "tools/mcp/freecad-mcp", "rev-parse", "HEAD"],
    "nested_branch": ["git", "-C", "tools/mcp/freecad-mcp", "branch", "--show-current"],
    "nested_upstream": ["git", "-C", "tools/mcp/freecad-mcp", "rev-parse", "@{u}"],
    "nested_status": ["git", "-C", "tools/mcp/freecad-mcp", "status", "--short"],
    "nested_diff": ["git", "-C", "tools/mcp/freecad-mcp", "diff", "--binary", "HEAD"],
}
FINAL_AFTERMATH_KEYS = (
    "owned_snapshot_volume_absent", "owned_volume_users", "active_containers",
    "retained_stopped_containers", "general_volumes_preserved", "target_processes",
    "target_listeners",
)
SNAPSHOT_RECEIPT_KEYS = frozenset({
    "command", "return_code", "references", "statuses", "nested_diff_bytes", "hashes", "imports_pass",
})
AFTERMATH_RAW_RECEIPT_KEYS = frozenset({
    "argv", "target", "wrapper_return_code", "underlying_return_code", "stdout", "stderr",
})
AFTERMATH_RECEIPT_IDENTITY_KEYS = frozenset({"path", "sha256", "size"})
FROZEN_RESOURCE_CONTRACT_KEYS = frozenset({"commands", "general_volumes", "stopped_containers"})
FROZEN_GIT_RESULT_KEYS = frozenset({"argv", "return_code", "stdout", "stderr"})


def _read_hashed_json_contract(identity: Mapping[str, Any], label: str) -> Any:
    """Read JSON only when the caller's path, size, and digest bind its bytes."""

    _require(
        isinstance(identity, Mapping) and set(identity) == AFTERMATH_RECEIPT_IDENTITY_KEYS
        and isinstance(identity.get("path"), str) and identity["path"]
        and isinstance(identity.get("sha256"), str) and len(identity["sha256"]) == 64
        and type(identity.get("size")) is int and identity["size"] >= 0,
        f"{label} identity is malformed",
    )
    path = Path(identity["path"])
    _require(path.is_file(), f"{label} is unavailable")
    data = path.read_bytes()
    _require(
        len(data) == identity["size"] and hashlib.sha256(data).hexdigest() == identity["sha256"],
        f"{label} identity does not bind bytes",
    )
    try:
        return json.loads(data.decode("utf-8"))
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"{label} is not valid JSON") from exc


def _read_frozen_snapshot_git_contract(identity: Mapping[str, Any]) -> dict[str, dict[str, Any]]:
    """Load independently frozen full Git results before snapshot population."""

    contract = _read_hashed_json_contract(identity, "snapshot Git-result contract")
    _require(
        isinstance(contract, dict) and set(contract) == set(SNAPSHOT_GIT_RESULT_KEYS),
        "snapshot Git-result contract has an incomplete command set",
    )
    for key in SNAPSHOT_GIT_RESULT_KEYS:
        result = contract[key]
        _require(
            isinstance(result, dict) and set(result) == FROZEN_GIT_RESULT_KEYS
            and result.get("argv") == SNAPSHOT_GIT_COMMANDS[key]
            and type(result.get("return_code")) is int and result["return_code"] == 0
            and isinstance(result.get("stdout"), str)
            and result.get("stderr") == "",
            f"snapshot Git-result contract {key} is malformed or non-green",
        )
    return {key: dict(contract[key]) for key in SNAPSHOT_GIT_RESULT_KEYS}


def _read_frozen_resource_contract(identity: Mapping[str, Any]) -> dict[str, Any]:
    """Load the independently frozen resource inventory and exact command argv."""

    contract = _read_hashed_json_contract(identity, "aftermath resource contract")
    _require(
        isinstance(contract, dict) and set(contract) == FROZEN_RESOURCE_CONTRACT_KEYS
        and isinstance(contract.get("commands"), dict)
        and set(contract["commands"]) == set(FINAL_AFTERMATH_KEYS),
        "aftermath resource contract is incomplete or malformed",
    )
    commands = contract["commands"]
    _require(
        all(
            isinstance(command, list) and bool(command)
            and all(isinstance(part, str) and part for part in command)
            for command in commands.values()
        ),
        "aftermath resource contract command is malformed",
    )
    _require(
        commands["owned_snapshot_volume_absent"][:3] == ["docker", "volume", "inspect"]
        and len(commands["owned_snapshot_volume_absent"]) == 4
        and all(isinstance(part, str) and part for part in commands["owned_snapshot_volume_absent"])
        and commands["owned_volume_users"] == [
            "docker", "ps", "-a", "--filter",
            f"volume={commands['owned_snapshot_volume_absent'][-1]}", "--format", "{{.ID}}",
        ]
        and commands["active_containers"] == ["docker", "ps", "-q"]
        and commands["retained_stopped_containers"] == [
            "docker", "ps", "-a", "--filter", "status=exited", "--format", "{{.ID}}|{{.State}}",
        ]
        and commands["general_volumes_preserved"] == [
            "docker", "volume", "ls", "--format", "{{.Name}}",
        ]
        and all(
            commands[key][:3] == ["pwsh", "-NoProfile", "-File"]
            and len(commands[key]) == 4 and isinstance(commands[key][-1], str)
            and commands[key][-1].replace("\\", "/").endswith(f"/{key.replace('_', '-')}.ps1")
            for key in ("target_processes", "target_listeners")
        ),
        "aftermath resource contract command is not a canonical direct probe",
    )
    volumes = contract.get("general_volumes")
    _require(
        isinstance(volumes, list) and bool(volumes)
        and all(isinstance(name, str) and name for name in volumes)
        and len(set(volumes)) == len(volumes),
        "aftermath resource contract volume inventory is malformed",
    )
    stopped = contract.get("stopped_containers")
    _require(
        isinstance(stopped, list)
        and all(
            isinstance(item, dict) and set(item) == {"id", "state"}
            and isinstance(item.get("id"), str) and item["id"]
            and item.get("state") == "exited"
            for item in stopped
        )
        and len({item["id"] for item in stopped}) == len(stopped),
        "aftermath resource contract stopped-container inventory is malformed",
    )
    return {
        "commands": {key: list(commands[key]) for key in FINAL_AFTERMATH_KEYS},
        "general_volumes": list(volumes),
        "stopped_containers": [dict(item) for item in stopped],
    }


def validate_snapshot_qualification_receipt(
    receipt: Mapping[str, Any], *, expected_references: Mapping[str, str],
    expected_statuses: Mapping[str, Sequence[str]], expected_nested_diff_bytes: int,
    expected_hashes: Mapping[str, str], expected_command: Sequence[str],
) -> None:
    """Fail closed over a fresh snapshot's explicitly frozen expectations."""

    _require(
        set(expected_references) == set(SNAPSHOT_REFERENCE_KEYS)
        and all(isinstance(expected_references[key], str) and expected_references[key] for key in SNAPSHOT_REFERENCE_KEYS),
        "snapshot expectations must explicitly freeze every reference",
    )
    _require(set(expected_statuses) == {"parent", "nested"} and all(isinstance(lines, Sequence) and not isinstance(lines, (str, bytes)) and all(isinstance(line, str) for line in lines) for lines in expected_statuses.values()), "snapshot expectations must explicitly freeze parent and nested status")
    _require(expected_nested_diff_bytes == 0, "snapshot nested diff expectation must be exactly zero")
    _require(isinstance(expected_command, Sequence) and not isinstance(expected_command, (str, bytes)) and bool(expected_command) and all(isinstance(part, str) and part for part in expected_command), "snapshot expectation must explicitly freeze its command")
    expected_hash_paths = {*LUNA_SNAPSHOT_CANDIDATE_PATHS, "build/release/bin/FreeCAD.exe", "build/release/bin/FreeCADCmd.exe"}
    _require(set(expected_hashes) == expected_hash_paths and all(isinstance(path, str) and path and isinstance(digest, str) and len(digest) == 64 and all(char in "0123456789abcdef" for char in digest) for path, digest in expected_hashes.items()), "snapshot expectations must explicitly freeze sixteen file hashes")
    _require(isinstance(receipt, Mapping) and set(receipt) == SNAPSHOT_RECEIPT_KEYS and receipt.get("command") == list(expected_command) and receipt.get("return_code") == 0 and receipt.get("references") == dict(expected_references) and receipt.get("statuses") == {key: list(value) for key, value in expected_statuses.items()} and receipt.get("nested_diff_bytes") == expected_nested_diff_bytes and receipt.get("hashes") == dict(expected_hashes) and receipt.get("imports_pass") is True, "snapshot qualification receipt does not match its frozen expectations")


def validate_captured_snapshot_qualification(
    receipt: Mapping[str, Any], *, snapshot_root: Path,
    expected_hashes: Mapping[str, str], frozen_git_result_contract: Mapping[str, Any],
    expected_import_command: Sequence[str], expected_import_stdout: str,
) -> None:
    """Bind copied bytes and raw Git results to contracts frozen before capture."""

    expected_paths = {
        *LUNA_SNAPSHOT_CANDIDATE_PATHS,
        "build/release/bin/FreeCAD.exe", "build/release/bin/FreeCADCmd.exe",
    }
    _require(set(expected_hashes) == expected_paths and set(receipt) == {"hashes", "git_commands", "imports"}, "captured snapshot receipt has an incomplete path/key set")
    _require(receipt.get("hashes") == dict(expected_hashes), "captured snapshot hashes do not match frozen expectations")
    root = Path(snapshot_root)
    for relative, digest in expected_hashes.items():
        path = root.joinpath(*relative.split("/"))
        _require(path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == digest, f"captured snapshot bytes do not bind {relative}")
    expected_git = _read_frozen_snapshot_git_contract(frozen_git_result_contract)
    git = receipt.get("git_commands")
    _require(
        isinstance(git, Mapping) and set(git) == set(SNAPSHOT_GIT_RESULT_KEYS)
        and git == expected_git,
        "captured snapshot Git results do not match the frozen command/result contract",
    )
    imports = receipt.get("imports")
    _require(isinstance(imports, Mapping) and set(imports) == {"argv", "return_code", "stdout", "stderr"} and imports.get("argv") == list(expected_import_command) and imports.get("return_code") == 0 and imports.get("stdout") == expected_import_stdout and imports.get("stderr") == "", "captured snapshot import command did not complete exactly")


def _validated_aftermath_receipts(
    receipts: Mapping[str, Any], *, expected: Mapping[str, bool | int],
    frozen_resource_contract: Mapping[str, Any],
) -> tuple[dict[str, bool | int], dict[str, dict[str, Any]]]:
    """Parse immutable raw command receipts into the seal's aftermath claims."""

    _require(
        set(expected) == set(FINAL_AFTERMATH_KEYS)
        and type(expected["owned_snapshot_volume_absent"]) is bool
        and type(expected["general_volumes_preserved"]) is bool
        and all(type(expected[key]) is int and expected[key] >= 0 for key in FINAL_AFTERMATH_KEYS if key not in {"owned_snapshot_volume_absent", "general_volumes_preserved"}),
        "aftermath expectations are incomplete or malformed",
    )
    _require(isinstance(receipts, Mapping) and set(receipts) == set(FINAL_AFTERMATH_KEYS), "aftermath receipts are incomplete")
    resource_contract = _read_frozen_resource_contract(frozen_resource_contract)
    commands = resource_contract["commands"]
    derived: dict[str, bool | int] = {}
    bindings: dict[str, dict[str, Any]] = {}
    for key in FINAL_AFTERMATH_KEYS:
        identity = receipts[key]
        _require(
            isinstance(identity, Mapping) and set(identity) == AFTERMATH_RECEIPT_IDENTITY_KEYS
            and isinstance(identity.get("path"), str) and identity["path"]
            and isinstance(identity.get("sha256"), str) and len(identity["sha256"]) == 64
            and type(identity.get("size")) is int and identity["size"] >= 0,
            f"{key} receipt identity is malformed",
        )
        path = Path(identity["path"])
        _require(path.is_file(), f"{key} receipt is unavailable")
        data = path.read_bytes()
        _require(len(data) == identity["size"] and hashlib.sha256(data).hexdigest() == identity["sha256"], f"{key} receipt identity does not bind bytes")
        try:
            raw = json.loads(data.decode("utf-8"))
        except (UnicodeDecodeError, json.JSONDecodeError) as exc:
            raise ValueError(f"{key} receipt is not valid raw JSON") from exc
        _require(isinstance(raw, dict) and set(raw) == AFTERMATH_RAW_RECEIPT_KEYS, f"{key} raw receipt schema is malformed")
        _require(raw["argv"] == commands[key], f"{key} receipt command does not match frozen argv")
        _require(
            isinstance(raw["target"], str) and raw["target"]
            and isinstance(raw["stdout"], str) and isinstance(raw["stderr"], str)
            and type(raw["wrapper_return_code"]) is int
            and type(raw["underlying_return_code"]) is int,
            f"{key} receipt raw streams are malformed",
        )
        if key == "owned_snapshot_volume_absent":
            subject = commands[key][-1]
            _require(raw["target"] == subject and raw["wrapper_return_code"] == 0 and raw["underlying_return_code"] == 1 and raw["stdout"] == "[]\n" and raw["stderr"] == f"Error response from daemon: get {subject}: no such volume\n", "owned volume receipt does not prove exact natural inspect absence")
            value: bool | int = True
        elif key in {"owned_volume_users", "active_containers"}:
            identifiers = raw["stdout"].splitlines()
            _require(
                raw["target"] == (
                    commands["owned_snapshot_volume_absent"][-1]
                    if key == "owned_volume_users" else key
                )
                and raw["wrapper_return_code"] == raw["underlying_return_code"] == 0
                and raw["stderr"] == ""
                and raw["stdout"] == "".join(f"{identifier}\n" for identifier in identifiers)
                and all(re.fullmatch(r"[0-9a-f]{12,64}", identifier) for identifier in identifiers),
                f"{key} receipt direct Docker output is malformed or contradictory",
            )
            value = len(identifiers)
        elif key == "general_volumes_preserved":
            volumes = raw["stdout"].splitlines()
            _require(
                raw["target"] == key
                and raw["wrapper_return_code"] == raw["underlying_return_code"] == 0
                and raw["stderr"] == ""
                and raw["stdout"] == "".join(f"{name}\n" for name in volumes)
                and volumes == resource_contract["general_volumes"],
                "general-volume receipt does not prove the frozen exact inventory",
            )
            value = True
        elif key == "retained_stopped_containers":
            stopped: list[dict[str, str]] = []
            for line in raw["stdout"].splitlines():
                identifier, separator, state = line.partition("|")
                _require(
                    separator == "|" and re.fullmatch(r"[0-9a-f]{12,64}", identifier)
                    and state == "exited",
                    "retained stopped-container receipt is not direct Docker ID/state output",
                )
                stopped.append({"id": identifier, "state": state})
            _require(
                raw["target"] == key
                and raw["wrapper_return_code"] == raw["underlying_return_code"] == 0
                and raw["stderr"] == "" and isinstance(stopped, list)
                and raw["stdout"] == "".join(f"{item['id']}|{item['state']}\n" for item in stopped)
                and stopped == resource_contract["stopped_containers"],
                "retained stopped-container receipt does not prove the frozen exact inventory",
            )
            value = len(stopped)
        else:
            try:
                entries = json.loads(raw["stdout"])
            except json.JSONDecodeError as exc:
                raise ValueError(f"{key} receipt is not valid JSON") from exc
            _require(
                raw["target"] == commands[key][-1]
                and raw["wrapper_return_code"] == raw["underlying_return_code"] == 0
                and raw["stderr"] == "" and isinstance(entries, list),
                f"{key} receipt output is malformed or contradictory",
            )
            if key in {"target_processes", "target_listeners"}:
                _require(entries == [], f"{key} receipt does not prove an empty result")
            value = len(entries)
        _require(value == expected[key], f"{key} receipt does not prove the frozen resource state")
        derived[key] = value
        bindings[key] = dict(identity)
    return derived, bindings


def derive_final_aftermath_from_receipts(
    receipts: Mapping[str, Any], *, expected: Mapping[str, bool | int],
    frozen_resource_contract: Mapping[str, Any],
) -> dict[str, bool | int]:
    """Derive aftermath values from immutable raw receipts, never an observation field."""

    return _validated_aftermath_receipts(
        receipts, expected=expected, frozen_resource_contract=frozen_resource_contract
    )[0]


def validate_final_resource_proof(
    receipts: Mapping[str, Any], *, expected: Mapping[str, bool | int],
    frozen_resource_contract: Mapping[str, Any],
) -> dict[str, bool | int]:
    """Fail closed over raw final-resource results and their frozen inventory."""

    derived, _ = _validated_aftermath_receipts(
        receipts, expected=expected, frozen_resource_contract=frozen_resource_contract
    )
    _require(
        derived["owned_snapshot_volume_absent"] is True
        and derived["general_volumes_preserved"] is True
        and derived["owned_volume_users"] == derived["active_containers"]
        == derived["target_processes"] == derived["target_listeners"] == 0,
        "final resource proof does not prove an empty active process/container state",
    )
    return derived


def build_derived_final_seal(
    manifest: Mapping[str, Any], *, aftermath_receipts: Mapping[str, Any],
    expected_aftermath: Mapping[str, bool | int], aftermath_resource_contract: Mapping[str, Any],
    snapshot_git_result_contract: Mapping[str, Any],
) -> dict[str, Any]:
    """Build a seal bound to frozen snapshot and final-resource contracts."""

    _require(
        isinstance(manifest.get("path"), str) and manifest["path"]
        and isinstance(manifest.get("sha256"), str) and len(manifest["sha256"]) == 64
        and type(manifest.get("size")) is int and manifest["size"] >= 0
        and type(manifest.get("entry_count")) is int and manifest["entry_count"] >= 0,
        "seal manifest binding is malformed",
    )
    aftermath = validate_final_resource_proof(
        aftermath_receipts, expected=expected_aftermath,
        frozen_resource_contract=aftermath_resource_contract,
    )
    _, bindings = _validated_aftermath_receipts(
        aftermath_receipts, expected=expected_aftermath,
        frozen_resource_contract=aftermath_resource_contract,
    )
    _read_frozen_snapshot_git_contract(snapshot_git_result_contract)
    return {
        "manifest": dict(manifest), "aftermath": aftermath,
        "aftermath_receipts": bindings,
        "aftermath_resource_contract": dict(aftermath_resource_contract),
        "snapshot_git_result_contract": dict(snapshot_git_result_contract),
    }


def _validate_linux_aftermath(aftermath: str) -> None:
    """Parse the launcher aftermath as one exact, non-ambiguous schema."""

    lines = aftermath.splitlines()
    expected_sections = (
        ("freecad_or_launcher_leftovers_begin", "freecad_or_launcher_leftovers_end"),
        ("port_9875_listeners_begin", "port_9875_listeners_end"),
    )
    _require(lines.count("pytest_rc=0") == 1, "Linux aftermath does not bind exactly one pytest rc=0")
    _require(
        all(not line.startswith("pytest_rc=") or line == "pytest_rc=0" for line in lines),
        "Linux aftermath contains a contradictory pytest result",
    )
    cursor = 1
    _require(lines and lines[0] == "pytest_rc=0", "Linux aftermath has malformed ordering")
    for begin, end in expected_sections:
        _require(lines.count(begin) == 1 and lines.count(end) == 1, f"{begin} occurrence is not exact")
        _require(cursor < len(lines) and lines[cursor] == begin, f"{begin} ordering is malformed")
        cursor += 1
        while cursor < len(lines) and lines[cursor] == "":
            cursor += 1
        _require(cursor < len(lines) and lines[cursor] == end, f"{begin} is not empty")
        cursor += 1
    _require(cursor == len(lines), "Linux aftermath contains unclassified residue")


def _fcstd_evidence_paths(evidence: Mapping[str, Any]) -> list[str]:
    """Return the distinct producer FCStd source identities in scan order."""

    artifacts = evidence.get("artifacts")
    documents = artifacts.get("documents") if isinstance(artifacts, dict) else None
    _require(isinstance(documents, list), "FCStd evidence inventory is absent")
    sources: list[str] = []
    seen: set[str] = set()
    for entry in documents:
        path = entry.get("path") if isinstance(entry, dict) else None
        if not isinstance(path, str) or not path.lower().endswith(".fcstd"):
            continue
        identity = os.path.normcase(os.path.normpath(path)).casefold()
        _require(identity and identity not in seen, "FCStd evidence paths are duplicate")
        seen.add(identity)
        sources.append(path)
    _require(bool(sources), "FCStd evidence inventory is empty")
    return sources


def _retained_fcstd_target(output: Path, index: int, source_path: str) -> Path:
    source_name = Path(source_path).name
    _require(source_name.lower().endswith(".fcstd"), "FCStd source name is malformed")
    token = hashlib.sha256(source_path.encode("utf-8")).hexdigest()[:16]
    return output / "retained-fcstd" / f"{index:03d}-{token}-{source_name}"


def _validate_retained_fcstd_copies(
    artifacts: Mapping[str, Any], evidence: Mapping[str, Any], output: Path
) -> dict[str, dict[str, Any]]:
    """Snapshot output-retained FCStds once and bind them to evidence paths."""

    copies = artifacts.get("fcstd")
    _require(isinstance(copies, dict), "retained FCStd artifacts are absent")
    sources = _fcstd_evidence_paths(evidence)
    expected = {os.path.normcase(os.path.normpath(path)).casefold(): path for path in sources}
    _require(set(copies) == set(expected), "retained FCStd source inventory is incomplete")
    retained_root = (output / "retained-fcstd").resolve()
    snapshots: dict[str, dict[str, Any]] = {}
    retained_paths: set[Path] = set()
    for index, source_path in enumerate(sources):
        identity = os.path.normcase(os.path.normpath(source_path)).casefold()
        copy = copies[identity]
        _require(isinstance(copy, dict), "retained FCStd record is malformed")
        _require(
            copy.get("source_path") == source_path,
            "retained FCStd record does not bind its producer source path",
        )
        snapshot = _validate_artifact(copy.get("artifact"), "retained FCStd")
        _require(
            snapshot.path.resolve().parent == retained_root
            and snapshot.path.resolve()
            == _retained_fcstd_target(output, index, source_path).resolve()
            and snapshot.path.resolve() not in retained_paths,
            "retained FCStd is outside the deterministic output subtree",
        )
        retained_paths.add(snapshot.path.resolve())
        snapshots[identity] = {
            "source_path": source_path,
            "path": str(snapshot.path),
            "data": snapshot.data,
        }
    return snapshots


def _validate_artifacts(
    packet: Mapping[str, Any], platform: str
) -> dict[str, _ArtifactSnapshot]:
    artifacts = packet.get("artifacts")
    _require(isinstance(artifacts, dict), "retained artifacts are absent")
    required = ["runner_log", "evidence", "launcher_log", "junit"]
    if platform == "windows":
        required.append("handoff")
    if platform == "linux-docker":
        required.extend(["handoff", "aftermath", "container_script", "release_barrier"])
    paths = {name: _validate_artifact(artifacts.get(name), name) for name in required}
    retained_binary_copies: dict[str, dict[str, Any]] = {}
    if platform == "linux-docker":
        copies = packet.get("binary_copies") or {}
        _require(isinstance(copies, dict), "retained Linux binary copies are malformed")
        for name, copy in copies.items():
            _require(isinstance(name, str) and isinstance(copy, dict), "retained Linux binary copy is malformed")
            artifact = _validate_artifact(copy.get("artifact"), f"retained Linux binary {name}")
            _require(
                isinstance(copy.get("container_path"), str) and copy["container_path"]
                and artifact.data
                and copy["artifact"].get("sha256") == hashlib.sha256(artifact.data).hexdigest(),
                "retained Linux binary copy does not bind its container path and bytes",
            )
            retained_binary_copies[name] = {
                "container_path": copy["container_path"], "path": str(artifact.path),
            }
    try:
        evidence = json.loads(paths["evidence"].text())
        _require(isinstance(evidence, dict), "retained evidence is not an object")
        retained_fcstd_copies = _validate_retained_fcstd_copies(
            artifacts, evidence, paths["evidence"].path.parent
        )
        validate_completed_stage_evidence(
            evidence,
            stage=str(packet["stage"]),
            repo_root=Path(str(packet["repo_root"])),
            retained_binary_copies=retained_binary_copies,
            retained_fcstd_copies=retained_fcstd_copies,
        )
    except (json.JSONDecodeError, ValueError) as exc:
        raise ValueError(f"retained evidence is not complete passing stage evidence: {exc}") from exc
    _validate_junit(paths["junit"].data, str(packet["stage"]))
    if platform == "windows":
        try:
            handoff = json.loads(paths["handoff"].text())
        except json.JSONDecodeError as exc:
            raise ValueError("retained Windows handoff is malformed") from exc
        environment = packet.get("environment") or {}
        _require(
            handoff.get("schema_version") == 1
            and handoff.get("stage") == packet.get("stage")
            and type(handoff.get("coordinator_return_code")) is int
            and handoff.get("coordinator_return_code") == 0
            and bool(handoff.get("evidence_path"))
            and bool(handoff.get("launcher_path"))
            and environment.get(HANDOFF_ENV) == str(paths["handoff"].path.resolve()),
            "retained Windows handoff is malformed",
        )
    else:
        try:
            handoff = json.loads(paths["handoff"].text())
        except json.JSONDecodeError as exc:
            raise ValueError("retained Linux handoff is malformed") from exc
        environment = packet.get("environment") or {}
        _require(
            handoff.get("schema_version") == 1
            and handoff.get("stage") == packet.get("stage")
            and type(handoff.get("coordinator_return_code")) is int
            and handoff.get("coordinator_return_code") == 0
            and bool(handoff.get("evidence_path"))
            and bool(handoff.get("launcher_path"))
            and environment.get(HANDOFF_ENV) == f"/out/stage-{str(packet['stage']).lower()}-handoff.json",
            "retained Linux handoff is malformed",
        )
        _validate_linux_aftermath(paths["aftermath"].text())
        script = paths["container_script"].text()
        release_line = f"release=/out/stage-{str(packet['stage']).lower()}.release"
        _require(
            release_line in script
            and script.index(release_line) < script.index("export PART3_STAGE_LIVE=1"),
            "container script does not wait behind the retained release barrier",
        )
        _require(
            "unset FREECAD_MCP_SESSION_TTL_SECONDS" in script,
            "container script does not preserve the default-TTL environment",
        )
        _require(
            "export PYTHONFAULTHANDLER=1" in script
            and script.index("export PYTHONFAULTHANDLER=1")
            < script.index("python3 -m pytest"),
            "container script does not enable pre-execution Python fault traces",
        )
        _require(
            paths["release_barrier"].text() == "release\n",
            "release barrier artifact is malformed",
        )
    return paths


def _source_matches(observed: object, expected: Path) -> bool:
    observed_text = str(observed or "").replace("\\", "/").rstrip("/").lower()
    expected_text = str(expected.resolve()).replace("\\", "/").rstrip("/").lower()
    if observed_text == expected_text:
        return True
    if len(expected_text) >= 3 and expected_text[1:3] == ":/":
        drive_suffix = f"/{expected_text[0]}{expected_text[2:]}"
        return observed_text.endswith(drive_suffix)
    return False


def _read_only_mount(
    inspect: Mapping[str, Any], destination: str, expected_source: Path
) -> bool:
    mounts = inspect.get("Mounts")
    if not isinstance(mounts, list):
        return False
    matches = [
        mount
        for mount in mounts
        if isinstance(mount, dict) and mount.get("Destination") == destination
    ]
    if (
        len(matches) != 1
        or matches[0].get("RW") is not False
        or str(matches[0].get("Type") or "bind") != "bind"
        or not _source_matches(matches[0].get("Source"), expected_source)
    ):
        return False
    # Docker Desktop reports Mode="" for `--mount ...,readonly`; RW=false is
    # the live inspect authority. When Mode is populated it must agree.
    mode = str(matches[0].get("Mode") or "")
    return not mode or "ro" in mode.split(",")


def _root_filesystem_is_read_only(inspect: Mapping[str, Any]) -> bool:
    host_config = inspect.get("HostConfig")
    return (
        isinstance(host_config, dict)
        and host_config.get("ReadonlyRootfs") is True
    )


def _is_exact_docker_missing(
    proof: object, *, command: Sequence[str], subject: str
) -> bool:
    """Accept only Docker's completed, exact-object missing response."""

    if not isinstance(proof, dict):
        return False
    if proof.get("command") != list(command):
        return False
    if (
        proof.get("timed_out") is not False
        or type(proof.get("return_code")) is not int
        or proof.get("return_code") != 1
    ):
        return False
    stdout = str(proof.get("stdout") or "")
    if stdout.strip():
        try:
            if json.loads(stdout) != []:
                return False
        except json.JSONDecodeError:
            return False
    expected = re.compile(
        rf"(?:Error(?: response from daemon)?: )?No such (?:object|container): {re.escape(subject)}",
        re.IGNORECASE,
    )
    return expected.fullmatch(str(proof.get("stderr") or "").strip()) is not None


def _is_exact_docker_absence(proof: object, subject: str) -> bool:
    """Accept only Docker's completed, exact inspect-absence response."""

    return _is_exact_docker_missing(
        proof, command=["docker", "inspect", subject], subject=subject
    )


def _canonical_utc_instant(value: object) -> datetime | None:
    """Parse only the canonical zero-offset UTC form emitted by utc_now_iso."""

    if not isinstance(value, str) or not re.fullmatch(
        r"\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}(?:\.\d{6})?\+00:00", value
    ):
        return None
    try:
        parsed = datetime.fromisoformat(value)
    except ValueError:
        return None
    if parsed.tzinfo is None or parsed.utcoffset() != timezone.utc.utcoffset(parsed):
        return None
    return parsed if parsed.isoformat() == value else None


def _exact_docker_launch_id(stdout: object) -> str | None:
    """Accept one Docker-created lowercase hex id and nothing else."""

    if not isinstance(stdout, str) or re.fullmatch(r"[0-9a-f]{64}\n?", stdout) is None:
        return None
    return stdout.rstrip("\n")


def validate_packet(packet: Mapping[str, Any]) -> dict[str, Any]:
    """Acceptance validation: structure, execution success and artifacts must pass."""

    _require(packet.get("schema_version") == 1, "unsupported packet schema")
    qualified_stages = {stage.upper() for stage in STAGE_NODEIDS}
    _require(
        packet.get("stage") in qualified_stages,
        "packet stage must be a qualified executable stage",
    )
    _require(
        packet.get("prepared_before_execution") is True,
        "packet was not retained before execution",
    )
    _nonempty_command(packet)
    _validate_ttl(packet)
    environment = packet.get("environment")
    _require(
        isinstance(environment, dict)
        and environment.get(LIVE_STAGE_ENV) == "1"
        and environment.get(TTL_ENV) is None
        and environment.get(FAULT_HANDLER_ENV) == "1",
        "exact live-stage/default-TTL/fault-handler environment is absent",
    )

    execution = packet.get("execution")
    _require(isinstance(execution, dict), "execution result is absent")
    _require(execution.get("status") == "completed", "execution did not complete")
    _require(
        type(execution.get("return_code")) is int,
        "execution return code is absent",
    )
    _require(execution.get("return_code") == 0, "stage execution did not pass")

    platform = packet.get("platform")
    _require(platform in {"windows", "linux-docker"}, "unsupported platform")
    artifact_paths = _validate_artifacts(packet, str(platform))
    if platform == "windows":
        command = _nonempty_command(packet)
        expected_command = _windows_stage_command(
            Path(command[0]),
            artifact_paths["junit"].path.parent,
            str(packet["stage"]).lower(),
        )
        _require(
            command == expected_command,
            "Windows exact command is not the requested stage pytest launch",
        )
        return dict(packet)

    docker = packet.get("docker")
    _require(isinstance(docker, dict), "Docker proof is absent")
    image = docker.get("image")
    _require(isinstance(image, dict), "Docker image proof is absent")
    image_id = image.get("id")
    _require(
        isinstance(image_id, str)
        and re.fullmatch(r"sha256:[0-9a-f]{64}", image_id) is not None,
        "immutable Docker image identity is absent",
    )
    image_inspect = image.get("inspect")
    _require(
        isinstance(image_inspect, dict) and image_inspect.get("Id") == image_id,
        "Docker image inspect does not bind the image identity",
    )
    command = _nonempty_command(packet)
    _require(
        command[:2] == ["docker", "run"] and image_id in command,
        "outer Docker command is absent or does not use the inspected image id",
    )
    script_snapshot = artifact_paths["container_script"]
    expected_script = _linux_stage_script(str(packet["stage"]).lower()).encode(
        "utf-8"
    )
    _require(
        script_snapshot.data == expected_script,
        "retained container script is not the canonical requested stage launch",
    )
    expected_script_target = f"/out/{script_snapshot.path.name}"
    _require(
        len(command) >= 5
        and command[:4] == ["docker", "run", "--detach", "--name"]
        and bool(command[4])
        and not command[4].startswith("-"),
        "outer Docker command does not carry one constructor-owned container name",
    )
    expected_command = _linux_stage_command(
        repo_root=Path(str(packet["repo_root"])),
        build_root=Path(str(packet.get("build_root") or "")),
        output=script_snapshot.path.parent,
        image_id=image_id,
        container_name=command[4],
        stage=str(packet["stage"]).lower(),
    )
    expected_command[-1] = expected_script_target
    _require(
        command == expected_command,
        "outer Docker command is not the exact retained-script constructor shape",
    )
    command_text = " ".join(command).replace("\\", "/")
    repo_source = str(Path(str(packet["repo_root"])).resolve()).replace("\\", "/")
    build_source = str(Path(str(packet.get("build_root") or "")).resolve()).replace(
        "\\", "/"
    )
    _require(
        "--network none" in command_text
        and "--read-only" in command
        and f"src={repo_source},dst=/repo,readonly" in command_text
        and f"src={build_source},dst=/workspace/build,readonly" in command_text
        and "dst=/repo,readonly" in command_text
        and "dst=/workspace/build,readonly" in command_text,
        "outer Docker command lacks network-none/read-only mount isolation",
    )
    container_id = docker.get("container_id")
    _require(
        isinstance(container_id, str)
        and re.fullmatch(r"[0-9a-f]{64}", container_id) is not None,
        "retained container id is absent",
    )
    live_inspect = docker.get("live_inspect")
    _require(isinstance(live_inspect, dict), "live Docker inspect is absent")
    _require(
        live_inspect.get("Id") == container_id,
        "live Docker inspect does not bind the retained container id",
    )
    _require(
        live_inspect.get("Image") == image_id
        and live_inspect.get("Name") == f"/{command[4]}",
        "live Docker inspect does not bind the inspected image and constructor-owned name",
    )
    state = live_inspect.get("State")
    _require(
        isinstance(state, dict) and state.get("Running") is True,
        "Docker inspect was not captured while the container was live",
    )
    host_config = live_inspect.get("HostConfig")
    _require(
        isinstance(host_config, dict)
        and host_config.get("NetworkMode") == "none"
        and host_config.get("ReadonlyRootfs") is True,
        "live Docker network/root-filesystem isolation is not exact",
    )
    for destination in (LINUX_REPO_MOUNT, LINUX_BUILD_MOUNT):
        expected_source = (
            Path(str(packet["repo_root"]))
            if destination == LINUX_REPO_MOUNT
            else Path(str(packet.get("build_root") or ""))
        )
        _require(
            _read_only_mount(live_inspect, destination, expected_source),
            f"read-only live mount proof is absent for {destination}",
        )
    barrier = docker.get("barrier")
    ready_utc = _canonical_utc_instant(barrier.get("ready_inspect_utc") if isinstance(barrier, dict) else None)
    probes_utc = _canonical_utc_instant(barrier.get("probes_completed_utc") if isinstance(barrier, dict) else None)
    released_utc = _canonical_utc_instant(barrier.get("released_utc") if isinstance(barrier, dict) else None)
    _require(
        isinstance(barrier, dict)
        and barrier.get("ready") is True
        and barrier.get("released_after_probes") is True
        and ready_utc is not None and probes_utc is not None and released_utc is not None
        and ready_utc <= probes_utc <= released_utc,
        "Docker readiness/release barrier proof is absent",
    )
    write_probe = docker.get("write_probe")
    _require(isinstance(write_probe, dict), "rejected write probe is absent")
    _require(write_probe.get("attempted") is True, "write probe was not attempted")
    _require(write_probe.get("refused") is True, "write probe was not refused")
    _require(
        set(write_probe.get("paths") or [])
        == {LINUX_REPO_MOUNT, LINUX_BUILD_MOUNT},
        "write probe did not cover both read-only mounts",
    )
    attempts = write_probe.get("attempts")
    _require(isinstance(attempts, list) and len(attempts) == 2, "probe attempts absent")
    by_path = {
        entry.get("path"): entry for entry in attempts if isinstance(entry, dict)
    }
    _require(
        set(by_path) == {LINUX_REPO_MOUNT, LINUX_BUILD_MOUNT},
        "per-path probe evidence is incomplete",
    )
    for destination, attempt in by_path.items():
        command_attempt = attempt.get("command")
        expected_target = posixpath.normpath(
            f"{destination.rstrip('/')}/.part3-write-probe"
        )
        _require(
            attempt.get("container_id") == container_id
            and command_attempt == _erofs_probe_command(container_id, destination)
            and attempt.get("path") == destination
            and attempt.get("target") == expected_target,
            f"probe is not bound to {destination} and the retained container",
        )
        try:
            stdout_result = json.loads(str(attempt.get("stdout") or "").strip())
        except json.JSONDecodeError as exc:
            raise ValueError("probe stdout is not retained structured JSON") from exc
        _require(
            isinstance(stdout_result, dict)
            and stdout_result.get("path") == destination
            and stdout_result.get("target") == expected_target
            and stdout_result.get("errno") == attempt.get("errno")
            and stdout_result.get("errno_name") == attempt.get("errno_name")
            and stdout_result.get("strerror") == attempt.get("strerror"),
            f"probe stdout does not bind the {destination} refusal",
        )
        for inspect_name in ("pre_inspect", "post_inspect"):
            attempt_inspect = attempt.get(inspect_name)
            _require(
                isinstance(attempt_inspect, dict)
                and attempt_inspect.get("Id") == live_inspect.get("Id")
                and attempt_inspect.get("Image") == image_id
                and attempt_inspect.get("Name") == f"/{command[4]}"
                and isinstance(attempt_inspect.get("State"), dict)
                and attempt_inspect["State"].get("Running") is True
                and _root_filesystem_is_read_only(attempt_inspect),
                f"{inspect_name} does not prove the live probe container",
            )
            expected_source = (
                Path(str(packet["repo_root"]))
                if destination == LINUX_REPO_MOUNT
                else Path(str(packet.get("build_root") or ""))
            )
            _require(
                _read_only_mount(attempt_inspect, destination, expected_source),
                f"{inspect_name} does not bind {destination} read-only",
            )
        _require(
            attempt.get("return_code") == PROBE_REFUSAL_EXIT
            and attempt.get("errno") == 30
            and attempt.get("errno_name") == "EROFS"
            and "read-only file system" in str(attempt.get("strerror") or "").lower()
            and attempt.get("refused") is True,
            f"probe for {destination} is not an errno-specific EROFS refusal",
        )
    cleanup = docker.get("cleanup")
    _require(isinstance(cleanup, dict), "Docker cleanup proof is absent")
    _require(
        cleanup.get("command") == ["docker", "rm", "-f", container_id]
        and type(cleanup.get("return_code")) is int
        and cleanup.get("return_code") == 0
        and cleanup.get("timed_out") is False,
        "Docker cleanup command did not succeed for the exact container id",
    )
    absence = cleanup.get("post_cleanup_absence")
    _require(isinstance(absence, dict), "post-cleanup container absence is absent")
    for key, subject in (("id", container_id), ("name", command[4])):
        proof = absence.get(key)
        _require(
            _is_exact_docker_absence(proof, subject),
            f"post-cleanup exact container {key} absence is not proved",
        )
    return dict(packet)


def _write_packet(path: Path, packet: Mapping[str, Any]) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    temporary = path.with_name(f".{path.name}.{uuid.uuid4().hex}.tmp")
    temporary.write_text(
        json.dumps(packet, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )
    os.replace(temporary, path)


def _run_and_log(
    command: Sequence[str],
    *,
    cwd: Path,
    env: Mapping[str, str],
    log_path: Path,
    timeout_seconds: int,
) -> int:
    log_path.parent.mkdir(parents=True, exist_ok=True)
    with log_path.open("w", encoding="utf-8", newline="\n") as log:
        creationflags = (
            int(getattr(subprocess, "CREATE_NEW_PROCESS_GROUP", 0))
            if os.name == "nt"
            else 0
        )
        process = subprocess.Popen(
            list(command),
            cwd=str(cwd),
            env=dict(env),
            stdout=log,
            stderr=subprocess.STDOUT,
            text=True,
            creationflags=creationflags,
        )
        try:
            return int(process.wait(timeout=timeout_seconds))
        except subprocess.TimeoutExpired as exc:
            cleanup = _terminate_owned_windows_tree(process)
            log.write(f"STAGE_TIMEOUT seconds={timeout_seconds}\n")
            log.write(json.dumps({"timeout_cleanup": cleanup}, sort_keys=True) + "\n")
            raise StageTimeout("windows-pytest", timeout_seconds, cleanup) from exc


def _windows_owned_pids(root_pid: int, *, deadline: float | None = None) -> dict[str, Any]:
    """Inventory the root and descendants; an incomplete inventory is never safe."""

    if os.name != "nt":
        return {"pids": [root_pid], "complete": True, "diagnostic": "non-windows"}
    script = (
        f"$pending=@([int]{int(root_pid)});$seen=@();"
        "while($pending.Count){$currentPid=$pending[0];$pending=@($pending|Select-Object -Skip 1);"
        "if($seen -contains $currentPid){continue};$seen+= $currentPid;"
        "$pending+=@(Get-CimInstance Win32_Process -Filter ('ParentProcessId='+$currentPid) -ErrorAction Stop|ForEach-Object ProcessId)};"
        "$seen|ConvertTo-Json -Compress"
    )
    remaining = (
        WINDOWS_TIMEOUT_CLEANUP_SECONDS
        if deadline is None else deadline - time.monotonic()
    )
    if remaining <= 0:
        return {"pids": [root_pid], "complete": False, "diagnostic": "inventory_deadline"}
    try:
        completed = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
            capture_output=True,
            text=True,
            check=False,
            timeout=remaining,
        )
        if completed.returncode == 0 and not str(completed.stderr or "").strip():
            payload = json.loads(completed.stdout or "[]")
            values = payload if isinstance(payload, list) else [payload]
            if not all(type(value) is int and value > 0 for value in values):
                raise ValueError("inventory contains non-positive or non-integer pid")
            pids = sorted(set(values))
            if root_pid in pids:
                return {"pids": pids, "complete": True, "diagnostic": "accepted"}
            return {
                "pids": [root_pid], "complete": False,
                "diagnostic": "inventory_missing_root",
            }
        diagnostic = (
            "inventory_stderr" if completed.returncode == 0 else "inventory_return_code"
        )
        return {
            "pids": [root_pid], "complete": False,
            "diagnostic": diagnostic,
            "return_code": int(completed.returncode),
            "stderr": completed.stderr,
        }
    except subprocess.TimeoutExpired:
        return {"pids": [root_pid], "complete": False, "diagnostic": "inventory_timeout"}
    except (OSError, ValueError, json.JSONDecodeError) as exc:
        return {
            "pids": [root_pid], "complete": False,
            "diagnostic": f"inventory_{type(exc).__name__}",
        }


def _windows_existing_owned_pids(
    pids: Sequence[int], *, deadline: float | None = None
) -> dict[str, Any]:
    """Return a complete exact-PID aftermath inventory or fail closed."""

    if os.name != "nt":
        return {"complete": True, "existing": [], "diagnostics": ["non-windows"]}
    existing: list[int] = []
    diagnostics: list[dict[str, Any]] = []
    cleanup_deadline = (
        time.monotonic() + WINDOWS_TIMEOUT_CLEANUP_SECONDS
        if deadline is None else deadline
    )
    for pid in pids:
        remaining = cleanup_deadline - time.monotonic()
        if remaining <= 0:
            diagnostics.append({"pid": pid, "diagnostic": "aftermath_query_deadline"})
            continue
        try:
            script = (
                "@((Get-CimInstance Win32_Process -Filter "
                f"('ProcessId='+[int]{int(pid)}) -ErrorAction Stop|"
                "ForEach-Object ProcessId))|ConvertTo-Json -Compress"
            )
            completed = subprocess.run(
                [
                    "powershell", "-NoProfile", "-NonInteractive", "-Command",
                    script,
                ],
                capture_output=True,
                text=True,
                check=False,
                timeout=remaining,
            )
            if completed.returncode != 0 or str(completed.stderr or "").strip():
                diagnostics.append({
                    "pid": pid, "diagnostic": "aftermath_query_error",
                    "return_code": int(completed.returncode), "stderr": completed.stderr,
                })
                continue
            payload = json.loads(completed.stdout or "[]")
            values = payload if isinstance(payload, list) else [payload]
            if not all(type(value) is int and value > 0 for value in values):
                raise ValueError("aftermath contains non-positive or non-integer pid")
            if sorted(set(values)) not in ([], [pid]):
                raise ValueError("aftermath returned an unexpected pid")
            if values:
                existing.append(pid)
        except subprocess.TimeoutExpired:
            diagnostics.append({"pid": pid, "diagnostic": "aftermath_query_timeout"})
        except (OSError, ValueError, json.JSONDecodeError) as exc:
            diagnostics.append({"pid": pid, "diagnostic": f"aftermath_{type(exc).__name__}"})
    return {
        "complete": not diagnostics,
        "existing": existing,
        "diagnostics": diagnostics,
    }


def _terminate_owned_windows_tree(process: subprocess.Popen[Any]) -> dict[str, Any]:
    """Bounded termination of only the timeout-owned process tree."""

    cleanup_deadline = time.monotonic() + WINDOWS_TIMEOUT_CLEANUP_SECONDS
    inventory = _windows_owned_pids(int(process.pid), deadline=cleanup_deadline)
    owned = list(inventory["pids"])
    result: dict[str, Any] = {
        "root_pid": int(process.pid),
        "owned_pids": owned,
        "inventory": inventory,
        "command": ["taskkill", "/PID", str(process.pid), "/T", "/F"],
        "return_code": None,
        "timed_out": False,
        "surviving_owned_pids": [],
        "passed": False,
    }
    try:
        if os.name == "nt":
            completed = subprocess.run(
                result["command"],
                capture_output=True,
                text=True,
                check=False,
                timeout=max(0.0, cleanup_deadline - time.monotonic()),
            )
            result["return_code"] = int(completed.returncode)
            result["stdout"] = completed.stdout
            result["stderr"] = completed.stderr
        else:
            process.kill()
            result["return_code"] = 0
        process.wait(timeout=max(0.0, cleanup_deadline - time.monotonic()))
    except subprocess.TimeoutExpired:
        result["timed_out"] = True
    except OSError as exc:
        result["error"] = f"{type(exc).__name__}: {exc}"
    aftermath = _windows_existing_owned_pids(owned, deadline=cleanup_deadline)
    result["aftermath"] = aftermath
    result["surviving_owned_pids"] = list(aftermath["existing"])
    result["passed"] = (
        inventory["complete"] is True
        and aftermath["complete"] is True
        and result["return_code"] == 0
        and result["timed_out"] is False
        and not result["surviving_owned_pids"]
    )
    return result


class StageTimeout(RuntimeError):
    def __init__(self, phase: str, seconds: int, cleanup: Mapping[str, Any]) -> None:
        super().__init__(f"{phase} timed out after {seconds} seconds")
        self.phase = phase
        self.seconds = seconds
        self.cleanup = dict(cleanup)


def _windows_stage_command(
    python_executable: Path, output: Path, stage_key: str
) -> list[str]:
    return [
        str(python_executable.resolve()),
        "-m",
        "pytest",
        STAGE_NODEIDS[stage_key],
        "-vv",
        "-p",
        "no:cacheprovider",
        "--color=no",
        f"--junitxml={output / f'stage-{stage_key}-junit.xml'}",
    ]


def _copy_retained(source: Path, target: Path) -> dict[str, Any]:
    _require(source.is_file(), f"source artifact is absent: {source}")
    shutil.copy2(source, target)
    return _artifact_record(target)


def _retain_host_fcstd_copies(evidence: Mapping[str, Any], output: Path) -> dict[str, Any]:
    """Copy each host-visible producer FCStd before any platform cleanup."""

    copies: dict[str, Any] = {}
    for index, source_path in enumerate(_fcstd_evidence_paths(evidence)):
        source = Path(source_path)
        target = _retained_fcstd_target(output, index, source_path)
        target.parent.mkdir(parents=True, exist_ok=True)
        identity = os.path.normcase(os.path.normpath(source_path)).casefold()
        _require(identity not in copies, "FCStd source identity is duplicate")
        copies[identity] = {
            "source_path": source_path,
            "artifact": _copy_retained(source, target),
        }
    return copies


def _retain_linux_fcstd_copies(
    packet: dict[str, Any], *, output: Path
) -> dict[str, Any]:
    """Bind FCStd files bridged from container tmpfs before process exit."""

    evidence_path = output / f"stage-{str(packet['stage']).lower()}-evidence.json"
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    # Timeout/cleanup controls deliberately retain skeletal evidence and mock
    # the final packet gate.  A completed packet still fails closed there.
    artifacts = evidence.get("artifacts") if isinstance(evidence, dict) else None
    if not isinstance(artifacts, dict) or not isinstance(artifacts.get("documents"), list):
        return {}
    copies: dict[str, Any] = {}
    for index, source_path in enumerate(_fcstd_evidence_paths(evidence)):
        target = _retained_fcstd_target(output, index, source_path)
        _require(
            target.is_file(),
            "Linux FCStd bridge did not retain the producer container path",
        )
        identity = os.path.normcase(os.path.normpath(source_path)).casefold()
        _require(identity not in copies, "FCStd source identity is duplicate")
        copies[identity] = {
            "source_path": source_path,
            "artifact": _artifact_record(target),
        }
    return copies


def _retain_linux_binary_copies(
    packet: dict[str, Any], *, output: Path
) -> None:
    """Copy producer-addressed Linux binaries through the inspected bind mount.

    Linux evidence names binaries in the container namespace.  The retained
    copy is the host-verifiable bridge after that namespace is gone. Copying
    from the bind source preserves nanosecond timestamps that ``docker cp``
    truncates on Docker Desktop.
    """

    evidence_path = output / f"stage-{str(packet['stage']).lower()}-evidence.json"
    evidence = json.loads(evidence_path.read_text(encoding="utf-8"))
    fingerprints = ((evidence.get("environment") or {}).get("binary_fingerprint") or {})
    # Timeout/cleanup unit controls retain a deliberately skeletal evidence
    # stub and replace the final packet validator.  Production evidence is
    # still required to carry fingerprints by that final fail-closed gate.
    if not isinstance(fingerprints, dict) or not fingerprints:
        return
    build_root_value = packet.get("build_root")
    _require(isinstance(build_root_value, str) and build_root_value, "Linux build root is absent")
    build_root = Path(build_root_value).resolve()
    producer_root = PurePosixPath(LINUX_BUILD_MOUNT)
    copies: dict[str, Any] = {}
    for name, fingerprint in fingerprints.items():
        _require(isinstance(name, str) and isinstance(fingerprint, dict), "Linux binary evidence is malformed")
        container_path = fingerprint.get("path")
        _require(
            isinstance(container_path, str),
            "Linux binary evidence is not a producer container build path",
        )
        try:
            relative = PurePosixPath(container_path).relative_to(producer_root)
        except ValueError as exc:
            raise ValueError(
                "Linux binary evidence is not a producer container build path"
            ) from exc
        _require(
            bool(relative.parts) and ".." not in relative.parts,
            "Linux binary evidence escapes the producer build root",
        )
        _require(
            name == relative.name
            and name not in {".", ".."}
            and "/" not in name
            and "\\" not in name
            and ":" not in name,
            "Linux binary evidence name is not a safe producer basename",
        )
        source = build_root.joinpath(*relative.parts).resolve()
        _require(
            source.is_relative_to(build_root) and source.is_file(),
            "Linux binary producer bind source is unavailable",
        )
        source_info = source.stat()
        _require(
            hashlib.sha256(source.read_bytes()).hexdigest() == fingerprint.get("sha256")
            and source_info.st_size == fingerprint.get("size")
            and source_info.st_mtime_ns == fingerprint.get("mtime_ns"),
            "Linux binary producer bind source does not match its fingerprint",
        )
        target = output / f"stage-{str(packet['stage']).lower()}-binary-{name}"
        shutil.copy2(source, target)
        target_info = target.stat()
        _require(
            target.is_file()
            and target_info.st_size == source_info.st_size
            and target_info.st_mtime_ns == source_info.st_mtime_ns,
            "Linux binary copy did not preserve the producer bind identity",
        )
        copies[name] = {
            "container_path": container_path,
            "artifact": _artifact_record(target),
        }
    packet["binary_copies"] = copies


def _collect_windows_artifacts(
    *,
    output: Path,
    stage_key: str,
    runner_log: Path,
    handoff_path: Path,
    retain_fcstd: bool,
) -> dict[str, Any]:
    _require(handoff_path.is_file(), "pytest did not produce the stage evidence handoff")
    handoff = json.loads(handoff_path.read_text(encoding="utf-8"))
    _require(
        handoff.get("schema_version") == 1
        and handoff.get("stage") == stage_key.upper()
        and type(handoff.get("coordinator_return_code")) is int,
        "stage evidence handoff is malformed",
    )
    evidence_source = Path(str(handoff.get("evidence_path") or ""))
    launcher_source = Path(str(handoff.get("launcher_path") or ""))
    evidence = json.loads(evidence_source.read_text(encoding="utf-8"))
    _require(isinstance(evidence, dict), "stage evidence is malformed")
    artifacts = {
        "runner_log": _artifact_record(runner_log),
        "junit": _artifact_record(output / f"stage-{stage_key}-junit.xml"),
        "handoff": _artifact_record(handoff_path),
        "evidence": _copy_retained(
            evidence_source, output / f"stage-{stage_key}-evidence.json"
        ),
        "launcher_log": _copy_retained(
            launcher_source, output / f"stage-{stage_key}-launcher.log"
        ),
    }
    artifacts["fcstd"] = (
        _retain_host_fcstd_copies(evidence, output) if retain_fcstd else {}
    )
    return artifacts


def run_windows(
    *,
    repo_root: Path,
    output_dir: Path,
    stage: str,
    python_executable: Path,
) -> int:
    stage_key = stage.lower()
    output = output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    command = _windows_stage_command(python_executable, output, stage_key)
    env = dict(os.environ)
    env[LIVE_STAGE_ENV] = "1"
    env[FAULT_HANDLER_ENV] = "1"
    env.pop(TTL_ENV, None)
    handoff_path = output / f"stage-{stage_key}-handoff.json"
    handoff_path.unlink(missing_ok=True)
    env[HANDOFF_ENV] = str(handoff_path)
    packet_path = output / f"stage-{stage_key}-execution-packet.json"
    packet: dict[str, Any] = {
        "schema_version": 1,
        "platform": "windows",
        "stage": stage_key.upper(),
        "repo_root": str(repo_root.resolve()),
        "prepared_before_execution": True,
        "prepared_utc": utc_now_iso(),
        "exact_command": command,
        "environment": {
            LIVE_STAGE_ENV: env[LIVE_STAGE_ENV],
            TTL_ENV: None,
            FAULT_HANDLER_ENV: env[FAULT_HANDLER_ENV],
            HANDOFF_ENV: str(handoff_path),
        },
        "ttl": session_ttl_provenance(repo_root, environ=env),
        "execution": {"status": "prepared", "return_code": None},
    }
    _write_packet(packet_path, packet)
    runner_log = output / f"stage-{stage_key}-runner.log"
    try:
        return_code = _run_and_log(
            command,
            cwd=repo_root,
            env=env,
            log_path=runner_log,
            timeout_seconds=STAGE_EXECUTION_TIMEOUT_SECONDS[stage_key],
        )
    except StageTimeout as exc:
        packet["execution"] = {
            "status": "timed_out",
            "return_code": 124,
            "classification": "timeout",
            "phase": exc.phase,
            "timeout_seconds": exc.seconds,
            "cleanup": exc.cleanup,
            "aftermath": {
                "owned_pids": exc.cleanup.get("owned_pids"),
                "surviving_owned_pids": exc.cleanup.get("surviving_owned_pids"),
                "passed": exc.cleanup.get("passed"),
            },
            "finished_utc": utc_now_iso(),
        }
        _write_packet(packet_path, packet)
        return 124
    packet["execution"] = {
        "status": "completed",
        "return_code": return_code,
        "finished_utc": utc_now_iso(),
    }
    _write_packet(packet_path, packet)
    try:
        packet["artifacts"] = _collect_windows_artifacts(
            output=output,
            stage_key=stage_key,
            runner_log=runner_log,
            handoff_path=handoff_path,
            retain_fcstd=return_code == 0,
        )
    except Exception as exc:
        packet["artifact_collection_error"] = f"{type(exc).__name__}: {exc}"
        _write_packet(packet_path, packet)
        if return_code == 0:
            raise
    _write_packet(packet_path, packet)
    if return_code != 0:
        return return_code
    validate_packet(packet)
    return return_code


def _linux_stage_script(stage: str) -> str:
    nodeid = STAGE_NODEIDS[stage]
    return f"""#!/usr/bin/env bash
set -uo pipefail
release=/out/stage-{stage}.release
release_deadline=$((SECONDS + {LINUX_RELEASE_BARRIER_TIMEOUT_SECONDS}))
while [ ! -f "$release" ]; do
  if [ "$SECONDS" -ge "$release_deadline" ]; then
    echo "release_barrier_timeout={LINUX_RELEASE_BARRIER_TIMEOUT_SECONDS}" >/out/stage-{stage}-aftermath.txt
    exit 124
  fi
  sleep 0.1
done
export DISPLAY=:99
export QT_QPA_PLATFORM=xcb
export LD_LIBRARY_PATH=/workspace/build/debug/lib${{LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}}
export PYTHONPATH=/repo/.pixi/envs/default/Lib/site-packages:/repo
export PYTHONUNBUFFERED=1
export PYTHONDONTWRITEBYTECODE=1
export PYTHONFAULTHANDLER=1
export PART3_STAGE_LIVE=1
export FREECAD_EXE=/workspace/build/debug/bin/FreeCAD
unset FREECAD_MCP_SESSION_TTL_SECONDS
Xvfb :99 -screen 0 1280x1024x24 -nolisten tcp >/out/stage-{stage}-xvfb.log 2>&1 &
xvfb_pid=$!
for _ in $(seq 1 40); do [ -f /tmp/.X99-lock ] && break; sleep 0.25; done
cd /repo
python3 -m pytest {nodeid} -vv -p no:cacheprovider --color=no --junitxml=/out/stage-{stage}-junit.xml
rc=$?
handoff=${{PART3_STAGE_EVIDENCE_HANDOFF:-}}
if ! python3 - "$handoff" "{stage.upper()}" /out/stage-{stage}-evidence.json /out/stage-{stage}-launcher.log <<'PY'
import hashlib, json, os, shutil, sys
handoff, stage, evidence_target, launcher_target = sys.argv[1:]
try:
    with open(handoff, encoding="utf-8") as stream:
        payload = json.load(stream)
    assert payload.get("schema_version") == 1
    assert payload.get("stage") == stage
    assert type(payload.get("coordinator_return_code")) is int
    evidence = payload.get("evidence_path")
    launcher = payload.get("launcher_path")
    assert isinstance(evidence, str) and evidence
    assert isinstance(launcher, str) and launcher
    shutil.copy2(evidence, evidence_target)
    shutil.copy2(launcher, launcher_target)
    with open(evidence, encoding="utf-8") as stream:
        evidence_payload = json.load(stream)
    artifacts = evidence_payload.get("artifacts")
    documents = artifacts.get("documents") if isinstance(artifacts, dict) else None
    assert isinstance(documents, list)
    sources = []
    seen = set()
    for entry in documents:
        source = entry.get("path") if isinstance(entry, dict) else None
        if not isinstance(source, str) or not source.lower().endswith(".fcstd"):
            continue
        identity = os.path.normcase(os.path.normpath(source)).casefold()
        assert identity and identity not in seen
        seen.add(identity)
        sources.append(source)
    assert sources
    retained_root = "/out/retained-fcstd"
    os.makedirs(retained_root, exist_ok=True)
    for index, source in enumerate(sources):
        source_name = os.path.basename(source)
        assert source_name.lower().endswith(".fcstd")
        token = hashlib.sha256(source.encode("utf-8")).hexdigest()[:16]
        retained_target = os.path.join(
            retained_root, "%03d-%s-%s" % (index, token, source_name)
        )
        shutil.copy2(source, retained_target)
except (AssertionError, OSError, ValueError, TypeError, json.JSONDecodeError):
    raise SystemExit(1)
PY
then
  rc=1
fi
freecad_leftovers=$(pgrep -a -f '/workspace/build/debug/bin/FreeCAD|/repo/start_freecad.py' || true)
listener_9875=$(ss -lntp 2>/dev/null | awk '$4 ~ /:9875$/ {{print}}' || true)
{{
  echo "pytest_rc=$rc"
  echo "freecad_or_launcher_leftovers_begin"
  printf '%s\n' "$freecad_leftovers"
  echo "freecad_or_launcher_leftovers_end"
  echo "port_9875_listeners_begin"
  printf '%s\n' "$listener_9875"
  echo "port_9875_listeners_end"
}} >/out/stage-{stage}-aftermath.txt
if [ -n "$freecad_leftovers" ] || [ -n "$listener_9875" ]; then rc=1; fi
kill "$xvfb_pid" >/dev/null 2>&1 || true
wait "$xvfb_pid" 2>/dev/null || true
exit "$rc"
"""


def _linux_stage_command(
    *,
    repo_root: Path,
    build_root: Path,
    output: Path,
    image_id: str,
    container_name: str,
    stage: str,
) -> list[str]:
    return [
        "docker",
        "run",
        "--detach",
        "--name",
        container_name,
        "--network",
        "none",
        "--read-only",
        "--tmpfs",
        "/tmp:rw,nosuid,nodev,size=2g",
        "--mount",
        f"type=bind,src={repo_root.resolve()},dst={LINUX_REPO_MOUNT},readonly",
        "--mount",
        f"type=bind,src={build_root.resolve()},dst={LINUX_BUILD_MOUNT},readonly",
        "--mount",
        f"type=bind,src={output.resolve()},dst=/out",
        "--env",
        f"{HANDOFF_ENV}=/out/stage-{stage}-handoff.json",
        "--entrypoint",
        "/bin/bash",
        image_id,
        f"/out/stage-{stage}-container.sh",
    ]


class DockerControlTimeout(RuntimeError):
    def __init__(self, phase: str) -> None:
        super().__init__(f"Docker control timed out during {phase}")
        self.phase = phase


class ReleaseBarrierExpired(RuntimeError):
    pass


def _docker_json(command: Sequence[str], *, phase: str) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            list(command), capture_output=True, text=True, check=False,
            timeout=LINUX_DOCKER_CONTROL_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise DockerControlTimeout(phase) from exc
    if completed.returncode != 0:
        raise RuntimeError(
            f"Docker command failed ({completed.returncode}): {completed.stderr.strip()}"
        )
    payload = json.loads(completed.stdout)
    if isinstance(payload, list):
        if len(payload) != 1 or not isinstance(payload[0], dict):
            raise RuntimeError("Docker inspect did not return exactly one object")
        return payload[0]
    if not isinstance(payload, dict):
        raise RuntimeError("Docker command did not return an object")
    return payload


def _erofs_probe_command(container_id: str, destination: str) -> list[str]:
    probe_source = """import errno, json, os, sys
target = os.path.join(sys.argv[1], '.part3-write-probe')
try:
    with open(target, 'xb'):
        pass
except OSError as exc:
    print(json.dumps({'path': sys.argv[1], 'target': target, 'errno': exc.errno, 'errno_name': errno.errorcode.get(exc.errno), 'strerror': exc.strerror}))
    raise SystemExit(73 if exc.errno == errno.EROFS else 74)
else:
    os.unlink(target)
    print(json.dumps({'path': sys.argv[1], 'target': target, 'errno': None, 'errno_name': None, 'strerror': None}))
    raise SystemExit(0)
"""
    return [
        "docker",
        "exec",
        container_id,
        "python3",
        "-c",
        probe_source,
        destination,
    ]


def _run_erofs_probe(container_id: str, destination: str) -> dict[str, Any]:
    pre_inspect = _docker_json(
        ["docker", "inspect", container_id], phase="probe-pre-inspect"
    )
    state = pre_inspect.get("State")
    if isinstance(state, dict) and state.get("Running") is False and state.get("ExitCode") == 124:
        raise ReleaseBarrierExpired()
    command = _erofs_probe_command(container_id, destination)
    try:
        completed = subprocess.run(
            command, capture_output=True, text=True, check=False,
            timeout=LINUX_DOCKER_CONTROL_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        raise DockerControlTimeout("probe-exec") from exc
    post_inspect = _docker_json(
        ["docker", "inspect", container_id], phase="probe-post-inspect"
    )
    state = post_inspect.get("State")
    if isinstance(state, dict) and state.get("Running") is False and state.get("ExitCode") == 124:
        raise ReleaseBarrierExpired()
    try:
        result = json.loads((completed.stdout or "").strip())
    except json.JSONDecodeError:
        result = {}
    if not isinstance(result, dict):
        result = {}
    target = posixpath.normpath(f"{destination.rstrip('/')}/.part3-write-probe")
    exact_refusal = (
        completed.returncode == PROBE_REFUSAL_EXIT
        and result.get("path") == destination
        and result.get("target") == target
        and result.get("errno") == 30
        and result.get("errno_name") == "EROFS"
        and "read-only file system" in str(result.get("strerror") or "").lower()
    )
    return {
        "path": destination,
        "container_id": container_id,
        "command": command,
        "return_code": int(completed.returncode),
        "errno": result.get("errno"),
        "errno_name": result.get("errno_name"),
        "strerror": result.get("strerror"),
        "target": result.get("target"),
        "refused": exact_refusal,
        "stdout": completed.stdout,
        "stderr": completed.stderr,
        "pre_inspect": pre_inspect,
        "post_inspect": post_inspect,
    }


def _docker_result(command: Sequence[str], *, timeout: int | None = None) -> dict[str, Any]:
    try:
        completed = subprocess.run(
            list(command), capture_output=True, text=True, check=False, timeout=timeout
        )
        return {
            "command": list(command),
            "return_code": int(completed.returncode),
            "stdout": completed.stdout,
            "stderr": completed.stderr,
            "timed_out": False,
        }
    except subprocess.TimeoutExpired as exc:
        return {
            "command": list(command),
            "return_code": 124,
            "stdout": str(exc.stdout or ""),
            "stderr": str(exc.stderr or ""),
            "timed_out": True,
            "timeout_seconds": timeout,
        }


def _cleanup_linux_container(
    packet: dict[str, Any], *, container_id: str | None, container_name: str
) -> bool:
    """Bounded cleanup with exact absence proof for every known identity.

    A launch timeout has no trustworthy id, but the generated --name is ours
    and is sufficient to reconcile a daemon-accepted launch without touching
    unrelated containers.  Docker reports a never-created name as a completed
    exact missing response from ``rm``; that is accepted only with an exact
    follow-up ``inspect`` absence proof.
    """

    cleanup_subject = container_id or container_name
    cleanup = _docker_result(
        ["docker", "rm", "-f", cleanup_subject],
        timeout=LINUX_DOCKER_CONTROL_TIMEOUT_SECONDS,
    )
    subjects = [("name", container_name)]
    if container_id:
        subjects.insert(0, ("id", container_id))
    absence = {
        key: _docker_result(["docker", "inspect", subject], timeout=LINUX_DOCKER_CONTROL_TIMEOUT_SECONDS)
        for key, subject in subjects
    }
    packet["docker"]["cleanup"] = {
        **cleanup,
        "target": {"kind": "id" if container_id else "name", "subject": cleanup_subject},
        "post_cleanup_absence": absence,
        "completed_utc": utc_now_iso(),
    }
    cleanup_completed = cleanup["return_code"] == 0 and cleanup["timed_out"] is False
    cleanup_missing = _is_exact_docker_missing(
        cleanup,
        command=["docker", "rm", "-f", cleanup_subject],
        subject=cleanup_subject,
    )
    return (
        (cleanup_completed or cleanup_missing)
        and all(_is_exact_docker_absence(absence[key], subject) for key, subject in subjects)
    )


def _finalize_docker_launch_timeout(
    *,
    packet: dict[str, Any],
    packet_path: Path,
    container_name: str,
    exception: subprocess.TimeoutExpired,
) -> int:
    """Fail closed if Docker may have accepted a timed-out detached launch."""

    packet["docker"]["launch_timeout"] = {
        "command": list(exception.cmd) if isinstance(exception.cmd, (list, tuple)) else str(exception.cmd),
        "container_name": container_name,
        "stdout": str(exception.stdout or ""),
        "stderr": str(exception.stderr or ""),
        "timed_out": True,
        "timeout_seconds": LINUX_DOCKER_CONTROL_TIMEOUT_SECONDS,
    }
    packet["execution"] = {
        "status": "timed_out", "return_code": 124,
        "classification": "timeout", "phase": "docker-launch",
        "timeout_seconds": LINUX_DOCKER_CONTROL_TIMEOUT_SECONDS,
        "finished_utc": utc_now_iso(),
    }
    cleanup_ok = _cleanup_linux_container(
        packet, container_id=None, container_name=container_name
    )
    packet["execution"]["cleanup_verified"] = cleanup_ok
    _write_packet(packet_path, packet)
    return 124


def _finalize_docker_launch_failure(
    *,
    packet: dict[str, Any],
    packet_path: Path,
    container_name: str,
    launched: subprocess.CompletedProcess[str],
) -> int:
    """Reconcile a non-timeout launch that did not yield an exact id."""

    return_code = int(launched.returncode) or 1
    packet["docker"]["launch_failure"] = {
        "command": list(launched.args) if isinstance(launched.args, (list, tuple)) else str(launched.args),
        "container_name": container_name,
        "return_code": int(launched.returncode),
        "stdout": str(launched.stdout or ""),
        "stderr": str(launched.stderr or ""),
        "timed_out": False,
    }
    packet["execution"] = {
        "status": "completed", "return_code": return_code,
        "classification": "launch-failure", "phase": "docker-launch",
        "finished_utc": utc_now_iso(),
    }
    cleanup_ok = _cleanup_linux_container(
        packet, container_id=None, container_name=container_name
    )
    packet["execution"]["cleanup_verified"] = cleanup_ok
    _write_packet(packet_path, packet)
    return return_code


def _is_release_barrier_timeout(waited: Mapping[str, Any]) -> bool:
    """The container's reserved 124 exit is a barrier timeout, not pytest."""

    return (
        waited.get("timed_out") is False
        and waited.get("return_code") == 0
        and str(waited.get("stdout") or "").strip() == "124"
    )


def _pre_release_barrier_expired(inspect: Mapping[str, Any], release_path: Path) -> bool:
    """Recognize the reserved barrier exit before the host creates release."""

    state = inspect.get("State")
    return (
        not release_path.exists()
        and isinstance(state, dict)
        and state.get("Running") is False
        and state.get("ExitCode") == 124
    )


def _finalize_release_barrier_timeout(
    *,
    packet: dict[str, Any],
    packet_path: Path,
    output: Path,
    stage_key: str,
    container_id: str,
    container_name: str,
) -> int:
    aftermath_path = output / f"stage-{stage_key}-aftermath.txt"
    aftermath_path.write_text(
        f"release_barrier_timeout={LINUX_RELEASE_BARRIER_TIMEOUT_SECONDS}\n"
        "pytest_started=false\n",
        encoding="utf-8",
        newline="\n",
    )
    packet["execution"] = {
        "status": "timed_out",
        "return_code": 124,
        "classification": "timeout",
        "phase": "release-barrier",
        "timeout_seconds": LINUX_RELEASE_BARRIER_TIMEOUT_SECONDS,
        "pytest_started": False,
        "aftermath": _artifact_record(aftermath_path),
        "finished_utc": utc_now_iso(),
    }
    cleanup_ok = _cleanup_linux_container(
        packet, container_id=container_id, container_name=container_name
    )
    packet["execution"]["cleanup_verified"] = cleanup_ok
    _write_packet(packet_path, packet)
    _require(cleanup_ok, "Docker cleanup did not prove exact id/name absence")
    return 124


def _finalize_docker_control_timeout(
    *,
    packet: dict[str, Any],
    packet_path: Path,
    phase: str,
    container_id: str,
    container_name: str,
) -> int:
    packet["execution"] = {
        "status": "timed_out",
        "return_code": 124,
        "classification": "timeout",
        "phase": phase,
        "timeout_seconds": LINUX_DOCKER_CONTROL_TIMEOUT_SECONDS,
        "finished_utc": utc_now_iso(),
    }
    cleanup_ok = _cleanup_linux_container(
        packet, container_id=container_id, container_name=container_name
    )
    packet["execution"]["cleanup_verified"] = cleanup_ok
    _write_packet(packet_path, packet)
    return 124


def _finalize_docker_control_failure(
    *,
    packet: dict[str, Any],
    packet_path: Path,
    phase: str,
    control: Mapping[str, Any],
    container_id: str,
    container_name: str,
) -> int:
    """Retain a completed Docker control failure before exact cleanup."""

    return_code = int(control.get("return_code") or 1)
    packet["docker"].setdefault("control_failures", {})[phase] = dict(control)
    packet["execution"] = {
        "status": "completed", "return_code": return_code,
        "classification": "control-failure", "phase": phase,
        "finished_utc": utc_now_iso(),
    }
    cleanup_ok = _cleanup_linux_container(
        packet, container_id=container_id, container_name=container_name
    )
    packet["execution"]["cleanup_verified"] = cleanup_ok
    _write_packet(packet_path, packet)
    return return_code


def run_linux_docker(
    *,
    repo_root: Path,
    build_root: Path,
    output_dir: Path,
    stage: str,
    image: str,
) -> int:
    stage_key = stage.lower()
    output = output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    script_path = output / f"stage-{stage_key}-container.sh"
    script_path.write_text(
        _linux_stage_script(stage_key), encoding="utf-8", newline="\n"
    )
    release_path = output / f"stage-{stage_key}.release"
    release_path.unlink(missing_ok=True)

    image_inspect = _docker_json(
        ["docker", "image", "inspect", image], phase="image-inspect"
    )
    image_id = str(image_inspect.get("Id") or "")
    name = f"part3-{stage_key}-{uuid.uuid4().hex[:12]}"
    command = _linux_stage_command(
        repo_root=repo_root,
        build_root=build_root,
        output=output,
        image_id=image_id,
        container_name=name,
        stage=stage_key,
    )
    packet_path = output / f"stage-{stage_key}-execution-packet.json"
    ttl_env = dict(os.environ)
    ttl_env.pop(TTL_ENV, None)
    packet: dict[str, Any] = {
        "schema_version": 1,
        "platform": "linux-docker",
        "stage": stage_key.upper(),
        "repo_root": str(repo_root.resolve()),
        "build_root": str(build_root.resolve()),
        "prepared_before_execution": True,
        "prepared_utc": utc_now_iso(),
        "exact_command": command,
        "environment": {
            LIVE_STAGE_ENV: "1",
            TTL_ENV: None,
            FAULT_HANDLER_ENV: "1",
            HANDOFF_ENV: f"/out/stage-{stage_key}-handoff.json",
        },
        "ttl": session_ttl_provenance(repo_root, environ=ttl_env),
        "docker": {
            "image": {"requested": image, "id": image_id, "inspect": image_inspect},
            "container_id": None,
            "live_inspect": None,
            "barrier": {
                "path": str(release_path),
                "ready": False,
                "ready_inspect_utc": None,
                "probes_completed_utc": None,
                "released_after_probes": False,
                "released_utc": None,
            },
            "write_probe": None,
            "cleanup": None,
        },
        "execution": {"status": "prepared", "return_code": None},
    }
    _write_packet(packet_path, packet)

    try:
        launched = subprocess.run(
            command, capture_output=True, text=True, check=False,
            timeout=LINUX_DOCKER_CONTROL_TIMEOUT_SECONDS,
        )
    except subprocess.TimeoutExpired as exc:
        return _finalize_docker_launch_timeout(
            packet=packet,
            packet_path=packet_path,
            container_name=name,
            exception=exc,
        )
    container_id = _exact_docker_launch_id(launched.stdout)
    if launched.returncode != 0 or container_id is None:
        return _finalize_docker_launch_failure(
            packet=packet,
            packet_path=packet_path,
            container_name=name,
            launched=launched,
        )
    packet["docker"]["container_id"] = container_id
    try:
        try:
            live_inspect = _docker_json(
                ["docker", "inspect", container_id], phase="live-inspect"
            )
        except DockerControlTimeout as exc:
            return _finalize_docker_control_timeout(
                packet=packet, packet_path=packet_path, phase=exc.phase,
                container_id=container_id, container_name=name,
            )
        packet["docker"]["live_inspect"] = live_inspect
        if _pre_release_barrier_expired(live_inspect, release_path):
            return _finalize_release_barrier_timeout(
                packet=packet,
                packet_path=packet_path,
                output=output,
                stage_key=stage_key,
                container_id=container_id,
                container_name=name,
            )
        _require(
            _root_filesystem_is_read_only(live_inspect),
            "Docker root filesystem was not read-only before probe release",
        )
        packet["docker"]["barrier"]["ready"] = bool(
            isinstance(live_inspect.get("State"), dict)
            and live_inspect["State"].get("Running") is True
        )
        packet["docker"]["barrier"]["ready_inspect_utc"] = utc_now_iso()
        try:
            attempts = [
                _run_erofs_probe(container_id, destination)
                for destination in (LINUX_REPO_MOUNT, LINUX_BUILD_MOUNT)
            ]
        except ReleaseBarrierExpired:
            return _finalize_release_barrier_timeout(
                packet=packet,
                packet_path=packet_path,
                output=output,
                stage_key=stage_key,
                container_id=container_id,
                container_name=name,
            )
        except DockerControlTimeout as exc:
            return _finalize_docker_control_timeout(
                packet=packet,
                packet_path=packet_path,
                phase=exc.phase,
                container_id=container_id,
                container_name=name,
            )
        packet["docker"]["write_probe"] = {
            "attempted": True,
            "paths": [entry["path"] for entry in attempts],
            "refused": all(entry["refused"] for entry in attempts),
            "attempts": attempts,
        }
        packet["docker"]["barrier"]["probes_completed_utc"] = utc_now_iso()
        _write_packet(packet_path, packet)
        # This partial validator is deliberately performed before releasing the
        # stage. A malformed/failed probe therefore leads directly to cleanup.
        for attempt in attempts:
            for inspect_name in ("pre_inspect", "post_inspect"):
                inspect = attempt.get(inspect_name)
                if isinstance(inspect, dict) and _pre_release_barrier_expired(
                    inspect, release_path
                ):
                    return _finalize_release_barrier_timeout(
                        packet=packet,
                        packet_path=packet_path,
                        output=output,
                        stage_key=stage_key,
                        container_id=container_id,
                        container_name=name,
                    )
            _require(attempt["refused"] is True, "mount probe did not prove EROFS")
            for inspect_name in ("pre_inspect", "post_inspect"):
                inspect = attempt[inspect_name]
                _require(
                    inspect.get("Id") == live_inspect.get("Id")
                    and isinstance(inspect.get("State"), dict)
                    and inspect["State"].get("Running") is True
                    and _root_filesystem_is_read_only(inspect),
                    "container stopped or changed identity during mount probes",
                )
        release_path.write_text("release\n", encoding="utf-8", newline="\n")
        packet["docker"]["barrier"]["released_after_probes"] = True
        packet["docker"]["barrier"]["released_utc"] = utc_now_iso()
        _write_packet(packet_path, packet)
        stage_timeout = STAGE_EXECUTION_TIMEOUT_SECONDS[stage_key]
        waited = _docker_result(
            ["docker", "wait", container_id], timeout=stage_timeout
        )
        if waited["timed_out"]:
            return_code = 124
            packet["execution"] = {
                "status": "timed_out",
                "return_code": return_code,
                "classification": "timeout",
                "phase": "docker-wait",
                "timeout_seconds": stage_timeout,
                "finished_utc": utc_now_iso(),
            }
        elif _is_release_barrier_timeout(waited):
            return _finalize_release_barrier_timeout(
                packet=packet,
                packet_path=packet_path,
                output=output,
                stage_key=stage_key,
                container_id=container_id,
                container_name=name,
            )
        else:
            return_code = (
                int(str(waited["stdout"]).strip())
                if waited["return_code"] == 0
                else int(waited["return_code"])
            )
        logs = _docker_result(
            ["docker", "logs", container_id],
            timeout=LINUX_DOCKER_CONTROL_TIMEOUT_SECONDS,
        )
        if logs["timed_out"]:
            return _finalize_docker_control_timeout(
                packet=packet, packet_path=packet_path, phase="docker-logs",
                container_id=container_id, container_name=name,
            )
        if logs["return_code"] != 0:
            return _finalize_docker_control_failure(
                packet=packet,
                packet_path=packet_path,
                phase="docker-logs",
                control=logs,
                container_id=container_id,
                container_name=name,
            )
        (output / f"stage-{stage_key}-runner.log").write_text(
            (str(logs["stdout"]) or "") + (str(logs["stderr"]) or ""),
            encoding="utf-8",
            newline="\n",
        )
        if packet["execution"]["status"] != "timed_out":
            packet["execution"] = {
                "status": "completed",
                "return_code": int(return_code),
                "finished_utc": utc_now_iso(),
            }
        _write_packet(packet_path, packet)
        artifact_paths = {
            "runner_log": output / f"stage-{stage_key}-runner.log",
            "evidence": output / f"stage-{stage_key}-evidence.json",
            "launcher_log": output / f"stage-{stage_key}-launcher.log",
            "junit": output / f"stage-{stage_key}-junit.xml",
            "handoff": output / f"stage-{stage_key}-handoff.json",
            "aftermath": output / f"stage-{stage_key}-aftermath.txt",
            "container_script": script_path,
            "release_barrier": release_path,
        }
        try:
            if int(return_code) == 0:
                packet["artifacts"] = {
                    name: _artifact_record(path)
                    for name, path in artifact_paths.items()
                }
                packet["artifacts"]["fcstd"] = _retain_linux_fcstd_copies(
                    packet, output=output
                )
                _retain_linux_binary_copies(
                    packet, output=output
                )
            else:
                packet["artifacts"] = {
                    name: _artifact_record(path)
                    for name, path in artifact_paths.items()
                    if path.is_file()
                }
                missing = sorted(
                    name for name, path in artifact_paths.items() if not path.is_file()
                )
                packet["artifacts"]["fcstd"] = {}
                if missing:
                    packet["artifact_collection_error"] = (
                        "failed stage did not publish: " + ", ".join(missing)
                    )
        except Exception as exc:
            packet["artifact_collection_error"] = f"{type(exc).__name__}: {exc}"
            _write_packet(packet_path, packet)
            if int(return_code) == 0:
                raise
        _write_packet(packet_path, packet)
        cleanup_ok = _cleanup_linux_container(
            packet, container_id=container_id, container_name=name
        )
        packet["execution"]["cleanup_verified"] = cleanup_ok
        _write_packet(packet_path, packet)
        _require(cleanup_ok, "Docker cleanup did not prove exact id/name absence")
        if int(return_code) != 0:
            return int(return_code)
        validate_packet(packet)
        return int(return_code)
    finally:
        if packet["docker"].get("cleanup") is None:
            cleanup_ok = _cleanup_linux_container(
                packet, container_id=container_id, container_name=name
            )
            _write_packet(packet_path, packet)
            if not cleanup_ok:
                raise RuntimeError("Docker finally cleanup did not prove exact id/name absence")


def _parser() -> argparse.ArgumentParser:
    parser = argparse.ArgumentParser(description=__doc__)
    subparsers = parser.add_subparsers(dest="command", required=True)

    validate = subparsers.add_parser("validate")
    validate.add_argument("packet", type=Path)

    windows = subparsers.add_parser("windows")
    windows.add_argument("--stage", choices=sorted(STAGE_NODEIDS), required=True)
    windows.add_argument("--repo", type=Path, default=REPO_ROOT)
    windows.add_argument("--output", type=Path, required=True)
    windows.add_argument("--python", type=Path, default=Path(sys.executable))

    linux = subparsers.add_parser("linux-docker")
    linux.add_argument("--stage", choices=sorted(STAGE_NODEIDS), required=True)
    linux.add_argument("--repo", type=Path, default=REPO_ROOT)
    linux.add_argument("--build", type=Path, required=True)
    linux.add_argument("--output", type=Path, required=True)
    linux.add_argument("--image", required=True)
    return parser


def main(argv: Sequence[str] | None = None) -> int:
    arguments = _parser().parse_args(argv)
    if arguments.command == "validate":
        packet = json.loads(arguments.packet.read_text(encoding="utf-8"))
        validate_packet(packet)
        print("PART3_GATE_PACKET: VALID")
        return 0
    if arguments.command == "windows":
        return run_windows(
            repo_root=arguments.repo.resolve(),
            output_dir=arguments.output,
            stage=arguments.stage,
            python_executable=arguments.python,
        )
    return run_linux_docker(
        repo_root=arguments.repo.resolve(),
        build_root=arguments.build.resolve(),
        output_dir=arguments.output,
        stage=arguments.stage,
        image=arguments.image,
    )


if __name__ == "__main__":
    raise SystemExit(main())
