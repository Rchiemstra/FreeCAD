"""
iteration/loop.py — parameter iteration loop.

Orchestrates the design → export → simulate → evaluate cycle:

1. Validate proposed parameters against policy.
2. Set parameters in FreeCAD (via XML-RPC).
3. Export URDF (via bridge.freecad_bridge).
4. Run the scenario (via runner.runner.run_test).
5. Compare with previous result.
6. Return IterationResult.

All steps that require FreeCAD or Gazebo fail gracefully with a clear
error message stored in IterationResult.error — no exception propagates.

Usage::

    from iteration.loop import IterationLoop
    from iteration.policy import DEFAULT_ARM_2DOF_POLICY

    loop = IterationLoop(
        scenario_name="reach_top_shelf",
        policy=DEFAULT_ARM_2DOF_POLICY,
    )

    result = loop.run_once({"link1_length": 0.55})
    print(result.summary())

    sweep = loop.sweep("link1_length", [0.3, 0.4, 0.5, 0.6])
    report = loop.compare(sweep)
    print(report.text)
"""
from __future__ import annotations

import logging
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from runner.result import RunResult
    from iteration.policy import Policy
    from iteration.report import ComparisonReport, ResultDiff

log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# IterationResult
# ---------------------------------------------------------------------------

@dataclass
class IterationResult:
    """Result of one iteration step."""
    params:       dict[str, float]      = field(default_factory=dict)
    run_result:   Optional["RunResult"] = None
    diff:         Optional["ResultDiff"]= None
    error:        str                   = ""

    def passed(self) -> bool:
        return self.run_result is not None and self.run_result.status == "pass"

    def summary(self) -> str:
        parts = [f"params={self.params}"]
        if self.run_result is not None:
            parts.append(self.run_result.summary())
        if self.diff is not None:
            parts.append(self.diff.summary())
        if self.error:
            parts.append(f"ERROR: {self.error}")
        return " | ".join(parts)


# ---------------------------------------------------------------------------
# IterationLoop
# ---------------------------------------------------------------------------

class IterationLoop:
    """
    Manages the design-change → sim → evaluate cycle.

    Parameters
    ----------
    scenario_name : str
        Name of the scenario to run on every iteration.
    policy : Policy
        The edit policy governing allowed parameter changes.
    doc_name : str
        FreeCAD document name. Empty = use ActiveDocument.
    rpc_url : str
        FreeCAD XML-RPC endpoint.
    bridge_module : module | None
        Inject a mock Gazebo bridge for testing.
    scenarios_dir : Path | None
        Override tests/scenarios/.
    sim_runs_dir : Path | None
        Override sim_runs/.
    """

    def __init__(
        self,
        scenario_name: str,
        policy: "Policy",
        doc_name: str = "",
        rpc_url:  str = "http://localhost:9875",
        bridge_module=None,
        scenarios_dir: Optional[Path] = None,
        sim_runs_dir:  Optional[Path] = None,
    ):
        self._scenario_name = scenario_name
        self._policy        = policy
        self._doc_name      = doc_name
        self._rpc_url       = rpc_url
        self._bridge_module = bridge_module
        self._scenarios_dir = scenarios_dir
        self._sim_runs_dir  = sim_runs_dir
        self._history:  list[IterationResult] = []
        self._baseline: Optional["RunResult"] = None

    # ------------------------------------------------------------------
    # Public API
    # ------------------------------------------------------------------

    def run_once(self, params: dict[str, float]) -> IterationResult:
        """
        Apply ``params``, export URDF, run scenario, evaluate, return result.

        Any single-step failure returns an IterationResult with a non-empty
        ``error`` field instead of raising.
        """
        # Step 1: validate against policy
        ok, errors = self._policy.check_all(params)
        if not ok:
            r = IterationResult(params=params, error="; ".join(errors))
            self._history.append(r)
            return r

        # Step 2: set parameters in FreeCAD
        set_err = self._set_params(params)
        if set_err:
            r = IterationResult(params=params, error=f"set_parameter: {set_err}")
            self._history.append(r)
            return r

        # Step 3: export URDF
        export_err = self._export_urdf()
        if export_err:
            log.info("[IterationLoop] URDF export failed: %s (continuing with existing URDF)", export_err)
            # Non-fatal: continue with existing URDF on disk

        # Step 4: run scenario
        from runner.runner import run_test
        try:
            run_result = run_test(
                self._scenario_name,
                scenarios_dir=self._scenarios_dir,
                sim_runs_dir =self._sim_runs_dir,
                bridge_module=self._bridge_module,
            )
        except Exception as exc:
            r = IterationResult(params=params, error=f"run_test: {exc}")
            self._history.append(r)
            return r

        # Step 5: diff against baseline
        diff = None
        if self._baseline is not None:
            from iteration.report import diff_results
            diff = diff_results(self._baseline, run_result)
        else:
            self._baseline = run_result

        result = IterationResult(params=params, run_result=run_result, diff=diff)
        self._history.append(result)
        log.info("[IterationLoop] %s", result.summary())
        return result

    def sweep(
        self,
        param_name: str,
        values: list[float],
    ) -> list[IterationResult]:
        """
        Run the scenario for each value of one parameter.

        Other parameters remain at their most-recently-set value (or the
        FreeCAD document default if no prior iteration has run).

        Returns a list of IterationResult, one per value in ``values``.
        """
        results: list[IterationResult] = []
        for v in values:
            r = self.run_once({param_name: v})
            results.append(r)
        return results

    def compare(
        self,
        results: Optional[list[IterationResult]] = None,
    ) -> "ComparisonReport":
        """
        Compare a list of iteration results and return a ComparisonReport.

        If ``results`` is None, uses the full history.
        """
        from iteration.report import compare_results
        rs = results or self._history
        run_results = [r.run_result for r in rs if r.run_result is not None]
        return compare_results(run_results)

    @property
    def history(self) -> list[IterationResult]:
        return list(self._history)

    # ------------------------------------------------------------------
    # Internal helpers
    # ------------------------------------------------------------------

    def _set_params(self, params: dict[str, float]) -> str:
        """Set all parameters. Returns empty string on success, error message on failure."""
        from iteration.parameter import set_parameter
        for name, value in params.items():
            try:
                set_parameter(
                    param_name=name,
                    value=value,
                    doc_name=self._doc_name,
                    rpc_url=self._rpc_url,
                )
            except ConnectionError as exc:
                return (
                    f"{exc} "
                    "(FreeCAD not running — cannot apply parameter changes; "
                    "re-run with FreeCAD open)"
                )
            except Exception as exc:
                return str(exc)
        return ""

    def _export_urdf(self) -> str:
        """Export URDF from FreeCAD. Returns empty string on success."""
        try:
            from bridge.freecad_bridge import export_urdf
            from bridge.project import load_project
            cfg = load_project()
            out = Path(cfg.root) / "generated" / "arm_2dof.urdf"
            export_urdf(str(out))
            return ""
        except Exception as exc:
            return str(exc)
