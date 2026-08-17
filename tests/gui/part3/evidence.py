# SPDX-License-Identifier: LGPL-2.1-or-later
"""Evidence schema and writer for Part 3 stress runs (ADR §8)."""

from __future__ import annotations

import json
from datetime import datetime, timezone
from pathlib import Path
from typing import Any

SCHEMA_VERSION = 2


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
        "shutdown": {
            "deadline_seconds": 60,
            "forced": False,
            "stalled_stage": None,
        },
        "artifacts": {"documents": [], "lock_anchors": [], "unexplained": []},
        "checks": [],
        "failed_checks": [],
        "verdict": None,
    }
    return payload


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
    "empty_evidence",
    "finalize_evidence",
    "print_verdict_line",
    "utc_now_iso",
    "write_evidence",
]
