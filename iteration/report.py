"""
iteration/report.py — result comparison and failure summarization.

Compares two or more RunResult objects and generates a human-readable
report that an LLM can use to decide on the next design change.

Usage::

    from iteration.report import compare_results, summarize_failure

    report = compare_results(results)   # list[RunResult]
    print(report.text)                  # markdown table

    msg = summarize_failure(result)     # single RunResult
    # "FAIL: 2/7 assertions failed.
    #   - reach_target_within: Did not reach target within 8s — min dist=0.32m
    #   - rtf_above: avg_rtf=0.28 (< threshold=0.5)
    # Suggestion: try increasing link2_length or reducing payload mass."
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import TYPE_CHECKING

if TYPE_CHECKING:
    from runner.result import RunResult
    from runner.assertions import AssertionResult


# ---------------------------------------------------------------------------
# ComparisonReport
# ---------------------------------------------------------------------------

@dataclass
class ComparisonReport:
    """
    Comparison of multiple run results (e.g. a parameter sweep).

    Attributes
    ----------
    results : list[RunResult]
    text : str
        Human-readable markdown table.
    best_run_id : str
        run_id of the run with the most assertions passed.
    worst_run_id : str
        run_id with the fewest assertions passed.
    """
    results:      list["RunResult"] = field(default_factory=list)
    text:         str = ""
    best_run_id:  str = ""
    worst_run_id: str = ""


def compare_results(results: list["RunResult"]) -> ComparisonReport:
    """
    Compare a list of RunResult objects and produce a ComparisonReport.

    The text is a markdown table with one row per result.
    """
    if not results:
        return ComparisonReport(text="(no results to compare)")

    # Collect all assertion types across all results
    all_types: list[str] = []
    for r in results:
        for ar in r.assertion_results:
            if ar.assertion_type not in all_types:
                all_types.append(ar.assertion_type)

    # Build header
    lines: list[str] = []
    header = "| Run ID | Status | " + " | ".join(all_types) + " |"
    sep    = "|--------|--------|" + "|".join("--------" for _ in all_types) + "|"
    lines.append(header)
    lines.append(sep)

    best_score, worst_score = -1, 999
    best_run_id = worst_run_id = ""

    for r in results:
        score = sum(1 for ar in r.assertion_results if ar.passed)
        total = len(r.assertion_results)

        if score > best_score:
            best_score, best_run_id = score, r.run_id
        if score < worst_score:
            worst_score, worst_run_id = score, r.run_id

        cells: list[str] = []
        for atype in all_types:
            match = next((ar for ar in r.assertion_results if ar.assertion_type == atype), None)
            if match is None:
                cells.append("–")
            elif match.passed:
                cells.append("✓")
            else:
                cells.append("✗")

        row = f"| {r.run_id[:30]} | {r.status} | " + " | ".join(cells) + " |"
        lines.append(row)

    lines.append("")
    lines.append(f"Best:  {best_run_id}  ({best_score}/{len(all_types)} passed)")
    lines.append(f"Worst: {worst_run_id} ({worst_score}/{len(all_types)} passed)")

    return ComparisonReport(
        results      =results,
        text         ="\n".join(lines),
        best_run_id  =best_run_id,
        worst_run_id =worst_run_id,
    )


def summarize_failure(result: "RunResult") -> str:
    """
    Return a concise failure summary for an LLM agent to read.

    If the result is a pass, returns a brief success message.
    If the result has an error, returns the error.
    Otherwise, lists each failed assertion with its message and a suggestion.
    """
    if result.status == "pass":
        total = len(result.assertion_results)
        return f"PASS: All {total} assertions passed for {result.scenario.name!r}."

    if result.status == "error":
        return f"ERROR running {result.scenario.name!r}: {result.error_message}"

    failed = [ar for ar in result.assertion_results if not ar.passed]
    passed = [ar for ar in result.assertion_results if ar.passed]
    lines  = [
        f"FAIL: {len(failed)}/{len(result.assertion_results)} assertions failed "
        f"for {result.scenario.name!r}."
    ]

    for ar in failed:
        lines.append(f"  ✗ {ar.assertion_type}: {ar.message}")

    lines.append("")
    lines.append(f"Passed: {', '.join(ar.assertion_type for ar in passed) or 'none'}")

    # Heuristic suggestions based on what failed
    suggestions = _generate_suggestions(failed)
    if suggestions:
        lines.append("")
        lines.append("Suggestions:")
        for s in suggestions:
            lines.append(f"  • {s}")

    return "\n".join(lines)


def _generate_suggestions(failed: list["AssertionResult"]) -> list[str]:
    """Generate heuristic design-change suggestions based on failed assertions."""
    suggestions: list[str] = []
    types = {ar.assertion_type for ar in failed}

    if "reach_target_within" in types or "pose_within_tolerance" in types:
        suggestions.append(
            "Robot cannot reach the target: consider increasing link lengths "
            "(link1_length, link2_length) or adjusting joint limits."
        )

    if "max_joint_torque_below" in types:
        suggestions.append(
            "Joint torques are too high: consider reducing link masses "
            "(link1_mass, link2_mass) or the target payload."
        )

    if "rtf_above" in types:
        suggestions.append(
            "Real-time factor is too low: simplify collision meshes or "
            "increase the physics step size (world SDF timestep)."
        )

    if "no_self_collision" in types or "collision_count_below" in types:
        suggestions.append(
            "Self-collisions detected: widen the link visual/collision geometry "
            "or adjust joint limits to avoid overlapping configurations."
        )

    if "sim_time_under" in types:
        suggestions.append(
            "Simulation ran longer than allowed: check that the controller "
            "terminates cleanly and that the scenario duration is set correctly."
        )

    return suggestions


# ---------------------------------------------------------------------------
# ResultDiff — compare two runs directly
# ---------------------------------------------------------------------------

@dataclass
class ResultDiff:
    """Difference between a baseline and a new run."""
    baseline_id:    str
    new_id:         str
    newly_passing:  list[str]   = field(default_factory=list)
    newly_failing:  list[str]   = field(default_factory=list)
    unchanged_pass: list[str]   = field(default_factory=list)
    unchanged_fail: list[str]   = field(default_factory=list)

    def improved(self) -> bool:
        return len(self.newly_passing) > len(self.newly_failing)

    def summary(self) -> str:
        lines = [f"Diff: {self.baseline_id} → {self.new_id}"]
        if self.newly_passing:
            lines.append(f"  + {', '.join(self.newly_passing)} (now passing)")
        if self.newly_failing:
            lines.append(f"  − {', '.join(self.newly_failing)} (now failing)")
        if not self.newly_passing and not self.newly_failing:
            lines.append("  (no change in pass/fail status)")
        verdict = "IMPROVED" if self.improved() else ("REGRESSED" if self.newly_failing else "UNCHANGED")
        lines.append(f"  Verdict: {verdict}")
        return "\n".join(lines)


def diff_results(baseline: "RunResult", new: "RunResult") -> ResultDiff:
    """Compare two RunResult objects and report which assertions changed status."""
    def _map(r: "RunResult") -> dict[str, bool]:
        return {ar.assertion_type: ar.passed for ar in r.assertion_results}

    b = _map(baseline)
    n = _map(new)
    all_types = set(b) | set(n)

    newly_passing:  list[str] = []
    newly_failing:  list[str] = []
    unchanged_pass: list[str] = []
    unchanged_fail: list[str] = []

    for t in sorted(all_types):
        was = b.get(t, False)
        now = n.get(t, False)
        if not was and now:
            newly_passing.append(t)
        elif was and not now:
            newly_failing.append(t)
        elif was and now:
            unchanged_pass.append(t)
        else:
            unchanged_fail.append(t)

    return ResultDiff(
        baseline_id   =baseline.run_id,
        new_id        =new.run_id,
        newly_passing =newly_passing,
        newly_failing =newly_failing,
        unchanged_pass=unchanged_pass,
        unchanged_fail=unchanged_fail,
    )
