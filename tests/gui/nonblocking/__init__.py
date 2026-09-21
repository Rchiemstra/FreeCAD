# SPDX-License-Identifier: LGPL-2.1-or-later
"""Offline contracts for deterministic GUI responsiveness scenarios."""

from .harness import (
    ACTION_KINDS,
    ACTION_SCHEDULE,
    DURATION_MS,
    LATENCY_SAMPLE_COUNT,
    MAX_LATENCY_MS,
    P99_LATENCY_MS,
    ActionRecord,
    ContractError,
    EvidenceRecord,
    ResponsivenessScenario,
    ThresholdResult,
)

__all__ = [
    "ACTION_KINDS",
    "ACTION_SCHEDULE",
    "DURATION_MS",
    "LATENCY_SAMPLE_COUNT",
    "MAX_LATENCY_MS",
    "P99_LATENCY_MS",
    "ActionRecord",
    "ContractError",
    "EvidenceRecord",
    "ResponsivenessScenario",
    "ThresholdResult",
]
