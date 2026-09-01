# SPDX-License-Identifier: LGPL-2.1-or-later
"""Evidence schema and writer for Part 3 stress runs (ADR §8)."""

from __future__ import annotations

import hashlib
import importlib.util
import io
import json
import os
import re
import subprocess
import zipfile
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

from tests.gui.part3.scenarios import COVERAGE_ITEMS, resolve_executable_stage

SCHEMA_VERSION = 2

SHUTDOWN_TIMESTAMP_KEYS = (
    "requested_utc",
    "documents_closed_utc",
    "rpc_admission_closed_utc",
    "worker_shutdown_utc",
    "listener_shutdown_utc",
    "window_closed_utc",
    "process_exit_utc",
)

DOCUMENT_SUFFIX = ".fcstd"
LOCK_ANCHOR_SUFFIX = ".freecad-save.lock"
LEGACY_BACKUP_SUFFIX = ".fcbak"
NUMBERED_BACKUP_PATTERN = re.compile(r"\.fcstd\d+$")

NESTED_MCP_RELPATH = "tools/mcp/freecad-mcp"
SESSION_TTL_ENV = "FREECAD_MCP_SESSION_TTL_SECONDS"

WINDOWS_BINARY_GLOBS = ("FreeCAD*.dll",)
POSIX_BINARY_GLOBS = ("libFreeCAD*.so*",)
_CYCLE_LOCAL_ACTIONS = (
    "set_active_document", "set_active_document", "rotate_camera", "pan_view",
    "zoom_view", "fit_all", "select_object", "expand_tree", "collapse_tree",
    "clear_selection",
)
_CYCLE_REMOTE_ACTIONS = (
    "begin_checked_edit", "commit_checked_property", "commit_checked_property",
    "recompute_document",
)
ALPHA_OBJECT = "StressBox"
ALPHA_PROPERTY = "AlphaValue"
BETA_OBJECT = "SecondBox"
BETA_PROPERTY = "BetaValue"
ALPHA_PROPERTY_KEY = f"ObjectProperty:{ALPHA_OBJECT}:{ALPHA_PROPERTY}"
BETA_PROPERTY_KEY = f"ObjectProperty:{BETA_OBJECT}:{BETA_PROPERTY}"
ALPHA_MODEL_KEY = f"ObjectModel:{ALPHA_OBJECT}"
BETA_MODEL_KEY = f"ObjectModel:{BETA_OBJECT}"
_STAGE_REVISION_KEYS = (
    BETA_MODEL_KEY, ALPHA_MODEL_KEY, BETA_PROPERTY_KEY, ALPHA_PROPERTY_KEY,
)
_STAGE_REVISION_FIELDS = {
    BETA_MODEL_KEY: {"kind": "ObjectModel", "subject": BETA_OBJECT},
    ALPHA_MODEL_KEY: {"kind": "ObjectModel", "subject": ALPHA_OBJECT},
    BETA_PROPERTY_KEY: {
        "kind": "ObjectProperty",
        "subject": BETA_OBJECT,
        "property_name": BETA_PROPERTY,
    },
    ALPHA_PROPERTY_KEY: {
        "kind": "ObjectProperty",
        "subject": ALPHA_OBJECT,
        "property_name": ALPHA_PROPERTY,
    },
}


def _normalized_artifact_path(value: str) -> str:
    """Use the filesystem's case-insensitive FCStd identity convention."""

    return os.path.normcase(os.path.normpath(value)).casefold()


def _save_operations_are_exact(save: dict[str, Any]) -> bool:
    """Bind the mandatory save triplet and any truthful cleaning writes."""

    operations = save.get("actual_save_operations")
    if not isinstance(operations, list) or len(operations) < 3:
        return False
    if not all(isinstance(operation, dict) for operation in operations):
        return False
    if [operation.get("kind") for operation in operations[:3]] != [
        "canonical_written_save",
        "canonical_unchanged_save",
        "save_copy",
    ]:
        return False
    if any(
        operation.get("kind") != "pre_personal_view_clean_save"
        for operation in operations[3:]
    ):
        return False
    written, unchanged, copied = operations[:3]
    save_copy = save.get("save_copy")
    unchanged_save = save.get("unchanged_save")
    return (
        all(item.get("truthful") is True for item in operations)
        and written.get("document") == save.get("document")
        and written.get("canonical_path") == save.get("canonical_path")
        and written.get("sha256_before") == save.get("sha256_before")
        and written.get("sha256_after") == save.get("sha256_after")
        and str(written.get("disposition") or "").lower() == "written"
        and written.get("file_written") is True
        and written.get("durability_verified") is True
        and isinstance(save.get("written_result"), dict)
        and written.get("result") == save["written_result"]
        and save["written_result"].get("saved") is True
        and save["written_result"].get("file_written") is True
        and save["written_result"].get("durability_verified") is True
        and str(save["written_result"].get("save_disposition") or "").lower() == "written"
        and unchanged.get("document") == save.get("document")
        and unchanged.get("canonical_path") == save.get("canonical_path")
        and unchanged.get("sha256_before") == save.get("sha256_after")
        and isinstance(unchanged_save, dict)
        # A truthful no-op save cannot change the canonical bytes.  Bind both
        # independently recorded after-values to the written save endpoint so
        # mutating the operation and its declaration together cannot mask it.
        and unchanged.get("sha256_before") == unchanged.get("sha256_after")
        and unchanged.get("sha256_after") == save.get("sha256_after")
        and unchanged.get("sha256_after") == unchanged_save.get("sha256_after")
        and str(unchanged.get("disposition") or "").lower() == "unchanged"
        and unchanged.get("file_written") is False
        and isinstance(unchanged_save.get("result"), dict)
        and unchanged.get("result") == unchanged_save["result"]
        and unchanged_save["result"].get("unchanged") is True
        and unchanged_save["result"].get("file_written") is False
        and str(unchanged_save["result"].get("save_disposition") or "").lower() == "unchanged"
        and isinstance(save_copy, dict)
        and copied.get("document") == save.get("document")
        and copied.get("canonical_path") == save.get("canonical_path")
        and copied.get("destination") == save_copy.get("destination")
        and copied.get("canonical_sha256_before") == save.get("sha256_after")
        and copied.get("canonical_sha256_after") == save.get("sha256_after")
        and copied.get("sha256_after") == save_copy.get("sha256_after")
        and str(copied.get("disposition") or "").lower() == "copy_written"
        and copied.get("file_written") is True
        and isinstance(save_copy.get("result"), dict)
        and copied.get("result") == save_copy["result"]
        and save_copy["result"].get("saved") is True
        and save_copy["result"].get("file_written") is True
        and str(save_copy["result"].get("save_disposition") or "").lower() == "copy_written"
    )


def _file_change_state_is_clean(state: object) -> bool:
    return (
        isinstance(state, dict)
        and state.get("pending_changes") == []
        and state.get("has_pending_file_changes") is False
    )


def _cleaning_save_operations_are_exact(
    operations: list[dict[str, Any]],
    known_documents: set[str],
    canonical_paths: dict[str, str],
    canonical_endpoints: dict[str, str],
) -> bool:
    """Advance the chronological cleaning-write stream per canonical document."""

    for operation in operations:
        document = operation.get("document")
        result = operation.get("result")
        current = canonical_endpoints.get(document) if isinstance(document, str) else None
        if (
            not isinstance(document, str)
            or document not in known_documents
            or not isinstance(operation.get("canonical_path"), str)
            or _normalized_artifact_path(operation["canonical_path"])
            != canonical_paths.get(document)
            or (
                current is not None
                and operation.get("sha256_before") != current
            )
            or (
                current is None
                and not _looks_like_digest(operation.get("sha256_before"))
            )
            or not _looks_like_digest(operation.get("sha256_after"))
            or operation.get("sha256_after") == operation.get("sha256_before")
            or operation.get("sha256_after") == current
            or str(operation.get("disposition") or "").lower() != "written"
            or operation.get("file_written") is not True
            or operation.get("durability_verified") is not True
            or not isinstance(operation.get("before_file_change_state"), dict)
            or _file_change_state_is_clean(operation.get("before_file_change_state"))
            or not _file_change_state_is_clean(operation.get("after_file_change_state"))
            or not isinstance(result, dict)
            or result.get("saved") is not True
            or result.get("file_written") is not True
            or result.get("durability_verified") is not True
            or str(result.get("save_disposition") or "").lower() != "written"
        ):
            return False
        canonical_endpoints[document] = operation["sha256_after"]
    return True


def _healthy_cycle_readiness(readiness: object, document: object) -> bool:
    if not isinstance(readiness, dict) or readiness.get("success") is not True:
        return False
    documents = readiness.get("documents")
    return (
        isinstance(documents, list)
        and len(documents) == 1
        and isinstance(documents[0], dict)
        and documents[0].get("document") == document
        and documents[0].get("ready") is True
        and documents[0].get("quarantined") is False
        and documents[0].get("collaboration_poisoned") is False
    )


def _revision_observation_key(item: object) -> str | None:
    if not isinstance(item, dict) or type(item.get("revision")) is not int:
        return None
    kind = item.get("kind")
    subject = item.get("subject")
    if kind == "ObjectModel" and set(item) == {"kind", "subject", "revision"}:
        key = f"ObjectModel:{subject}" if isinstance(subject, str) else None
    elif kind == "ObjectProperty" and set(item) == {
        "kind", "subject", "property_name", "revision",
    }:
        property_name = item.get("property_name")
        key = (
            f"ObjectProperty:{subject}:{property_name}"
            if isinstance(subject, str) and isinstance(property_name, str)
            else None
        )
    else:
        return None
    expected = _STAGE_REVISION_FIELDS.get(key)
    return (
        key
        if expected is not None
        and all(item.get(name) == value for name, value in expected.items())
        else None
    )


def _revision_map(observation: object) -> dict[str, int] | None:
    if not isinstance(observation, list) or len(observation) != len(_STAGE_REVISION_KEYS):
        return None
    result: dict[str, int] = {}
    for item in observation:
        key = _revision_observation_key(item)
        if key is None or key in result:
            return None
        result[key] = item["revision"]
    return result if tuple(result) == _STAGE_REVISION_KEYS else None


def stage_revision_vector_is_exact(observation: object) -> bool:
    """Require the producer's ordered, typed canonical stage revision vector."""

    return _revision_map(observation) is not None


def _cycle_mutation_is_exact(mutation: object, remote_actions: object, revisions_before: object, revisions_after: object) -> bool:
    if not isinstance(mutation, dict) or not isinstance(remote_actions, list):
        return False
    operation_id = mutation.get("operation_id")
    revision_before, revision_after, value = (
        mutation.get("revision_before"),
        mutation.get("revision_after"),
        mutation.get("expected_value"),
    )
    if (
        not isinstance(operation_id, str)
        or not operation_id
        or type(revision_before) is not int
        or type(revision_after) is not int
        or revision_after != revision_before + 1
        or type(value) is not int
        or mutation.get("landed_value") != value
    ):
        return False
    if not isinstance(remote_actions, list) or [item.get("method") if isinstance(item, dict) else None for item in remote_actions] != list(_CYCLE_REMOTE_ACTIONS):
        return False
    begin, first, replay, recompute = remote_actions
    first_result, replay_result = first.get("result_envelope"), replay.get("result_envelope")
    before_map, after_map = _revision_map(revisions_before), _revision_map(revisions_after)
    if before_map is None or after_map is None:
        return False
    target_key = BETA_PROPERTY_KEY
    unchanged_keys = (
        ALPHA_PROPERTY_KEY, ALPHA_MODEL_KEY, BETA_MODEL_KEY,
    )
    return (
        isinstance(begin.get("operation_id"), str)
        and bool(begin["operation_id"])
        and begin["operation_id"] not in {operation_id, recompute.get("operation_id")}
        and isinstance(begin.get("result_envelope"), dict)
        and begin["result_envelope"].get("success") is True
        and isinstance(begin["result_envelope"].get("session_id"), str)
        and bool(begin["result_envelope"]["session_id"])
        and first.get("operation_id") == operation_id
        and replay.get("operation_id") == operation_id
        and isinstance(first_result, dict)
        and first_result.get("success") is True
        and first_result.get("committed") is True
        and replay.get("committed_once") is True
        and replay_result == first_result
        and mutation.get("first_result") == first_result
        and mutation.get("replay_result") == replay_result
        and isinstance(recompute.get("operation_id"), str)
        and bool(recompute["operation_id"])
        and recompute["operation_id"] not in {begin["operation_id"], operation_id}
        and recompute.get("result_envelope") == {"success": True}
        and revision_before == before_map[target_key]
        and revision_after == after_map[target_key]
        and after_map[target_key] == before_map[target_key] + 1
        and all(after_map[key] == before_map[key] for key in unchanged_keys)
        and revisions_before == mutation.get("revisions_before")
        and revisions_after == mutation.get("revisions_after")
    )


def _expected_local_action_parameters(
    index: int, document: str, counterpart: str
) -> list[dict[str, Any]]:
    """The deterministic Part 3 personal-action schedule for one cycle."""

    return [
        {"document": counterpart},
        {"document": document},
        {"yaw": 7.0 + index, "pitch": 5.0 + (index % 7), "roll": 0.0},
        {"dx": 0.1 + (index % 5) * 0.05, "dy": 0.02, "dz": 0.0},
        {"direction": "in" if index % 2 == 0 else "out"},
        {"factor": 1.0},
        {"document": document, "object": ALPHA_OBJECT},
        {"document": document, "object": ALPHA_OBJECT},
        {"document": document, "object": ALPHA_OBJECT},
        {},
    ]


def _local_action_result_is_exact(action: dict[str, Any], parameters: dict[str, Any]) -> bool:
    observed = action.get("observed")
    if not isinstance(observed, dict):
        return False
    name = action.get("action")
    if name == "set_active_document":
        return observed.get("active_document") == parameters["document"]
    if name == "rotate_camera":
        return isinstance(observed.get("camera_orientation"), str) and bool(observed["camera_orientation"])
    if name == "pan_view":
        return isinstance(observed.get("view_position"), str) and bool(observed["view_position"])
    if name == "zoom_view":
        return observed == {"direction": parameters["direction"]}
    if name == "fit_all":
        return observed == {"factor": parameters["factor"]}
    if name == "select_object":
        return (
            type(observed.get("selection_count")) is int
            and observed["selection_count"] >= 1
            and observed.get("selected_object") == parameters["object"]
        )
    if name in {"expand_tree", "collapse_tree"}:
        expected_mod = 2 if name == "expand_tree" else 1
        return observed == {**parameters, "mod": expected_mod}
    return name == "clear_selection" and observed == {"selection_count": 0}


def _remote_action_parameters_are_exact(
    cycle: dict[str, Any], remote_actions: list[dict[str, Any]], index: int
) -> bool:
    document = cycle["document"]
    begin, first, replay, recompute = remote_actions
    begin_parameters = begin.get("parameters")
    commit_parameters = first.get("parameters")
    return (
        isinstance(begin_parameters, dict)
        and begin_parameters.get("operation_id") == begin.get("operation_id")
        and isinstance(begin_parameters.get("doc_selector"), dict)
        and begin_parameters["doc_selector"].get("document_name") == document
        and begin_parameters.get("revision_keys") == [
            {"kind": "ObjectProperty", "subject": BETA_OBJECT, "property_name": BETA_PROPERTY}
        ]
        and isinstance(commit_parameters, dict)
        and commit_parameters.get("operation_id") == first.get("operation_id")
        and commit_parameters.get("session_id") == begin.get("result_envelope", {}).get("session_id")
        and isinstance(commit_parameters.get("doc_selector"), dict)
        and commit_parameters["doc_selector"].get("document_name") == document
        and commit_parameters.get("object_name") == BETA_OBJECT
        and commit_parameters.get("property_name") == BETA_PROPERTY
        and commit_parameters.get("value_type") == "integer"
        and commit_parameters.get("value") == str(100 + index)
        and replay.get("parameters") == commit_parameters
        and recompute.get("parameters") == [document]
    )


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def session_ttl_provenance(
    repo_root: Path,
    *,
    environ: dict[str, str] | None = None,
) -> dict[str, Any]:
    """Record the server's effective session-TTL input without exposing secrets."""

    constants_path = (
        Path(repo_root)
        / NESTED_MCP_RELPATH
        / "addon"
        / "FreeCADMCP"
        / "_shared"
        / "protocol"
        / "constants.py"
    )
    spec = importlib.util.spec_from_file_location(
        "_part3_mcp_protocol_constants", constants_path
    )
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot load MCP protocol constants from {constants_path}")
    constants = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(constants)
    default_seconds = float(constants.DEFAULT_SESSION_TTL_SECONDS)
    maximum_seconds = float(constants.MAX_SESSION_TTL_SECONDS)
    constants_bytes = constants_path.read_bytes()

    source_env = os.environ if environ is None else environ
    override_present = SESSION_TTL_ENV in source_env
    override_value = source_env.get(SESSION_TTL_ENV) if override_present else None
    stripped = str(override_value or "").strip()
    effective_seconds = default_seconds
    source = "default"
    if stripped:
        try:
            candidate = float(stripped)
        except ValueError:
            source = "default_invalid_override"
        else:
            if 1.0 <= candidate <= maximum_seconds:
                effective_seconds = candidate
                source = "override"
            else:
                source = "default_out_of_range_override"
    elif override_present:
        source = "default_empty_override"
    return {
        "environment_variable": SESSION_TTL_ENV,
        "override_present": override_present,
        "override_value": override_value,
        "effective_seconds": effective_seconds,
        "default_seconds": default_seconds,
        "source": source,
        "constants_file": {
            "repo_relative_path": constants_path.relative_to(repo_root).as_posix(),
            "sha256": hashlib.sha256(constants_bytes).hexdigest(),
            "size": len(constants_bytes),
            "default_symbol": "DEFAULT_SESSION_TTL_SECONDS",
            "default_value": default_seconds,
            "maximum_symbol": "MAX_SESSION_TTL_SECONDS",
            "maximum_value": maximum_seconds,
        },
    }


def empty_evidence(*, stage: str | None = None) -> dict[str, Any]:
    """Return an empty schema_version 2 envelope.

    ``mode`` names the kind of run that produced the envelope ("stage" or
    "preflight_only"), so a preflight artifact can never be read as a stage
    result. ``isolation_verified`` and ``auth.v2_session`` start false and are
    only raised by an observation.
    """

    payload: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "started_utc": utc_now_iso(),
        "finished_utc": None,
        "environment": {
            "isolation_verified": False,
            "auth": {"v2_session": False},
        },
        "stage": stage,
        "mode": None,
        "cycles": [],
        "out_of_cycle_local_actions": [],
        "personal_action_proofs": [],
        "saves": [],
        "conflicts": {"same_property": {}, "independent_property": {}},
        "pause_resume": {"pause": {}, "refused": {}, "resume": {}, "after": {}},
        "shutdown": empty_shutdown_record(),
        "artifacts": {"documents": [], "lock_anchors": [], "unexplained": []},
        "checks": [],
        "failed_checks": [],
        "verdict": None,
    }
    return payload


def empty_shutdown_record() -> dict[str, Any]:
    """ADR §8 shutdown envelope with null timestamps until stamped."""

    record: dict[str, Any] = {key: None for key in SHUTDOWN_TIMESTAMP_KEYS}
    record["deadline_seconds"] = 60
    record["forced"] = False
    record["stalled_stage"] = None
    return record


def stamp_shutdown_transition(
    shutdown: dict[str, Any],
    key: str,
    *,
    at: str | None = None,
) -> None:
    if key not in SHUTDOWN_TIMESTAMP_KEYS:
        raise ValueError(f"unknown shutdown timestamp key: {key!r}")
    shutdown[key] = at or utc_now_iso()


def write_evidence(path: Path, payload: dict[str, Any]) -> None:
    if payload.get("schema_version") != SCHEMA_VERSION:
        raise ValueError(
            f"evidence schema_version must be {SCHEMA_VERSION}, "
            f"got {payload.get('schema_version')!r}"
        )
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(payload, indent=2, sort_keys=True) + "\n",
        encoding="utf-8",
        newline="\n",
    )


def finalize_evidence(payload: dict[str, Any], *, verdict: str) -> dict[str, Any]:
    finished = dict(payload)
    finished["finished_utc"] = utc_now_iso()
    finished["verdict"] = verdict
    return finished


def print_verdict_line(verdict: str) -> None:
    normalized = verdict.strip().upper()
    if normalized not in {"PASSED", "FAILED"}:
        raise ValueError(f"verdict must be PASSED or FAILED, got {verdict!r}")
    print(f"PART3_RESULT: {normalized}")


def sha256_file(path: Path) -> str:
    """Return the SHA-256 of one file, or an empty string when it is absent."""

    target = Path(path)
    if not target.is_file():
        return ""
    digest = hashlib.sha256()
    with target.open("rb") as stream:
        for chunk in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def file_fingerprint(path: Path) -> dict[str, Any]:
    """ADR §8 binary fingerprint member: mtime, size and SHA-256."""

    target = Path(path)
    info = target.stat()
    return {
        "path": str(target),
        "mtime_ns": int(info.st_mtime_ns),
        "size": int(info.st_size),
        "sha256": sha256_file(target),
    }


def freecad_binary_paths(executable: Path) -> list[Path]:
    """Return the launched executable plus the FreeCAD runtime libraries beside it."""

    exe = Path(executable).resolve()
    binaries: list[Path] = [exe] if exe.is_file() else []
    directory = exe.parent
    if not directory.is_dir():
        return binaries
    globs = WINDOWS_BINARY_GLOBS if os.name == "nt" else POSIX_BINARY_GLOBS
    seen = {exe}
    for pattern in globs:
        for candidate in sorted(directory.glob(pattern)):
            resolved = candidate.resolve()
            if resolved.is_file() and resolved not in seen:
                seen.add(resolved)
                binaries.append(resolved)
    return binaries


def binary_fingerprint(paths: list[Path]) -> dict[str, Any]:
    """Map binary file name to its mtime/size/SHA-256 fingerprint."""

    fingerprints: dict[str, Any] = {}
    for path in paths:
        target = Path(path)
        if target.is_file():
            fingerprints[target.name] = file_fingerprint(target)
    return fingerprints


def _git_output(repo: Path, arguments: list[str]) -> str:
    completed = subprocess.run(
        ["git", "-C", str(repo), *arguments],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        return ""
    return completed.stdout.strip()


def _looks_like_sha(value: str) -> bool:
    return len(value) == 40 and all(char in "0123456789abcdef" for char in value)


def _looks_like_digest(value: object) -> bool:
    return (
        isinstance(value, str)
        and len(value) == 64
        and all(char in "0123456789abcdef" for char in value)
    )


def _timestamp_is_utc(value: object) -> bool:
    return parse_canonical_zero_offset_timestamp(value) is not None


def parse_canonical_zero_offset_timestamp(value: object) -> datetime | None:
    """Parse the second-resolution UTC representation written by the producer.

    Shutdown evidence is causal evidence, not merely a collection of truthy
    strings.  Accepting only this canonical spelling keeps a naive timestamp,
    a non-UTC offset, and an alternate synthesized representation from being
    compared as if it were a producer observation.
    """

    if not isinstance(value, str) or not value:
        return None
    try:
        parsed = datetime.strptime(value, "%Y-%m-%dT%H:%M:%S+00:00")
    except ValueError:
        return None
    return parsed.replace(tzinfo=timezone.utc)


def shutdown_transitions_are_complete_and_ordered(record: object) -> bool:
    """Require every producer shutdown transition in its causal order."""

    if not isinstance(record, dict):
        return False
    transitions = [
        parse_canonical_zero_offset_timestamp(record.get(key))
        for key in SHUTDOWN_TIMESTAMP_KEYS
    ]
    return all(value is not None for value in transitions) and all(
        earlier <= later
        for earlier, later in zip(transitions, transitions[1:])
        if earlier is not None and later is not None
    )


def _fcstd_bytes_are_readable(data: bytes) -> bool:
    """Validate one already-retained FCStd byte snapshot without rereading it."""

    try:
        with zipfile.ZipFile(io.BytesIO(data), "r") as archive:
            return (
                "Document.xml" in {item.filename for item in archive.infolist()}
                and archive.testzip() is None
            )
    except (OSError, zipfile.BadZipFile):
        return False


def _fcstd_artifact_snapshots(
    artifacts: dict[str, Any],
    retained_fcstd_copies: dict[str, dict[str, Any]] | None,
) -> dict[str, dict[str, Any]]:
    """Bind every reported FCStd to one immutable byte snapshot.

    Direct assertions read the producer path once.  Packet assertions receive
    the one host-side snapshot retained before platform cleanup.
    """

    snapshots: dict[str, dict[str, Any]] = {}
    for entry in artifacts["documents"]:
        path_value = entry.get("path") if isinstance(entry, dict) else None
        if not isinstance(path_value, str) or not path_value.lower().endswith(DOCUMENT_SUFFIX):
            continue
        identity = _normalized_artifact_path(path_value)
        if not identity or identity in snapshots:
            raise ValueError("FCStd artifact paths are duplicate or malformed")
        if retained_fcstd_copies is None:
            source = Path(path_value)
            if not source.is_file():
                raise ValueError("reported FCStd artifact is unavailable")
            data = source.read_bytes()
        else:
            retained = retained_fcstd_copies.get(identity)
            if (
                not isinstance(retained, dict)
                or retained.get("source_path") != path_value
                or not isinstance(retained.get("data"), bytes)
            ):
                raise ValueError("reported FCStd has no retained immutable snapshot")
            data = retained["data"]
        digest = hashlib.sha256(data).hexdigest()
        if (
            type(entry.get("size")) is not int
            or entry["size"] != len(data)
            or len(data) <= 0
            or entry.get("sha256") != digest
            or entry.get("readable_archive") is not True
            or not _fcstd_bytes_are_readable(data)
        ):
            raise ValueError("FCStd metadata does not bind immutable readable bytes")
        snapshots[identity] = {
            "path": path_value,
            "size": len(data),
            "sha256": digest,
        }
    if retained_fcstd_copies is not None and set(retained_fcstd_copies) != set(snapshots):
        raise ValueError("retained FCStd inventory does not exactly bind evidence")
    return snapshots


def git_state(
    repo_root: Path,
    *,
    nested_relpath: str = NESTED_MCP_RELPATH,
) -> dict[str, Any]:
    """ADR §8 git block: parent/nested commits, branch and dirty state."""

    parent = Path(repo_root).resolve()
    nested = parent.joinpath(*nested_relpath.split("/"))
    gitlink = ""
    for field in _git_output(parent, ["ls-tree", "HEAD", nested_relpath]).split():
        if _looks_like_sha(field):
            gitlink = field
            break
    tracked = ["status", "--porcelain", "--untracked-files=no"]
    parent_tracked = _git_output(parent, tracked)
    parent_all = _git_output(parent, ["status", "--porcelain"])
    nested_tracked = _git_output(nested, tracked) if nested.is_dir() else ""
    nested_all = _git_output(nested, ["status", "--porcelain"]) if nested.is_dir() else ""
    return {
        "parent_commit": _git_output(parent, ["rev-parse", "HEAD"]),
        "nested_commit": _git_output(nested, ["rev-parse", "HEAD"]) if nested.is_dir() else "",
        "recorded_gitlink": gitlink,
        "branch": _git_output(parent, ["rev-parse", "--abbrev-ref", "HEAD"]),
        "dirty": bool(parent_tracked) or bool(nested_tracked),
        "untracked_present": bool(parent_all) or bool(nested_all),
    }


def new_cycle_record(index: int) -> dict[str, Any]:
    """ADR §8 cycle envelope for one view/mutation cycle."""

    return {
        "index": int(index),
        "local_actions": [],
        "remote_actions": [],
        "revisions_before": [],
        "revisions_after": [],
        "file_change_state_before": {},
        "file_change_state_after": {},
        "readiness": {},
        "coverage": [],
    }


def record_cycle(payload: dict[str, Any], cycle: dict[str, Any]) -> None:
    payload.setdefault("cycles", []).append(cycle)


def record_save(payload: dict[str, Any], save: dict[str, Any]) -> None:
    payload.setdefault("saves", []).append(save)


def record_conflict(payload: dict[str, Any], kind: str, detail: dict[str, Any]) -> None:
    conflicts = payload.setdefault(
        "conflicts", {"same_property": {}, "independent_property": {}}
    )
    if kind not in conflicts:
        raise ValueError(f"unknown conflict kind: {kind!r}")
    conflicts[kind] = detail


def record_pause_resume(payload: dict[str, Any], phase: str, detail: dict[str, Any]) -> None:
    pause_resume = payload.setdefault(
        "pause_resume", {"pause": {}, "refused": {}, "resume": {}, "after": {}}
    )
    if phase not in pause_resume:
        raise ValueError(f"unknown pause/resume phase: {phase!r}")
    pause_resume[phase] = detail


def record_check(
    payload: dict[str, Any],
    name: str,
    passed: bool,
    detail: Any = None,
) -> bool:
    """Append one named check; failures also land in failed_checks."""

    entry = {"name": str(name), "passed": bool(passed), "detail": detail}
    payload.setdefault("checks", []).append(entry)
    if not entry["passed"]:
        payload.setdefault("failed_checks", []).append(entry)
    return entry["passed"]


def classify_artifact(path: Path) -> str:
    """Classify one run artifact; R26/R27 lock anchors are their own class."""

    name = Path(path).name.lower()
    if name.endswith(LOCK_ANCHOR_SUFFIX):
        return "lock_anchors"
    if name.endswith(DOCUMENT_SUFFIX):
        return "documents"
    if name.endswith(LEGACY_BACKUP_SUFFIX):
        return "documents"
    if NUMBERED_BACKUP_PATTERN.search(name):
        return "documents"
    return "unexplained"


def archive_has_document_xml(path: Path) -> bool:
    """True when the FCStd is an ordinarily readable ZIP holding Document.xml."""

    target = Path(path)
    if not target.is_file():
        return False
    try:
        with zipfile.ZipFile(target, "r") as archive:
            if "Document.xml" not in {item.filename for item in archive.infolist()}:
                return False
            return archive.testzip() is None
    except (OSError, zipfile.BadZipFile):
        return False


def scan_artifacts(directories: list[Path]) -> dict[str, Any]:
    """Post-stage artifact scan: documents, R26/R27 lock anchors, unexplained."""

    scan: dict[str, Any] = {"documents": [], "lock_anchors": [], "unexplained": []}
    for directory in directories:
        root = Path(directory)
        if not root.is_dir():
            continue
        for candidate in sorted(root.rglob("*")):
            if not candidate.is_file():
                continue
            bucket = classify_artifact(candidate)
            entry: dict[str, Any] = {
                "path": str(candidate),
                "size": int(candidate.stat().st_size),
            }
            if bucket == "documents" and candidate.name.lower().endswith(DOCUMENT_SUFFIX):
                entry["readable_archive"] = archive_has_document_xml(candidate)
                entry["sha256"] = sha256_file(candidate)
            scan[bucket].append(entry)
    return scan


def verdict_from_checks(payload: dict[str, Any]) -> str:
    """PASSED only when no check failed and the artifact scan is fully explained."""

    if payload.get("failed_checks"):
        return "FAILED"
    artifacts = payload.get("artifacts") or {}
    if artifacts.get("unexplained"):
        return "FAILED"
    return "PASSED"


def _paused_readiness_is_exact(readiness: object, document_name: str) -> bool:
    """Accept the producer's one-document paused-readiness envelope only."""

    if not isinstance(readiness, dict) or readiness.get("success") is not True:
        return False
    documents = readiness.get("documents")
    pause = readiness.get("automation_pause")
    if (
        not isinstance(documents, list)
        or len(documents) != 1
        or not isinstance(documents[0], dict)
        or not isinstance(pause, dict)
    ):
        return False
    document = documents[0]
    return (
        pause.get("paused") is True
        and type(pause.get("active_write_count")) is int
        and pause["active_write_count"] == 0
        and document.get("document") == document_name
        and document.get("automation_paused") is True
        and type(document.get("active_write_count")) is int
        and document["active_write_count"] == 0
        and (
            "automation_paused" not in readiness
            or readiness["automation_paused"] is True
        )
        and (
            "active_write_count" not in readiness
            or (
                type(readiness["active_write_count"]) is int
                and readiness["active_write_count"] == 0
            )
        )
    )


def _paused_read_revisions_are_exact(result: object) -> bool:
    revisions = result.get("revisions") if isinstance(result, dict) else None
    return (
        isinstance(revisions, list)
        and len(revisions) == 1
        and _revision_observation_key(revisions[0]) == BETA_PROPERTY_KEY
    )


def _stage_identity_selector_is_exact(selector: object, document: str) -> bool:
    return (
        isinstance(selector, dict)
        and isinstance(selector.get("document_uid"), str)
        and bool(selector["document_uid"])
        and type(selector.get("document_instance_id")) is int
        and type(selector.get("lifecycle_epoch")) is int
        and selector.get("document_name") == document
    )


def _stage_scenario_operations_are_exact(
    payload: dict[str, Any], stage_documents: tuple[str, str],
    operation_ids: set[str], session_ids: set[str],
) -> bool:
    """Bind named scenarios to the validator-wide operation/session registry."""

    primary, secondary = stage_documents
    cycles = payload.get("cycles")
    conflicts = payload.get("conflicts")
    pause_resume = payload.get("pause_resume")
    history = payload.get("history")
    if not all(isinstance(value, dict) for value in (conflicts, pause_resume, history)) or not isinstance(cycles, list):
        return False

    # Personal-action snapshots come from LocalUserDriver observations before
    # and after the GUI operation.  They are independent of the typed RPC
    # records below, so they are the selector trust root rather than a
    # self-confirming value copied among RPC requests.
    proofs = payload.get("personal_action_proofs")
    def snapshot_selector(document: str) -> dict[str, Any] | None:
        if not isinstance(proofs, list):
            return None
        selectors: list[dict[str, Any]] = []
        for proof in proofs:
            if not isinstance(proof, dict):
                return None
            for phase in ("before", "after"):
                snapshots = proof.get(phase)
                snapshot = snapshots.get(document) if isinstance(snapshots, dict) else None
                if snapshot is None:
                    continue
                selector = snapshot.get("identity_selector") if isinstance(snapshot, dict) else None
                if not _stage_identity_selector_is_exact(selector, document):
                    return None
                selectors.append(selector)
        if not selectors or any(selector != selectors[0] for selector in selectors[1:]):
            return None
        return selectors[0]

    def cycle_selector(document: str, trusted: dict[str, Any]) -> dict[str, Any] | None:
        selectors: list[dict[str, Any]] = []
        for cycle in cycles:
            if not isinstance(cycle, dict) or cycle.get("document") != document:
                continue
            actions = cycle.get("remote_actions")
            if not isinstance(actions, list) or len(actions) < 3:
                return None
            selectors_for_cycle: list[object] = []
            for action in actions[:3]:
                params = action.get("parameters") if isinstance(action, dict) else None
                selectors_for_cycle.append(
                    params.get("doc_selector") if isinstance(params, dict) else None
                )
            if any(selector != trusted for selector in selectors_for_cycle):
                return None
            selectors.append(trusted)
        if not selectors or any(selector != selectors[0] for selector in selectors[1:]):
            return None
        return selectors[0]

    primary_snapshot_selector = snapshot_selector(primary)
    secondary_snapshot_selector = snapshot_selector(secondary)
    if primary_snapshot_selector is None or secondary_snapshot_selector is None:
        return False
    primary_selector = cycle_selector(primary, primary_snapshot_selector)
    secondary_selector = cycle_selector(secondary, secondary_snapshot_selector)
    if primary_selector is None or secondary_selector is None:
        return False
    def rpc(
        operation: object, *, method: str, selector: dict[str, Any], params: dict[str, Any],
        result: object, allow_no_operation_id: bool = False,
    ) -> bool:
        if not isinstance(operation, dict) or operation.get("method") != method:
            return False
        if operation.get("parameters") != params or operation.get("result") != result:
            return False
        if params.get("doc_selector") != selector:
            return False
        operation_id = operation.get("operation_id")
        if allow_no_operation_id:
            return operation_id is None and "operation_id" not in params
        if not isinstance(operation_id, str) or not operation_id or params.get("operation_id") != operation_id or operation_id in operation_ids:
            return False
        operation_ids.add(operation_id)
        return True

    def local(operation: object, *, action: str, document: str, object_name: str, property_name: str, value: int) -> bool:
        expected = {"document": document, "object": object_name, "property": property_name, "value": value, "stage_prepared": True}
        if not isinstance(operation, dict) or operation.get("action") != action or operation.get("parameters") != expected:
            return False
        operation_id = operation.get("operation_id")
        observed = operation.get("observed")
        if not isinstance(operation_id, str) or not operation_id or operation_id in operation_ids or not isinstance(observed, dict):
            return False
        if any(observed.get(key) != expected[key] for key in ("document", "object", "property", "value")):
            return False
        operation_ids.add(operation_id)
        return True

    def begin(operations: dict[str, Any], *, selector: dict[str, Any], keys: list[dict[str, str]]) -> str | None:
        operation = operations.get("begin")
        if not isinstance(operation, dict):
            return None
        operation_id = operation.get("operation_id")
        params = {"doc_selector": selector, "revision_keys": keys, "operation_id": operation_id}
        result = operation.get("result")
        if not rpc(operation, method="begin_checked_edit", selector=selector, params=params, result=result):
            return None
        session_id = result.get("session_id") if isinstance(result, dict) else None
        if result.get("success") is not True or not isinstance(session_id, str) or not session_id or session_id in session_ids:
            return None
        session_ids.add(session_id)
        return session_id

    same = conflicts.get("same_property")
    independent = conflicts.get("independent_property")
    if not isinstance(same, dict) or not isinstance(independent, dict) or same.get("document") != primary or independent.get("document") != secondary:
        return False
    same_ops = same.get("stage_operations")
    independent_ops = independent.get("stage_operations")
    if not isinstance(same_ops, dict) or not isinstance(independent_ops, dict) or same_ops.get("selector") != primary_selector or independent_ops.get("selector") != secondary_selector:
        return False
    same_session = begin(same_ops, selector=primary_selector, keys=[{"kind": "ObjectModel", "subject": "StressBox"}])
    independent_session = begin(independent_ops, selector=secondary_selector, keys=[{"kind": "ObjectProperty", "subject": "SecondBox", "property_name": "BetaValue"}])
    if same_session is None or independent_session is None or not local(same_ops.get("local_edit"), action="local_property_edit", document=primary, object_name="StressBox", property_name="AlphaValue", value=42) or not local(independent_ops.get("local_edit"), action="local_property_edit", document=secondary, object_name="StressBox", property_name="AlphaValue", value=11):
        return False
    same_refusal = same.get("refusal")
    same_refusal_operation = same_ops.get("refused_commit")
    independent_commit_operation = independent_ops.get("commit_operation")
    if not isinstance(same_refusal_operation, dict) or not isinstance(independent_commit_operation, dict):
        return False
    same_params = {"session_id": same_session, "doc_selector": primary_selector, "object_name": "StressBox", "property_name": "AlphaValue", "value_type": "integer", "value": "10", "operation_id": same_refusal_operation.get("operation_id")}
    if not rpc(same_refusal_operation, method="commit_checked_property", selector=primary_selector, params=same_params, result=same_refusal):
        return False
    independent_commit = independent.get("commit")
    independent_params = {"session_id": independent_session, "doc_selector": secondary_selector, "object_name": "SecondBox", "property_name": "BetaValue", "value_type": "integer", "value": "30", "operation_id": independent_commit_operation.get("operation_id")}
    if not rpc(independent_commit_operation, method="commit_checked_property", selector=secondary_selector, params=independent_params, result=independent_commit):
        return False
    replay_operation = independent_ops.get("commit_operation_replay")
    if (
        not isinstance(replay_operation, dict)
        or replay_operation.get("operation_id") != independent_commit_operation.get("operation_id")
        or replay_operation.get("replay_of_operation_id")
        != independent_commit_operation.get("operation_id")
        or replay_operation.get("method") != "commit_checked_property"
        or replay_operation.get("parameters") != independent_params
        or replay_operation.get("result") != independent.get("replay")
        or independent.get("committed_once") is not True
        or independent.get("replay") != independent_commit
    ):
        return False
    if same_ops.get("observed", {}).get("alpha_value") != 42 or same_ops["observed"].get("expected_revisions") != same.get("expected_revisions") or same_ops["observed"].get("current_revisions") != same.get("current_revisions"):
        return False
    if any(independent_ops.get("observed", {}).get(key) != independent.get(key) for key in ("alpha_revision_before", "alpha_revision_after", "beta_revision_before", "beta_revision_after", "alpha_value", "beta_value")):
        return False

    history_ops = history.get("stage_operations")
    if history.get("document") != primary or not isinstance(history_ops, dict) or history_ops.get("selector") != primary_selector:
        return False
    if not local(history_ops.get("local_edit"), action="local_property_edit", document=primary, object_name="StressBox", property_name="AlphaValue", value=77):
        return False
    for method, head_key, result_key, probe_key in (("undo", "undo_head", "undo_result", "undo_probe"), ("redo", "redo_head", "redo_result", "redo_probe")):
        head = history.get(head_key)
        result = history.get(result_key)
        probe = history_ops.get(probe_key)
        operation = history_ops.get(method)
        if not isinstance(head, dict) or not isinstance(probe, dict) or not isinstance(operation, dict):
            return False
        count_key, value_key = ("expected_undo_count", "expected_undo_head") if method == "undo" else ("expected_redo_count", "expected_redo_head")
        probe_params = {"doc_selector": primary_selector, "operation_id": probe.get("operation_id"), count_key: -1, value_key: "part3-history-head-probe"}
        if not rpc(probe, method=method, selector=primary_selector, params=probe_params, result=history.get(f"{method}_head_refusal")):
            return False
        params = {"doc_selector": primary_selector, "operation_id": operation.get("operation_id"), count_key: head.get("count"), value_key: head.get("head")}
        if not rpc(operation, method=method, selector=primary_selector, params=params, result=result):
            return False
    if not isinstance(history_ops.get("observed"), dict) or any(history_ops["observed"].get(key) != history.get(key) for key in ("value_after_edit", "value_after_undo", "value_after_redo")):
        return False

    pause_ops = pause_resume.get("stage_operations")
    if not isinstance(pause_ops, dict) or pause_ops.get("document") != primary or pause_ops.get("selector") != primary_selector:
        return False
    for key, paused in (("pause", True), ("resume", False)):
        operation = pause_ops.get(key)
        if operation != pause_resume.get(key) or not isinstance(operation, dict) or operation.get("action") != f"{key}_writes" or operation.get("parameters") != {} or operation.get("observed") != {"paused": paused}:
            return False
        operation_id = operation.get("operation_id")
        if not isinstance(operation_id, str) or not operation_id or operation_id in operation_ids:
            return False
        operation_ids.add(operation_id)
    refusal = pause_ops.get("refused_write")
    expected_refusal_params = {"doc_name": primary, "obj_name": "SecondBox", "properties": {"Properties": {"BetaValue": 7}}}
    if not isinstance(refusal, dict) or refusal.get("method") != "edit_object" or refusal.get("parameters") != expected_refusal_params or refusal.get("result") != pause_resume.get("refused"):
        return False
    read = pause_ops.get("paused_read")
    read_params = {"doc_selector": primary_selector, "revision_keys": [{"kind": "ObjectProperty", "subject": "SecondBox", "property_name": "BetaValue"}]}
    if not rpc(read, method="get_semantic_revisions", selector=primary_selector, params=read_params, result=pause_ops.get("paused_read_result"), allow_no_operation_id=True):
        return False
    if not _paused_read_revisions_are_exact(pause_ops.get("paused_read_result")) or pause_ops.get("readiness") != pause_resume.get("readiness_while_paused"):
        return False
    pause_session = begin(pause_ops, selector=primary_selector, keys=[{"kind": "ObjectProperty", "subject": "SecondBox", "property_name": "BetaValue"}])
    after = pause_resume.get("after")
    after_commit = pause_ops.get("after_commit")
    if not isinstance(after_commit, dict):
        return False
    after_params = {"session_id": pause_session, "doc_selector": primary_selector, "object_name": "SecondBox", "property_name": "BetaValue", "value_type": "integer", "value": "55", "operation_id": after_commit.get("operation_id")}
    return (
        pause_session is not None
        and isinstance(after, dict)
        and rpc(after_commit, method="commit_checked_property", selector=primary_selector, params=after_params, result=after.get("commit"))
        and pause_ops.get("value_after_resume") == after.get("value")
    )


def _expected_stage_source_hashes(repo_root: Path | None = None) -> dict[str, str]:
    """Return fresh source identities for the checkout under validation.

    A stage validator is also an anti-substitution boundary.  Keeping this in a
    process-global cache made a same-process source swap invisible to later
    packet checks, so deliberately hash the declared sources for each call.
    """

    from tests.gui.part3.stress_coordinator import STAGE_SOURCE_FILES

    source_root = Path(repo_root) if repo_root is not None else Path(__file__).resolve().parents[3]
    return {
        relative: sha256_file(source_root.joinpath(*relative.split("/")))
        for relative in STAGE_SOURCE_FILES
    }


def validate_completed_stage_evidence(
    payload: dict[str, Any],
    *,
    stage: str,
    view_mutation_cycles: int | None = None,
    save_cycles: int | None = None,
    repo_root: Path | None = None,
    retained_binary_copies: dict[str, dict[str, Any]] | None = None,
    retained_fcstd_copies: dict[str, dict[str, Any]] | None = None,
) -> None:
    """Reject a stage result unless every acceptance semantic is present.

    This is deliberately independent of a producer's ``verdict`` marker: it is
    used by the stage pytest assertion and by the retained execution-packet
    validator, so a small handwritten JSON stub cannot be promoted to stage
    evidence.
    """

    if repo_root is None:
        raise ValueError("completed live-stage evidence requires the packet repository root")
    repo_root = Path(repo_root).resolve()
    definition = resolve_executable_stage(stage)
    expected_cycles = (
        definition.view_mutation_cycles
        if view_mutation_cycles is None
        else view_mutation_cycles
    )
    expected_saves = definition.save_cycles if save_cycles is None else save_cycles
    if payload.get("schema_version") != SCHEMA_VERSION:
        raise ValueError("unsupported stage evidence schema")
    if payload.get("stage") != definition.stage or payload.get("mode") != "stage":
        raise ValueError("evidence is not the requested stage")
    if payload.get("verdict") != "PASSED":
        raise ValueError("stage evidence verdict is not PASSED")

    checks = payload.get("checks")
    failed = payload.get("failed_checks")
    if not isinstance(checks, list) or not checks or failed != []:
        raise ValueError("stage checks are absent or report a failure")
    check_names: set[str] = set()
    for check in checks:
        if (
            not isinstance(check, dict)
            or not isinstance(check.get("name"), str)
            or not check["name"]
            or check.get("passed") is not True
            or check["name"] in check_names
        ):
            raise ValueError("stage checks are not complete passing observations")
        check_names.add(check["name"])
    required_checks = {
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
    }
    if not required_checks.issubset(check_names):
        raise ValueError("required stage checks are incomplete")

    cycles = payload.get("cycles")
    saves = payload.get("saves")
    if (
        not isinstance(cycles, list)
        or not isinstance(saves, list)
        or len(cycles) != expected_cycles
        or len(saves) != expected_saves
        or [entry.get("index") if isinstance(entry, dict) else None for entry in cycles]
        != list(range(expected_cycles))
        or [entry.get("index") if isinstance(entry, dict) else None for entry in saves]
        != list(range(expected_saves))
    ):
        raise ValueError("stage cycle/save counts or indexes are not exact")
    cycle_documents = [
        cycle.get("document") if isinstance(cycle, dict) else None
        for cycle in cycles
    ]
    if (
        len(cycle_documents) < 2
        or any(not isinstance(document, str) or not document for document in cycle_documents)
        or len(set(cycle_documents)) != 2
    ):
        raise ValueError("stage document identities are not exactly two distinct values")
    stage_documents = (cycle_documents[0], cycle_documents[1])
    if any(
        document != stage_documents[index % 2]
        for index, document in enumerate(cycle_documents)
    ):
        raise ValueError("stage document identities do not alternate exactly")
    stage_document_identities = frozenset(stage_documents)
    operation_ids: set[str] = set()
    session_ids: set[str] = set()
    for index, cycle in enumerate(cycles):
        if not isinstance(cycle, dict):
            raise ValueError("stage cycle is malformed")
        cycle_checks = cycle.get("checks")
        before_state = cycle.get("file_change_state_before")
        after_state = cycle.get("file_change_state_after_personal_view")
        final_state = cycle.get("file_change_state_after")
        revisions_before = cycle.get("revisions_before")
        revisions_after_personal = cycle.get("revisions_after_personal_view")
        revisions_after = cycle.get("revisions_after")
        readiness = cycle.get("readiness")
        mutation = cycle.get("typed_mutation")
        local_actions = cycle.get("local_actions")
        remote_actions = cycle.get("remote_actions")
        if (
            not isinstance(cycle_checks, dict)
            or cycle_checks.get("personal_view_state_inert") is not True
            or cycle_checks.get("personal_view_state_not_dirtying") is not True
            or cycle_checks.get("typed_mutation_committed_once") is not True
            or cycle_checks.get("camera_changed") is not True
            or not isinstance(local_actions, list)
            or not local_actions
            or not isinstance(remote_actions, list)
            or not remote_actions
            or not isinstance(before_state, dict)
            or before_state.get("pending_changes") != []
            or before_state.get("has_pending_file_changes") is not False
            or not isinstance(after_state, dict)
            or after_state.get("pending_changes") != []
            or after_state.get("has_pending_file_changes") is not False
            or not isinstance(final_state, dict)
            or set(final_state) != {"pending_changes", "has_pending_file_changes"}
            or not isinstance(final_state.get("pending_changes"), list)
            or type(final_state.get("has_pending_file_changes")) is not bool
            or final_state["has_pending_file_changes"] != bool(final_state["pending_changes"])
            or cycle.get("document") != stage_documents[index % 2]
            or not isinstance(revisions_before, list)
            or not revisions_before
            or revisions_after_personal != revisions_before
            or not isinstance(revisions_after, list)
            or not revisions_after
            or not _healthy_cycle_readiness(readiness, cycle["document"])
            or not _cycle_mutation_is_exact(mutation, remote_actions, revisions_before, revisions_after)
            or [action.get("action") if isinstance(action, dict) else None for action in local_actions] != list(_CYCLE_LOCAL_ACTIONS)
            or len({action.get("operation_id") for action in local_actions if isinstance(action, dict)}) != len(_CYCLE_LOCAL_ACTIONS)
        ):
            raise ValueError("stage cycle acceptance proof is incomplete")
        expected_parameters = _expected_local_action_parameters(
            index,
            cycle["document"],
            stage_documents[(index + 1) % 2],
        )
        for action, parameters in zip(local_actions, expected_parameters):
            if (
                not isinstance(action, dict)
                or not isinstance(action.get("operation_id"), str)
                or not action["operation_id"]
                or not isinstance(action.get("action"), str)
                or not action["action"]
                or not isinstance(action.get("ack_utc"), str)
                or not _timestamp_is_utc(action["ack_utc"])
                or action.get("parameters") != parameters
                or not _local_action_result_is_exact(action, parameters)
                or action["operation_id"] in operation_ids
            ):
                raise ValueError("stage local action observation is incomplete")
            operation_ids.add(action["operation_id"])
        for action in remote_actions:
            operation_id = action.get("operation_id") if isinstance(action, dict) else None
            if (
                not isinstance(action, dict)
                or not isinstance(action.get("method"), str)
                or not action["method"]
                or not isinstance(action.get("ack_utc"), str)
                or not _timestamp_is_utc(action["ack_utc"])
                or not isinstance(operation_id, str)
                or not operation_id
                or not isinstance(action.get("result_envelope"), dict)
                or action["result_envelope"].get("success") is not True
            ):
                raise ValueError("stage remote action observation is incomplete")
        if not _remote_action_parameters_are_exact(cycle, remote_actions, index):
            raise ValueError("stage remote action parameters are incomplete")
        begin_id = remote_actions[0]["operation_id"]
        commit_id = remote_actions[1]["operation_id"]
        replay = remote_actions[2]
        recompute_id = remote_actions[3]["operation_id"]
        cycle_session_id = remote_actions[0]["result_envelope"].get("session_id")
        if (
            begin_id in operation_ids
            or commit_id in operation_ids
            or recompute_id in operation_ids
            or len({begin_id, commit_id, recompute_id}) != 3
            or not isinstance(cycle_session_id, str)
            or not cycle_session_id
            or cycle_session_id in session_ids
            # The replay is the sole operation-ID duplicate permitted in the
            # entire validator, and it must be the already-validated exact
            # replay of this cycle's first commit rather than a new identity.
            or replay.get("operation_id") != commit_id
            or replay.get("method") != remote_actions[1].get("method")
            or replay.get("parameters") != remote_actions[1].get("parameters")
            or replay.get("result_envelope") != remote_actions[1].get("result_envelope")
            or replay.get("committed_once") is not True
        ):
            raise ValueError("stage operation identities are not globally unique")
        operation_ids.update((begin_id, commit_id, recompute_id))
        session_ids.add(cycle_session_id)

    environment = payload.get("environment")
    if not isinstance(environment, dict):
        raise ValueError("stage environment evidence is absent")
    fingerprints = environment.get("binary_fingerprint")
    if not isinstance(fingerprints, dict) or not fingerprints:
        raise ValueError("stage binary fingerprints are absent")
    for name, fingerprint in fingerprints.items():
        if (
            not isinstance(name, str)
            or not isinstance(fingerprint, dict)
            or not _looks_like_digest(fingerprint.get("sha256"))
            or type(fingerprint.get("size")) is not int
            or fingerprint["size"] <= 0
            or type(fingerprint.get("mtime_ns")) is not int
            or fingerprint["mtime_ns"] <= 0
        ):
            raise ValueError("stage binary fingerprint is malformed")
        binary_path = fingerprint.get("path")
        if not isinstance(binary_path, str) or not binary_path:
            raise ValueError("stage binary fingerprint path is absent")
        target = Path(binary_path)
        if not target.is_file():
            copy = (retained_binary_copies or {}).get(name)
            if (
                not isinstance(copy, dict)
                or copy.get("container_path") != binary_path
                or not isinstance(copy.get("path"), str)
            ):
                raise ValueError("stage binary fingerprint path is unavailable")
            target = Path(copy["path"])
        try:
            info = target.stat()
        except OSError as exc:
            raise ValueError("stage binary fingerprint path is unavailable") from exc
        if (
            sha256_file(target) != fingerprint["sha256"]
            or info.st_size != fingerprint["size"]
            or info.st_mtime_ns != fingerprint["mtime_ns"]
        ):
            raise ValueError("stage binary fingerprint does not bind path bytes")
    git = environment.get("git")
    if (
        not isinstance(git, dict)
        or not isinstance(git.get("parent_commit"), str)
        or not _looks_like_sha(git["parent_commit"])
        or not isinstance(git.get("nested_commit"), str)
        or not _looks_like_sha(git["nested_commit"])
        or not isinstance(git.get("recorded_gitlink"), str)
        or not _looks_like_sha(git["recorded_gitlink"])
        or git["recorded_gitlink"] != git["nested_commit"]
        or not isinstance(git.get("branch"), str)
        or not git["branch"]
        or environment.get("isolation_verified") is not True
        or not environment.get("reported_user_app_data")
    ):
        raise ValueError("stage source or isolation provenance is incomplete")
    observed_git = git_state(repo_root)
    if any(git.get(key) != observed_git.get(key) for key in ("parent_commit", "nested_commit", "recorded_gitlink", "branch")):
        raise ValueError("stage git identity does not bind the packet repository")
    provenance = environment.get("build_provenance")
    # The coordinator constructs the authoritative source list; import lazily
    # because that module imports this evidence module while it initializes.
    expected_source_hashes = _expected_stage_source_hashes(repo_root)

    if (
        not isinstance(provenance, dict)
        or not isinstance(provenance.get("head_commit"), str)
        or len(provenance["head_commit"]) != 40
        or not provenance.get("head_committed_utc")
        or type(provenance.get("history_depth")) is not int
        or provenance["history_depth"] <= 0
        or not isinstance(provenance.get("binaries"), dict)
        or not provenance["binaries"]
        or not isinstance(provenance.get("binaries_predating_head"), list)
        or bool(provenance["binaries_predating_head"])
        or provenance.get("binary_commit_binding_enforced") is not False
        or not isinstance(provenance.get("provenance_caveat"), str)
        or not provenance["provenance_caveat"]
        or not isinstance(provenance.get("stage_sources"), dict)
        or provenance["stage_sources"] != expected_source_hashes
        or not all(
            isinstance(entry, dict)
            and bool(entry.get("mtime_utc"))
            and isinstance(entry.get("commits_not_in_binary"), list)
            and all(
                isinstance(commit, str) and _looks_like_sha(commit)
                for commit in entry["commits_not_in_binary"]
            )
            and _timestamp_is_utc(entry.get("mtime_utc"))
            and isinstance(entry.get("predates_head"), bool)
            and entry["predates_head"] == bool(entry["commits_not_in_binary"])
            for entry in provenance["binaries"].values()
        )
        or provenance["head_commit"] != git["parent_commit"]
        or set(provenance["binaries"]) != set(fingerprints)
        or provenance["binaries_predating_head"]
        != sorted(
            name
            for name, entry in provenance["binaries"].items()
            if entry["predates_head"] is True
        )
    ):
        raise ValueError("stage build provenance is incomplete")
    from tests.gui.part3.stress_coordinator import (
        _binary_relevant_history,
        _commit_history,
        _epoch_to_utc_iso,
    )

    actual_history = _commit_history(repo_root)
    if not actual_history:
        raise ValueError("packet repository history is unavailable")
    actual_head, actual_head_epoch = actual_history[0]
    binary_history = _binary_relevant_history(repo_root, actual_history)
    if (
        provenance["head_commit"] != actual_head
        or provenance["head_committed_utc"] != _epoch_to_utc_iso(actual_head_epoch)
        or provenance["history_depth"] != len(actual_history)
    ):
        raise ValueError("stage provenance does not bind fresh HEAD history")
    for name, fingerprint in fingerprints.items():
        entry = provenance["binaries"][name]
        binary_path = Path(fingerprint["path"])
        if not binary_path.is_file():
            copy = (retained_binary_copies or {}).get(name)
            binary_path = Path(str(copy.get("path") if isinstance(copy, dict) else ""))
        written = binary_path.stat().st_mtime
        missing = [commit for commit, epoch in binary_history if epoch > written]
        if (
            entry["mtime_utc"] != _epoch_to_utc_iso(written)
            or entry["commits_not_in_binary"] != missing
            or entry["predates_head"] != bool(missing)
        ):
            raise ValueError("binary provenance does not bind binary time and HEAD history")
    auth = environment.get("auth")
    if (
        not isinstance(auth, dict)
        or auth.get("v2_session") is not True
        or auth.get("session_token_present") is not True
        or type(auth.get("session_token_length")) is not int
        or auth["session_token_length"] <= 0
        or not auth.get("mcp_instance_id")
        or not auth.get("profile_instance_id")
        or auth.get("protocol_version") is None
    ):
        raise ValueError("stage session authorization evidence is incomplete")
    actor = environment.get("remote_actor")
    if (
        not isinstance(actor, dict)
        or actor.get("mode") != "in_process_typed_session"
        or actor.get("child_token_absence_proved") is not True
        or actor.get("adr_deviation") != "section 1.1"
        or actor.get("holds_rpc_session_in_coordinator_process") is not True
    ):
        raise ValueError("stage remote actor evidence is incomplete")
    ttl = environment.get("session_ttl")
    if (
        not isinstance(ttl, dict)
        or ttl.get("environment_variable") != SESSION_TTL_ENV
        or ttl.get("override_present") is not False
        or ttl.get("override_value") is not None
        or ttl.get("effective_seconds") != ttl.get("default_seconds")
        or ttl.get("source") != "default"
    ):
        raise ValueError("stage session TTL evidence is incomplete")

    coverage = payload.get("coverage")
    required = set(COVERAGE_ITEMS)
    if (
        not isinstance(coverage, dict)
        or set(coverage.get("required") or []) != required
        or set(coverage.get("observed") or []) != required
        or coverage.get("missing") != []
    ):
        raise ValueError("stage coverage is incomplete")
    artifacts = payload.get("artifacts")
    if (
        not isinstance(artifacts, dict)
        or artifacts.get("unexplained") != []
        or not isinstance(artifacts.get("documents"), list)
        or not artifacts["documents"]
        or not isinstance(artifacts.get("lock_anchors"), list)
        or not all(isinstance(entry, dict) for entry in artifacts["documents"])
        or not all(isinstance(entry, dict) for entry in artifacts["lock_anchors"])
        or not all(
            isinstance(entry.get("path"), str)
            and type(entry.get("size")) is int
            and entry["size"] >= 0
            and classify_artifact(Path(entry["path"])) == "documents"
            and (
                not str(entry["path"]).lower().endswith(DOCUMENT_SUFFIX)
                or (
                    entry.get("readable_archive") is True
                    and _looks_like_digest(entry.get("sha256"))
                )
            )
            and (
                str(entry["path"]).lower().endswith(DOCUMENT_SUFFIX)
                or "readable_archive" not in entry
            )
            for entry in artifacts["documents"]
        )
        or not all(
            isinstance(entry, dict)
            and isinstance(entry.get("path"), str)
            and type(entry.get("size")) is int
            and entry["size"] >= 0
            and classify_artifact(Path(entry["path"])) == "lock_anchors"
            for entry in artifacts["lock_anchors"]
        )
    ):
        raise ValueError("stage artifacts are not clean and complete")
    fcstd_artifacts = _fcstd_artifact_snapshots(artifacts, retained_fcstd_copies)

    proofs = payload.get("personal_action_proofs")
    if not isinstance(proofs, list) or not proofs:
        raise ValueError("personal-action proofs are absent")
    proof_indexes = [proof.get("index") if isinstance(proof, dict) else None for proof in proofs]
    if proof_indexes != list(range(len(proofs))):
        raise ValueError("personal-action proof indexes are not exact")
    # Delayed import avoids the coordinator's module-level import of this
    # evidence module while still making the packet gate use the producer's
    # single exact personal-state predicate.
    from tests.gui.part3.stress_coordinator import _personal_action_proof_is_exact

    for proof in proofs:
        if not isinstance(proof, dict):
            raise ValueError("personal-action proof is malformed")
        documents = proof.get("documents")
        before = proof.get("before")
        after = proof.get("after")
        if (
            not isinstance(proof.get("action"), str)
            or not proof["action"]
            or not isinstance(documents, list)
            or not documents
            or len(documents) != len(set(documents))
            or any(
                not isinstance(document, str)
                or document not in stage_document_identities
                for document in documents
            )
            or not isinstance(before, dict)
            or not isinstance(after, dict)
            or set(before) != set(documents)
            or set(after) != set(documents)
            or proof.get("clean_before") is not True
            or proof.get("clean_after") is not True
            or proof.get("semantic_revisions_unchanged") is not True
            or proof.get("passed") is not True
            or not _personal_action_proof_is_exact(proof)
        ):
            raise ValueError("personal-action proof is not exact")

    # A personal-state proof has credit only for the exact local operation that
    # produced it.  A bare proof, a duplicate index, or a proof attached to a
    # different action is not evidence that all classified actions were inert.
    from tests.gui.part3.stress_coordinator import PERSONAL_STATE_ACTIONS

    proof_by_index = {proof["index"]: proof for proof in proofs}
    out_of_cycle_actions = payload.get("out_of_cycle_local_actions")
    if not isinstance(out_of_cycle_actions, list) or not all(
        isinstance(action, dict) for action in out_of_cycle_actions
    ):
        raise ValueError("out-of-cycle local-action stream is malformed")
    if [action.get("out_of_cycle_index") for action in out_of_cycle_actions] != list(
        range(len(out_of_cycle_actions))
    ):
        raise ValueError("out-of-cycle local-action ordering is malformed")
    referenced_proofs: list[int] = []
    cycle_local_actions = [
        action for cycle in cycles for action in cycle["local_actions"]
    ]
    for action in cycle_local_actions:
        action_id = action.get("operation_id")
        if (
            not isinstance(action_id, str)
            or not action_id
            or action_id not in operation_ids
        ):
            raise ValueError("local action stream has duplicate or invalid identities")
    for action, is_out_of_cycle in [
        *((action, False) for action in cycle_local_actions),
        *((action, True) for action in out_of_cycle_actions),
    ]:
        action_id = action.get("operation_id")
        if not isinstance(action_id, str) or not action_id:
            raise ValueError("local action stream has duplicate or invalid identities")
        if is_out_of_cycle:
            if action_id in operation_ids:
                raise ValueError("local action stream reuses a global operation identity")
            operation_ids.add(action_id)
        personal = action["action"] in PERSONAL_STATE_ACTIONS
        proof_index = action.get("personal_action_proof_index")
        if personal:
            if type(proof_index) is not int or proof_index not in proof_by_index:
                raise ValueError("personal local action has no exact proof")
            proof = proof_by_index[proof_index]
            if (
                proof.get("action") != action["action"]
                or proof.get("operation_id") != action["operation_id"]
            ):
                raise ValueError("personal action proof is not operation-bound")
            referenced_proofs.append(proof_index)
        elif proof_index is not None:
            raise ValueError("non-personal local action carries a personal proof")
    if sorted(referenced_proofs) != list(range(len(proofs))):
        raise ValueError("personal-action proofs are missing, duplicate, or orphaned")

    known_documents = set(stage_document_identities)
    canonical_paths: dict[str, str] = {}
    canonical_root: Path | None = None
    for save in saves:
        if not isinstance(save, dict):
            raise ValueError("stage save is malformed")
        document = save.get("document")
        canonical_path = save.get("canonical_path")
        if (
            not isinstance(document, str)
            or document not in known_documents
            or not isinstance(canonical_path, str)
            or not canonical_path
        ):
            raise ValueError("stage save does not bind a known canonical document")
        root = Path(canonical_path).parent
        if canonical_root is None:
            canonical_root = root
        elif _normalized_artifact_path(str(root)) != _normalized_artifact_path(
            str(canonical_root)
        ):
            raise ValueError("stage canonical documents do not share one root")
        identity = _normalized_artifact_path(canonical_path)
        existing = canonical_paths.get(document)
        if existing is not None and existing != identity:
            raise ValueError("stage document has conflicting canonical paths")
        canonical_paths[document] = identity
    if canonical_root is None:
        raise ValueError("stage evidence has no canonical document root")
    for document in known_documents:
        canonical_paths.setdefault(
            document,
            _normalized_artifact_path(str(canonical_root / f"{document}.FCStd")),
        )

    canonical_endpoints: dict[str, str] = {}
    for save in saves:
        document = save.get("document") if isinstance(save, dict) else None
        if not isinstance(document, str) or not document:
            raise ValueError("stage save does not bind a document")
        previous_endpoint = canonical_endpoints.get(document)
        if (
            not isinstance(save, dict)
            or save.get("truthful") is not True
            or str(save.get("disposition") or "").lower() != "written"
            or save.get("file_written") is not True
            or save.get("durability_verified") is not True
            or not _looks_like_digest(save.get("sha256_before"))
            or not _looks_like_digest(save.get("sha256_after"))
            or save["sha256_before"] == save["sha256_after"]
            or not _save_operations_are_exact(save)
            or not isinstance(save.get("unchanged_save"), dict)
            or save["unchanged_save"].get("file_written") is not False
            or str(save["unchanged_save"].get("disposition") or "").lower()
            != "unchanged"
            or not isinstance(save.get("save_copy"), dict)
            or save["save_copy"].get("readable_archive") is not True
            or save["save_copy"].get("canonical_unchanged") is not True
            or not _looks_like_digest(save.get("canonical_artifact_sha256"))
            or not _looks_like_digest(save["save_copy"].get("sha256_after"))
            or not _looks_like_digest(save["save_copy"].get("artifact_sha256"))
            or save["save_copy"]["sha256_after"] != save["sha256_after"]
            or (
                previous_endpoint is not None
                and save["sha256_before"] != previous_endpoint
            )
        ):
            raise ValueError("stage save proof is not truthful")
        # The record's canonical save belongs to its owning document.  Later
        # cleaning operations are a chronological cross-document stream and
        # must advance their own document endpoints below.
        endpoint = save.get("sha256_after")
        if not isinstance(endpoint, str):
            raise ValueError("stage save has no canonical endpoint")
        canonical_endpoints[document] = endpoint
        operations = save["actual_save_operations"]
        if not _cleaning_save_operations_are_exact(
            operations[3:],
            known_documents,
            canonical_paths,
            canonical_endpoints,
        ):
            raise ValueError("stage cleaning save stream is not truthful")

    ordinary_documents = fcstd_artifacts
    seen_copy_paths: set[str] = set()
    for save in saves:
        canonical_path = save.get("canonical_path")
        save_copy = save.get("save_copy")
        copy_path = save_copy.get("destination") if isinstance(save_copy, dict) else None
        if (
            not isinstance(canonical_path, str)
            or not canonical_path
            or not isinstance(copy_path, str)
            or not copy_path
            or _normalized_artifact_path(canonical_path) == _normalized_artifact_path(copy_path)
            or _normalized_artifact_path(canonical_path) not in ordinary_documents
            or _normalized_artifact_path(copy_path) not in ordinary_documents
            or _normalized_artifact_path(copy_path) in seen_copy_paths
            or ordinary_documents[_normalized_artifact_path(canonical_path)].get("sha256")
            != save.get("canonical_artifact_sha256")
            or ordinary_documents[_normalized_artifact_path(copy_path)].get("sha256")
            != save_copy.get("artifact_sha256")
            or save_copy.get("artifact_sha256") != save_copy.get("sha256_after")
        ):
            raise ValueError("post-shutdown artifacts do not retain canonical/save-copy files")
        seen_copy_paths.add(_normalized_artifact_path(copy_path))
    for document, endpoint in canonical_endpoints.items():
        canonical_path = canonical_paths[document]
        artifact = ordinary_documents.get(canonical_path)
        if artifact is None or artifact.get("sha256") != endpoint:
            raise ValueError("final canonical document endpoint lacks a bound artifact")

    conflicts = payload.get("conflicts")
    same = conflicts.get("same_property") if isinstance(conflicts, dict) else None
    independent = conflicts.get("independent_property") if isinstance(conflicts, dict) else None
    same_refusal = same.get("refusal") if isinstance(same, dict) else None
    same_readiness = same.get("readiness") if isinstance(same, dict) else None
    changed_keys = same.get("changed_semantic_keys") if isinstance(same, dict) else None
    expected_revisions = same.get("expected_revisions") if isinstance(same, dict) else None
    current_revisions = same.get("current_revisions") if isinstance(same, dict) else None
    independent_commit = independent.get("commit") if isinstance(independent, dict) else None
    if (
        not isinstance(conflicts, dict)
        or not isinstance(same, dict)
        or same.get("targeted") is not True
        or same.get("write_lane_healthy") is not True
        or not isinstance(same.get("document"), str)
        or not same["document"]
        or not isinstance(same_refusal, dict)
        or same_refusal.get("success") is not False
        or same_refusal.get("error_code") != "DOCUMENT_CONFLICT"
        or not isinstance(same_refusal.get("data"), dict)
        or same_refusal["data"].get("changed_semantic_keys") != changed_keys
        or same_refusal["data"].get("expected_revisions") != expected_revisions
        or same_refusal["data"].get("current_revisions") != current_revisions
        or not isinstance(changed_keys, list)
        or not changed_keys
        or not set(changed_keys).issubset({ALPHA_PROPERTY_KEY, ALPHA_MODEL_KEY})
        or not isinstance(expected_revisions, dict)
        or not isinstance(current_revisions, dict)
        or set(expected_revisions) != set(changed_keys)
        or set(current_revisions) != set(changed_keys)
        or not all(type(expected_revisions[key]) is int and type(current_revisions[key]) is int and current_revisions[key] != expected_revisions[key] for key in changed_keys)
        or not all(current_revisions[key] == expected_revisions[key] + 1 for key in changed_keys)
        or not isinstance(same_readiness, dict)
        or same_readiness.get("success") is not True
        or not isinstance(same_readiness.get("documents"), list)
        or len(same_readiness["documents"]) != 1
        or not isinstance(same_readiness["documents"][0], dict)
        or same_readiness["documents"][0].get("document") != same["document"]
        or same_readiness["documents"][0].get("ready") is not True
        or same_readiness["documents"][0].get("quarantined") is not False
        or same_readiness["documents"][0].get("collaboration_poisoned") is not False
        or not isinstance(independent, dict)
        or independent.get("both_landed") is not True
        or independent.get("committed_once") is not True
        or not isinstance(independent.get("document"), str)
        or not independent["document"]
        or not isinstance(independent_commit, dict)
        or independent_commit.get("success") is not True
        or independent_commit.get("committed") is not True
        or type(independent.get("alpha_revision_before")) is not int
        or type(independent.get("alpha_revision_after")) is not int
        or type(independent.get("beta_revision_before")) is not int
        or type(independent.get("beta_revision_after")) is not int
        or independent["alpha_revision_after"] != independent["alpha_revision_before"] + 1
        or independent["beta_revision_after"] != independent["beta_revision_before"] + 1
        or independent.get("alpha_value") != 11
        or independent.get("beta_value") != 30
        or not isinstance(independent.get("replay"), dict)
        or independent["replay"] != independent_commit
    ):
        raise ValueError("stage conflict evidence is incomplete")
    pause_resume = payload.get("pause_resume")
    if (
        not isinstance(pause_resume, dict)
        or not isinstance(pause_resume.get("pause"), dict)
        or not isinstance(pause_resume["pause"].get("observed"), dict)
        or pause_resume["pause"]["observed"].get("paused") is not True
        or not isinstance(pause_resume.get("resume"), dict)
        or not isinstance(pause_resume["resume"].get("observed"), dict)
        or pause_resume["resume"]["observed"].get("paused") is not False
    ):
        raise ValueError("stage pause/resume evidence is incomplete")
    history = payload.get("history")
    undo = history.get("undo_head") if isinstance(history, dict) else None
    redo = history.get("redo_head") if isinstance(history, dict) else None
    undo_refusal = history.get("undo_head_refusal") if isinstance(history, dict) else None
    redo_refusal = history.get("redo_head_refusal") if isinstance(history, dict) else None
    if (
        not isinstance(undo, dict)
        or not isinstance(redo, dict)
        or not isinstance(undo_refusal, dict)
        or not isinstance(redo_refusal, dict)
        or type(undo.get("count")) is not int
        or undo["count"] <= 0
        or not undo.get("head")
        or type(redo.get("count")) is not int
        or redo["count"] <= 0
        or not redo.get("head")
        or undo_refusal.get("success") is not False
        or undo_refusal.get("error_code") != "HISTORY_HEAD_REJECTED"
        or type(undo_refusal.get("current_undo_count")) is not int
        or undo_refusal["current_undo_count"] != undo["count"]
        or str(undo_refusal.get("current_undo_head")) != str(undo.get("head"))
        or redo_refusal.get("success") is not False
        or redo_refusal.get("error_code") != "HISTORY_HEAD_REJECTED"
        or type(redo_refusal.get("current_redo_count")) is not int
        or redo_refusal["current_redo_count"] != redo.get("count")
        or str(redo_refusal.get("current_redo_head")) != str(redo.get("head"))
        or not isinstance(history.get("undo_result"), dict)
        or history["undo_result"].get("success") is not True
        or not isinstance(history.get("redo_result"), dict)
        or history["redo_result"].get("success") is not True
        or "value_after_edit" not in history
        or "value_after_undo" not in history
        or "value_after_redo" not in history
        or history["value_after_undo"] == history["value_after_edit"]
        or history["value_after_redo"] != history["value_after_edit"]
        or history.get("mismatched_head_refused") is not True
    ):
        raise ValueError("stage history-refusal evidence is incomplete")

    refusal = pause_resume.get("refused") if isinstance(pause_resume, dict) else None
    readiness = (
        pause_resume.get("readiness_while_paused")
        if isinstance(pause_resume, dict)
        else None
    )
    after_resume = pause_resume.get("after") if isinstance(pause_resume, dict) else None
    refusal_text = (
        f"{refusal.get('error') or ''} {refusal.get('error_code') or ''} "
        f"{refusal.get('data') or ''}"
        if isinstance(refusal, dict)
        else ""
    )
    if (
        not isinstance(refusal, dict)
        or refusal.get("success") is not False
        or (
            "AUTOMATION_PAUSED" not in refusal_text
            and "paused new MCP writes" not in refusal_text
        )
        or not _paused_readiness_is_exact(readiness, stage_documents[0])
        or not isinstance(after_resume, dict)
        or not isinstance(after_resume.get("commit"), dict)
        or after_resume["commit"].get("success") is not True
        or after_resume.get("value") != 55
    ):
        raise ValueError("stage pause/resume outcome evidence is incomplete")
    if not _stage_scenario_operations_are_exact(
        payload, stage_documents, operation_ids, session_ids
    ):
        raise ValueError("stage scenario evidence is not causally bound to its operations")

    shutdown = payload.get("shutdown")
    if (
        not isinstance(shutdown, dict)
        or shutdown.get("forced") is not False
        or shutdown.get("stalled_stage") is not None
        or shutdown.get("failed_step") is not None
        or shutdown.get("rpc_error") is not None
        or shutdown.get("deadline_seconds") != 60
        or not shutdown_transitions_are_complete_and_ordered(shutdown)
    ):
        raise ValueError("stage shutdown did not complete gracefully")


__all__ = [
    "ALPHA_MODEL_KEY",
    "ALPHA_OBJECT",
    "ALPHA_PROPERTY",
    "ALPHA_PROPERTY_KEY",
    "BETA_MODEL_KEY",
    "BETA_OBJECT",
    "BETA_PROPERTY",
    "BETA_PROPERTY_KEY",
    "DOCUMENT_SUFFIX",
    "LEGACY_BACKUP_SUFFIX",
    "LOCK_ANCHOR_SUFFIX",
    "NESTED_MCP_RELPATH",
    "SCHEMA_VERSION",
    "SESSION_TTL_ENV",
    "SHUTDOWN_TIMESTAMP_KEYS",
    "archive_has_document_xml",
    "binary_fingerprint",
    "classify_artifact",
    "empty_evidence",
    "empty_shutdown_record",
    "file_fingerprint",
    "finalize_evidence",
    "freecad_binary_paths",
    "git_state",
    "new_cycle_record",
    "parse_canonical_zero_offset_timestamp",
    "print_verdict_line",
    "record_check",
    "record_conflict",
    "record_cycle",
    "record_pause_resume",
    "record_save",
    "scan_artifacts",
    "session_ttl_provenance",
    "sha256_file",
    "shutdown_transitions_are_complete_and_ordered",
    "stage_revision_vector_is_exact",
    "stamp_shutdown_transition",
    "utc_now_iso",
    "verdict_from_checks",
    "write_evidence",
]
