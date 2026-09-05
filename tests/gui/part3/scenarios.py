# SPDX-License-Identifier: LGPL-2.1-or-later
"""Stage A/B/C cycle definitions and ADR §13 coverage checklist for Part 3 stress."""

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

#: Stages enabled by the completed acceptance program. Keeping this set
#: explicit makes any future stage definition fail closed until it is qualified.
EXECUTABLE_STAGES: frozenset[str] = frozenset(
    {STAGE_A.stage, STAGE_B.stage, STAGE_C.stage}
)

#: Every ADR §13 coverage item a stage run must exercise at least once.
COVERAGE_ITEMS: tuple[str, ...] = (
    "camera_rotation",
    "pan",
    "zoom",
    "fit",
    "selection",
    "tree_expand",
    "tree_collapse",
    "active_view_switching",
    "typed_model_mutation",
    "recompute",
    "save",
    "unchanged_save",
    "save_copy",
    "history_undo",
    "history_redo",
    "two_documents",
    "same_property_conflict",
    "independent_property_success",
    "local_pause",
    "local_resume",
)


def resolve_stage(name: str) -> StageDefinition:
    key = name.strip().upper()
    if key not in STAGES:
        supported = ", ".join(sorted(STAGES))
        raise ValueError(f"unsupported stage {name!r}; expected one of {supported}")
    return STAGES[key]


def resolve_executable_stage(name: str) -> StageDefinition:
    """Resolve a stage enabled by the completed acceptance program."""

    definition = resolve_stage(name)
    if definition.stage not in EXECUTABLE_STAGES:
        executable = ", ".join(sorted(EXECUTABLE_STAGES))
        raise ValueError(
            f"stage {definition.stage} is defined but not executable; "
            f"executable stages are {executable}"
        )
    return definition


__all__ = [
    "COVERAGE_ITEMS",
    "EXECUTABLE_STAGES",
    "STAGE_A",
    "STAGE_B",
    "STAGE_C",
    "STAGES",
    "StageDefinition",
    "resolve_executable_stage",
    "resolve_stage",
]
