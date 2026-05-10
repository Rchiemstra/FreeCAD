"""
iteration/sweep.py — linspace / arange helpers + SweepRunner.

Usage::

    from iteration.sweep import SweepRunner, linspace, arange
    from iteration.policy import DEFAULT_ARM_2DOF_POLICY

    runner = SweepRunner(
        scenario_name="reach_top_shelf",
        policy=DEFAULT_ARM_2DOF_POLICY,
        bridge_module=MockBridge(),   # optional; omit when Gazebo is live
    )
    results = runner.linspace("link1_length", 0.3, 0.7, 5)
    report  = runner.compare()
    print(report.text)
"""
from __future__ import annotations

import math
from typing import Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from iteration.policy import Policy
    from iteration.loop import IterationResult
    from iteration.report import ComparisonReport


# ---------------------------------------------------------------------------
# Pure helpers
# ---------------------------------------------------------------------------

def linspace(start: float, stop: float, n: int) -> list[float]:
    """Return ``n`` evenly spaced values from ``start`` to ``stop`` (inclusive)."""
    if n <= 0:
        return []
    if n == 1:
        return [start]
    step = (stop - start) / (n - 1)
    return [start + i * step for i in range(n)]


def arange(start: float, stop: float, step: float) -> list[float]:
    """Return values from ``start`` to ``stop`` (exclusive) with given ``step``."""
    if step == 0:
        raise ValueError("step must not be zero")
    values: list[float] = []
    current = start
    if step > 0:
        while current < stop - 1e-12:
            values.append(round(current, 12))
            current += step
    else:
        while current > stop + 1e-12:
            values.append(round(current, 12))
            current += step
    return values


# ---------------------------------------------------------------------------
# SweepRunner
# ---------------------------------------------------------------------------

class SweepRunner:
    """
    High-level sweep orchestrator.

    Delegates to IterationLoop for each step.  In v1 all runs are serial
    (one at a time).  Results are accumulated in ``.results``.
    """

    def __init__(
        self,
        scenario_name:  str,
        policy:         "Policy",
        doc_name:       str = "",
        rpc_url:        str = "http://localhost:9875",
        bridge_module         = None,
        scenarios_dir         = None,
        sim_runs_dir          = None,
    ):
        from iteration.loop import IterationLoop
        self._loop = IterationLoop(
            scenario_name =scenario_name,
            policy        =policy,
            doc_name      =doc_name,
            rpc_url       =rpc_url,
            bridge_module =bridge_module,
            scenarios_dir =scenarios_dir,
            sim_runs_dir  =sim_runs_dir,
        )
        self.results: list["IterationResult"] = []

    # ------------------------------------------------------------------
    # Sweep methods
    # ------------------------------------------------------------------

    def linspace(
        self,
        param_name: str,
        start:      float,
        stop:       float,
        n:          int,
        **fixed_params: float,
    ) -> list["IterationResult"]:
        """Run sweep over ``n`` evenly spaced values of ``param_name``."""
        values = linspace(start, stop, n)
        return self._sweep(param_name, values, fixed_params)

    def arange(
        self,
        param_name: str,
        start:      float,
        stop:       float,
        step:       float,
        **fixed_params: float,
    ) -> list["IterationResult"]:
        """Run sweep using ``arange`` values of ``param_name``."""
        values = arange(start, stop, step)
        return self._sweep(param_name, values, fixed_params)

    def sweep(
        self,
        param_name: str,
        values:     list[float],
        **fixed_params: float,
    ) -> list["IterationResult"]:
        """Run sweep over an explicit list of values for ``param_name``."""
        return self._sweep(param_name, values, fixed_params)

    # ------------------------------------------------------------------
    # Comparison
    # ------------------------------------------------------------------

    def compare(self, results: Optional[list["IterationResult"]] = None) -> "ComparisonReport":
        """Compare all accumulated results (or a provided subset)."""
        return self._loop.compare(results or self.results)

    # ------------------------------------------------------------------
    # Internal
    # ------------------------------------------------------------------

    def _sweep(
        self,
        param_name:   str,
        values:       list[float],
        fixed_params: dict[str, float],
    ) -> list["IterationResult"]:
        batch: list["IterationResult"] = []
        for v in values:
            params = dict(fixed_params)
            params[param_name] = v
            r = self._loop.run_once(params)
            self.results.append(r)
            batch.append(r)
        return batch
