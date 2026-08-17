# SPDX-License-Identifier: LGPL-2.1-or-later
"""Stage A/B/C cycle definitions for Part 3 stress (definitions only in WP06)."""

from __future__ import annotations

from dataclasses import dataclass


@dataclass(frozen=True, slots=True)
class StageDefinition:
    """One staged stress profile from ADR §13 / plan P3-WP10..WP11."""

    stage: str
    view_mutation_cycles: int
    save_cycles: int


STAGE_A = StageDefinition(stage="A", view_mutation_cycles=10, save_cycles=5)
STAGE_B = StageDefinition(stage="B", view_mutation_cycles=50, save_cycles=20)
STAGE_C = StageDefinition(stage="C", view_mutation_cycles=500, save_cycles=100)

STAGES: dict[str, StageDefinition] = {
    STAGE_A.stage: STAGE_A,
    STAGE_B.stage: STAGE_B,
    STAGE_C.stage: STAGE_C,
}


def resolve_stage(name: str) -> StageDefinition:
    key = name.strip().upper()
    if key not in STAGES:
        supported = ", ".join(sorted(STAGES))
        raise ValueError(f"unsupported stage {name!r}; expected one of {supported}")
    return STAGES[key]


__all__ = [
    "STAGE_A",
    "STAGE_B",
    "STAGE_C",
    "STAGES",
    "StageDefinition",
    "resolve_stage",
]
