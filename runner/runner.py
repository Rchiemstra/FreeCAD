"""
runner/runner.py — top-level Test Runner API.

    list_tests()              -> list[str]     scenario names
    run_test(name)            -> RunResult
    run_all_tests()           -> list[RunResult]

These functions are callable from inside FreeCAD via ``execute_code()``
or from the command line:

    python -m runner.runner reach_top_shelf

They can also be exposed by the Workbench Test Runner panel.
"""
from __future__ import annotations

import logging
import os
import sys
from pathlib import Path
from typing import Optional, TYPE_CHECKING

log = logging.getLogger(__name__)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def list_tests(scenarios_dir: Optional[str | Path] = None) -> list[str]:
    """
    Return the names of all scenario YAML files in tests/scenarios/.

    Parameters
    ----------
    scenarios_dir : str | Path | None
        Override the default ``tests/scenarios/`` directory.

    Returns
    -------
    list[str]
        Scenario stem names (e.g. ``["reach_top_shelf"]``).
    """
    from runner.scenario import list_scenario_files
    d = Path(scenarios_dir) if scenarios_dir else _default_scenarios_dir()
    return [p.stem for p in list_scenario_files(d)]


def run_test(
    name: str,
    scenarios_dir: Optional[str | Path] = None,
    sim_runs_dir:  Optional[str | Path] = None,
    bridge_module=None,
    poll_interval: float = 0.1,
) -> "RunResult":
    """
    Load, execute, and evaluate one scenario by name.

    Parameters
    ----------
    name : str
        Scenario stem name (e.g. ``"reach_top_shelf"``).
    scenarios_dir : str | Path | None
        Override ``tests/scenarios/``.
    sim_runs_dir : str | Path | None
        Override ``sim_runs/``.
    bridge_module : module | None
        Inject a mock bridge module (for testing).
    poll_interval : float
        Poll interval in seconds during execution.

    Returns
    -------
    RunResult
    """
    from runner.scenario import load_scenario, ScenarioLoadError
    from runner.executor import ScenarioExecutor
    from runner.assertions import evaluate_all
    from runner.result import RunResult, write_result
    from bridge.run_context import begin_run, finalize_run, metadata_for_result

    d = Path(scenarios_dir) if scenarios_dir else _default_scenarios_dir()
    yaml_path = d / f"{name}.yaml"
    runs_root = Path(sim_runs_dir) if sim_runs_dir else None
    run_ctx = begin_run(name, runs_root)

    def _finish(result: RunResult) -> RunResult:
        result.run_id = run_ctx.run_id
        result.metadata = metadata_for_result()
        _try_write(result, runs_root)
        return result

    try:
        # --- Load scenario ---
        try:
            scenario = load_scenario(yaml_path)
        except ScenarioLoadError as exc:
            result = RunResult(
                scenario=_dummy_scenario(name),
                status="error",
                error_message=str(exc),
            )
            return _finish(result)

        # --- Execute ---
        executor = ScenarioExecutor(
            scenario=scenario,
            bridge_module=bridge_module,
            poll_interval=poll_interval,
        )
        try:
            telemetry, exec_status = executor.run()
        except Exception as exc:
            result = RunResult(
                scenario=scenario,
                status="error",
                error_message=f"Execution error: {exc}",
            )
            return _finish(result)

        if exec_status != "ok":
            result = RunResult(
                scenario=scenario,
                status="error",
                error_message=exec_status,
            )
            return _finish(result)

        # --- Evaluate assertions ---
        assertion_results = evaluate_all(
            scenario.assertions, telemetry, scenario.goal
        )

        passed = all(r.passed for r in assertion_results)
        result = RunResult(
            scenario=scenario,
            assertion_results=assertion_results,
            telemetry=telemetry,
            status="pass" if passed else "fail",
        )
        log.info("[Runner] %s", result.summary())
        return _finish(result)
    finally:
        finalize_run()


def run_all_tests(
    scenarios_dir: Optional[str | Path] = None,
    sim_runs_dir:  Optional[str | Path] = None,
    bridge_module=None,
) -> list["RunResult"]:
    """
    Run all scenarios in scenarios_dir.

    Returns a list of RunResult objects (one per scenario).
    Never raises — each scenario failure is captured in its RunResult.
    """
    names = list_tests(scenarios_dir)
    results = []
    for name in names:
        log.info("[Runner] Running scenario: %s", name)
        r = run_test(
            name          =name,
            scenarios_dir =scenarios_dir,
            sim_runs_dir  =sim_runs_dir,
            bridge_module =bridge_module,
        )
        results.append(r)
    return results


# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------

def _default_scenarios_dir() -> Path:
    """Locate tests/scenarios/ relative to the project root."""
    try:
        from bridge.project import load_project
        cfg = load_project()
        return Path(cfg.root) / "tests" / "scenarios"
    except Exception:
        return Path.cwd() / "tests" / "scenarios"


def _dummy_scenario(name: str) -> "Scenario":
    """Minimal placeholder Scenario for error results."""
    from runner.scenario import Scenario
    s = Scenario()
    s.name  = name
    s.robot = "unknown"
    return s


def _try_write(result: "RunResult", sim_runs_dir: Optional[Path | str]) -> None:
    from runner.result import write_result
    try:
        write_result(result, Path(sim_runs_dir) if sim_runs_dir else None)
    except Exception as exc:
        log.warning("[Runner] Could not write result.yaml: %s", exc)


# ---------------------------------------------------------------------------
# CLI entry point
# ---------------------------------------------------------------------------

# Import for type hints only (avoids circular imports at module load)
if TYPE_CHECKING:
    from runner.scenario import Scenario
    from runner.result import RunResult


def _sim_runs_dir_cli() -> Optional[Path]:
    raw = os.environ.get("SIM_RUNS_DIR", "").strip()
    return Path(raw) if raw else None


def _cli_main(argv: list[str]) -> int:
    import argparse
    parser = argparse.ArgumentParser(description="FreeCAD/Gazebo Test Runner")
    sub = parser.add_subparsers(dest="cmd")

    list_p = sub.add_parser("list", help="List available scenario tests")
    list_p.add_argument("--dir", default=None, help="Scenarios directory override")

    run_p = sub.add_parser("run", help="Run a scenario test")
    run_p.add_argument("name", help="Scenario name")
    run_p.add_argument("--dir", default=None, help="Scenarios directory override")

    all_p = sub.add_parser("run-all", help="Run all scenario tests")
    all_p.add_argument("--dir", default=None, help="Scenarios directory override")

    args = parser.parse_args(argv)

    if args.cmd == "list":
        names = list_tests(args.dir)
        if not names:
            print("(no scenarios found)")
        for n in names:
            print(n)
        return 0

    if args.cmd == "run":
        result = run_test(
            args.name,
            scenarios_dir=args.dir,
            sim_runs_dir=_sim_runs_dir_cli(),
        )
        print(result.summary())
        for ar in result.assertion_results:
            print(f"  {ar}")
        return 0 if result.status == "pass" else 1

    if args.cmd == "run-all":
        results = run_all_tests(
            scenarios_dir=args.dir,
            sim_runs_dir=_sim_runs_dir_cli(),
        )
        for r in results:
            print(r.summary())
        failed = sum(1 for r in results if r.status != "pass")
        return 0 if failed == 0 else 1

    parser.print_help()
    return 1


if __name__ == "__main__":
    from bridge.logging_config import configure_logging

    configure_logging()
    sys.exit(_cli_main(sys.argv[1:]))
