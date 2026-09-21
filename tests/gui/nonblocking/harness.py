# SPDX-License-Identifier: LGPL-2.1-or-later
"""Value-only contract for the 30-second GUI responsiveness scenario.

This module deliberately contains no Qt, FreeCAD, document, clock, or thread
code.  A future execution lane can produce the records, while this module
defines the portable artifact and its fail-closed validation rules.
"""

from __future__ import annotations

import json
from math import ceil
from dataclasses import dataclass
from typing import Any, ClassVar


DURATION_MS = 30_000
P99_LATENCY_MS = 50
MAX_LATENCY_MS = 100

ACTION_KINDS = (
    "repaint",
    "resize",
    "navigation",
    "tree_scroll",
    "cached_property",
    "non_model_control",
    "busy_response",
    "cancellation",
    "latency_sample",
)
_ACTION_KIND_SET = frozenset(ACTION_KINDS)
ACTION_SCHEDULE = tuple(
    (kind, (DURATION_MS * index) // (len(ACTION_KINDS) - 1))
    for index, kind in enumerate(ACTION_KINDS)
)
_ACTION_SCHEDULE_BY_SEQUENCE = dict(enumerate(ACTION_SCHEDULE))


class ContractError(ValueError):
    """Raised when a scenario is not a valid deterministic contract."""


def _check_text(name: str, value: object) -> str:
    if not isinstance(value, str) or not value:
        raise ContractError(f"{name} must be a non-empty string")
    return value


def _check_nonnegative_int(name: str, value: object) -> int:
    if type(value) is not int or value < 0:
        raise ContractError(f"{name} must be a non-negative integer")
    return value


@dataclass(frozen=True, slots=True)
class ActionRecord:
    """One requested interaction, represented only by immutable values."""

    sequence: int
    kind: str
    at_ms: int
    value: str = ""

    def __post_init__(self) -> None:
        _check_nonnegative_int("sequence", self.sequence)
        _check_text("kind", self.kind)
        if self.kind not in _ACTION_KIND_SET:
            raise ContractError(f"unsupported action kind: {self.kind!r}")
        if type(self.at_ms) is not int or not 0 <= self.at_ms <= DURATION_MS:
            raise ContractError(f"at_ms must be between 0 and {DURATION_MS}")
        if not isinstance(self.value, str):
            raise ContractError("value must be a string")

    def as_dict(self) -> dict[str, object]:
        return {
            "sequence": self.sequence,
            "kind": self.kind,
            "at_ms": self.at_ms,
            "value": self.value,
        }


@dataclass(frozen=True, slots=True)
class EvidenceRecord:
    """The value-only observation associated with one action."""

    sequence: int
    action_sequence: int
    at_ms: int
    kind: str
    latency_ms: int
    outcome: str
    busy: bool = False
    cancelled: bool = False

    def __post_init__(self) -> None:
        _check_nonnegative_int("sequence", self.sequence)
        _check_nonnegative_int("action_sequence", self.action_sequence)
        if type(self.at_ms) is not int or self.at_ms < 0:
            raise ContractError("at_ms must be a non-negative integer")
        _check_text("kind", self.kind)
        if self.kind not in _ACTION_KIND_SET:
            raise ContractError(f"unsupported evidence kind: {self.kind!r}")
        if type(self.latency_ms) is not int or self.latency_ms < 0:
            raise ContractError("latency_ms must be a non-negative integer")
        _check_text("outcome", self.outcome)
        if type(self.busy) is not bool or type(self.cancelled) is not bool:
            raise ContractError("busy and cancelled must be booleans")

    def as_dict(self) -> dict[str, object]:
        return {
            "sequence": self.sequence,
            "action_sequence": self.action_sequence,
            "at_ms": self.at_ms,
            "kind": self.kind,
            "latency_ms": self.latency_ms,
            "outcome": self.outcome,
            "busy": self.busy,
            "cancelled": self.cancelled,
        }


@dataclass(frozen=True, slots=True)
class ThresholdResult:
    """Deterministic threshold verdict and the values used to reach it."""

    passed: bool
    sample_count: int
    p99_ms: int | None
    maximum_ms: int | None
    p99_limit_ms: int = P99_LATENCY_MS
    maximum_limit_ms: int = MAX_LATENCY_MS

    def as_dict(self) -> dict[str, object]:
        return {
            "passed": self.passed,
            "sample_count": self.sample_count,
            "p99_ms": self.p99_ms,
            "maximum_ms": self.maximum_ms,
            "p99_limit_ms": self.p99_limit_ms,
            "maximum_limit_ms": self.maximum_limit_ms,
        }


@dataclass(frozen=True, slots=True)
class ResponsivenessScenario:
    """A complete, serializable 30-second scenario artifact."""

    actions: tuple[ActionRecord, ...]
    evidence: tuple[EvidenceRecord, ...]
    duration_ms: int = DURATION_MS
    schema: ClassVar[str] = "gui-responsiveness.v1"

    def __post_init__(self) -> None:
        if type(self.duration_ms) is not int or self.duration_ms != DURATION_MS:
            raise ContractError(f"duration_ms must be exactly {DURATION_MS}")
        if not isinstance(self.actions, tuple) or not isinstance(self.evidence, tuple):
            raise ContractError("actions and evidence must be tuples")

    def validate(self) -> None:
        """Validate completeness, ordering, and action/evidence identity."""

        present = {item.kind for item in self.actions}
        missing = [kind for kind in ACTION_KINDS if kind not in present]
        if missing:
            raise ContractError(f"missing required actions: {', '.join(missing)}")
        evidence_kinds = {item.kind for item in self.evidence}
        missing_evidence = [kind for kind in ACTION_KINDS if kind not in evidence_kinds]
        if missing_evidence:
            raise ContractError(
                "missing required evidence: " + ", ".join(missing_evidence)
            )
        if [item.sequence for item in self.actions] != list(range(len(self.actions))):
            raise ContractError("actions must have contiguous deterministic sequence numbers")
        if [item.sequence for item in self.evidence] != list(range(len(self.evidence))):
            raise ContractError("evidence must have contiguous deterministic sequence numbers")
        action_times = [item.at_ms for item in self.actions]
        if action_times[0] != 0 or action_times[-1] != self.duration_ms:
            raise ContractError(
                "actions must cover the complete interval from 0 to "
                f"{self.duration_ms} ms"
            )
        if action_times != sorted(action_times) or len(set(action_times)) != len(action_times):
            raise ContractError("actions must be ordered by strictly increasing at_ms")
        by_sequence = {item.sequence: item for item in self.actions}
        if len(by_sequence) != len(self.actions):
            raise ContractError("action sequence numbers must be unique")
        for action in self.actions:
            if _ACTION_SCHEDULE_BY_SEQUENCE.get(action.sequence) != (
                action.kind,
                action.at_ms,
            ):
                raise ContractError("actions must match the canonical 30-second schedule")
        for observation in self.evidence:
            action = by_sequence.get(observation.action_sequence)
            if action is None:
                raise ContractError(
                    f"evidence references missing action {observation.action_sequence}"
                )
            if observation.kind != action.kind:
                raise ContractError("evidence kind must match its action kind")
            if observation.at_ms < action.at_ms or observation.at_ms > self.duration_ms:
                raise ContractError(f"evidence at_ms must be at most {self.duration_ms}")
            if observation.kind == "busy_response":
                if (
                    not observation.busy
                    or observation.cancelled
                    or observation.outcome != "busy"
                ):
                    raise ContractError("busy_response evidence must be marked busy")
            elif observation.kind == "cancellation":
                if (
                    not observation.cancelled
                    or observation.busy
                    or observation.outcome != "cancelled"
                ):
                    raise ContractError("cancellation evidence must be marked cancelled")
            elif observation.busy or observation.cancelled:
                raise ContractError("busy and cancelled flags require their matching action kind")
        observed_actions = {item.action_sequence for item in self.evidence}
        if observed_actions != set(by_sequence):
            raise ContractError("every action must have evidence")
        if not self.evidence:
            raise ContractError("scenario must contain evidence")

    def evaluate_thresholds(self) -> ThresholdResult:
        """Evaluate latency samples using nearest-rank p99, without sorting input."""

        self.validate()
        values = sorted(item.latency_ms for item in self.evidence)
        rank = max(1, ceil(len(values) * 0.99))
        p99 = values[rank - 1]
        maximum = values[-1]
        return ThresholdResult(
            passed=p99 <= P99_LATENCY_MS and maximum <= MAX_LATENCY_MS,
            sample_count=len(values),
            p99_ms=p99,
            maximum_ms=maximum,
        )

    def as_dict(self) -> dict[str, object]:
        self.validate()
        return {
            "schema": self.schema,
            "duration_ms": self.duration_ms,
            "actions": [item.as_dict() for item in self.actions],
            "evidence": [item.as_dict() for item in self.evidence],
            "thresholds": self.evaluate_thresholds().as_dict(),
        }

    def to_json(self) -> str:
        """Return canonical JSON independent of dictionary insertion order."""

        return json.dumps(self.as_dict(), sort_keys=True, separators=(",", ":"))

    @classmethod
    def from_dict(cls, payload: dict[str, Any]) -> "ResponsivenessScenario":
        if not isinstance(payload, dict) or payload.get("schema") != cls.schema:
            raise ContractError("unsupported or missing scenario schema")
        try:
            actions = tuple(ActionRecord(**item) for item in payload["actions"])
            evidence = tuple(EvidenceRecord(**item) for item in payload["evidence"])
            scenario = cls(
                actions=actions,
                evidence=evidence,
                duration_ms=payload["duration_ms"],
            )
        except (KeyError, TypeError) as error:
            raise ContractError("malformed scenario record") from error
        scenario.validate()
        try:
            thresholds = payload["thresholds"]
        except KeyError as error:
            raise ContractError("malformed scenario thresholds") from error
        if not isinstance(thresholds, dict):
            raise ContractError("malformed scenario thresholds")
        if thresholds != scenario.evaluate_thresholds().as_dict():
            raise ContractError("scenario thresholds do not match its evidence")
        return scenario

    @classmethod
    def from_json(cls, encoded: str) -> "ResponsivenessScenario":
        try:
            payload = json.loads(encoded)
        except (TypeError, json.JSONDecodeError) as error:
            raise ContractError("scenario JSON is invalid") from error
        return cls.from_dict(payload)


__all__ = [
    "ACTION_KINDS",
    "ACTION_SCHEDULE",
    "DURATION_MS",
    "MAX_LATENCY_MS",
    "P99_LATENCY_MS",
    "ActionRecord",
    "ContractError",
    "EvidenceRecord",
    "ResponsivenessScenario",
    "ThresholdResult",
]
