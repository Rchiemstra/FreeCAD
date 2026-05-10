"""
iteration/policy.py — bounded edit policies.

A Policy declares which FreeCAD parameters an LLM agent is allowed to change
and enforces numeric bounds, step granularity, and units.  It prevents the
agent from making destructive or nonsensical edits.

Usage::

    from iteration.policy import Policy, ParameterRule

    policy = Policy([
        ParameterRule("link1_length", min_val=0.1, max_val=1.0, step=0.05, unit="m"),
        ParameterRule("link2_length", min_val=0.05, max_val=0.8, step=0.05, unit="m"),
        ParameterRule("link1_mass",   min_val=0.1, max_val=5.0,  step=0.1,  unit="kg"),
    ])

    ok, msg = policy.check("link1_length", 0.45)   # -> (True, "")
    ok, msg = policy.check("link1_length", 999)    # -> (False, "…out of range…")
    ok, msg = policy.check("unknown_param", 1.0)   # -> (False, "…not in policy…")

    clamped = policy.clamp("link1_length", 1.5)    # -> 1.0  (hard clamp)
    snapped = policy.snap("link1_length", 0.43)    # -> 0.45 (nearest step)
"""
from __future__ import annotations

import math
from dataclasses import dataclass, field
from typing import Optional


# ---------------------------------------------------------------------------
# ParameterRule
# ---------------------------------------------------------------------------

@dataclass
class ParameterRule:
    """
    Declares the allowed range and step size for one FreeCAD parameter.

    Parameters
    ----------
    name : str
        The FreeCAD spreadsheet or property name (case-sensitive).
    min_val : float
        Minimum allowed value (inclusive).
    max_val : float
        Maximum allowed value (inclusive).
    step : float | None
        Minimum change granularity. If set, proposed values are snapped to
        the nearest multiple of ``step`` measured from ``min_val``.
    unit : str
        Informational unit label (e.g. "m", "kg", "rad"). Not enforced.
    description : str
        Human-readable description of what this parameter controls.
    """
    name:        str
    min_val:     float = 0.0
    max_val:     float = 1.0
    step:        Optional[float] = None
    unit:        str   = ""
    description: str   = ""

    def __post_init__(self):
        if self.min_val > self.max_val:
            raise ValueError(
                f"ParameterRule {self.name!r}: min_val={self.min_val} > max_val={self.max_val}"
            )
        if self.step is not None and self.step <= 0:
            raise ValueError(
                f"ParameterRule {self.name!r}: step must be positive, got {self.step}"
            )

    def check(self, value: float) -> tuple[bool, str]:
        """Return (ok, message). Empty message means ok."""
        if value < self.min_val or value > self.max_val:
            return False, (
                f"{self.name!r} = {value} is out of range "
                f"[{self.min_val}, {self.max_val}] {self.unit}"
            )
        return True, ""

    def clamp(self, value: float) -> float:
        """Clamp value to [min_val, max_val]."""
        return max(self.min_val, min(self.max_val, value))

    def snap(self, value: float) -> float:
        """
        Snap value to the nearest step multiple (from min_val).

        If step is None, returns the clamped value unchanged.
        """
        clamped = self.clamp(value)
        if self.step is None:
            return clamped
        steps = round((clamped - self.min_val) / self.step)
        snapped = self.min_val + steps * self.step
        return self.clamp(round(snapped, 10))   # round to avoid float drift


# ---------------------------------------------------------------------------
# Policy
# ---------------------------------------------------------------------------

@dataclass
class Policy:
    """
    Collection of ParameterRules that together define what an agent may edit.

    Parameters
    ----------
    rules : list[ParameterRule]
    allow_unknown : bool
        If True, parameters not in the policy are allowed (no range check).
        Default False — unknown parameters are rejected.
    """
    rules:         list[ParameterRule] = field(default_factory=list)
    allow_unknown: bool                = False

    def __post_init__(self):
        self._index: dict[str, ParameterRule] = {r.name: r for r in self.rules}

    def check(self, name: str, value: float) -> tuple[bool, str]:
        """
        Validate a proposed parameter change.

        Returns (True, "") on success or (False, reason) on failure.
        """
        rule = self._index.get(name)
        if rule is None:
            if self.allow_unknown:
                return True, ""
            return False, f"Parameter {name!r} is not in the edit policy"
        return rule.check(value)

    def clamp(self, name: str, value: float) -> float:
        """Clamp to rule bounds, or return value unchanged if not in policy."""
        rule = self._index.get(name)
        return rule.clamp(value) if rule else value

    def snap(self, name: str, value: float) -> float:
        """Snap to rule step, or return clamped value if not in policy."""
        rule = self._index.get(name)
        return rule.snap(value) if rule else value

    def check_all(self, params: dict[str, float]) -> tuple[bool, list[str]]:
        """Validate a dict of proposed changes. Returns (all_ok, error_list)."""
        errors: list[str] = []
        for name, value in params.items():
            ok, msg = self.check(name, value)
            if not ok:
                errors.append(msg)
        return len(errors) == 0, errors

    def clamp_all(self, params: dict[str, float]) -> dict[str, float]:
        """Clamp every value in ``params`` to its rule bounds."""
        return {name: self.clamp(name, value) for name, value in params.items()}

    def snap_all(self, params: dict[str, float]) -> dict[str, float]:
        """Snap every value in ``params`` to its rule step."""
        return {name: self.snap(name, value) for name, value in params.items()}

    def __contains__(self, name: str) -> bool:
        return name in self._index

    def parameter_names(self) -> list[str]:
        return list(self._index)


# ---------------------------------------------------------------------------
# Default policy for the demo arm_2dof robot
# ---------------------------------------------------------------------------

DEFAULT_ARM_2DOF_POLICY = Policy(rules=[
    ParameterRule(
        name="link1_length",
        min_val=0.1, max_val=0.8, step=0.05, unit="m",
        description="Length of arm segment 1 (from base to elbow)",
    ),
    ParameterRule(
        name="link2_length",
        min_val=0.1, max_val=0.6, step=0.05, unit="m",
        description="Length of arm segment 2 (from elbow to wrist)",
    ),
    ParameterRule(
        name="link1_mass",
        min_val=0.1, max_val=5.0, step=0.1, unit="kg",
        description="Mass of arm segment 1",
    ),
    ParameterRule(
        name="link2_mass",
        min_val=0.1, max_val=3.0, step=0.1, unit="kg",
        description="Mass of arm segment 2",
    ),
])
