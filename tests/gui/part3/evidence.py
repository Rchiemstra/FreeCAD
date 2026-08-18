# SPDX-License-Identifier: LGPL-2.1-or-later
"""Evidence schema and writer for Part 3 stress runs (ADR §8)."""

from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 2

SHUTDOWN_TIMESTAMP_KEYS = (
    "requested_utc",
    "rpc_admission_closed_utc",
    "worker_shutdown_utc",
    "documents_closed_utc",
    "listener_shutdown_utc",
    "window_closed_utc",
    "process_exit_utc",
)


def utc_now_iso() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat()


def empty_evidence(*, stage: str | None = None) -> dict[str, Any]:
    """Return an empty schema_version 2 envelope."""

    payload: dict[str, Any] = {
        "schema_version": SCHEMA_VERSION,
        "started_utc": utc_now_iso(),
        "finished_utc": None,
        "environment": {
            "isolation_verified": False,
            "auth": {"v2_session": False},
        },
        "stage": stage,
        "cycles": [],
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


__all__ = [
    "SCHEMA_VERSION",
    "SHUTDOWN_TIMESTAMP_KEYS",
    "empty_evidence",
    "empty_shutdown_record",
    "finalize_evidence",
    "print_verdict_line",
    "stamp_shutdown_transition",
    "utc_now_iso",
    "write_evidence",
]
