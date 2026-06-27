"""
runner/result.py — simulation run result writer.

Writes ``sim_runs/<timestamp>_<scenario>/result.yaml`` with:
  - status (pass/fail/error)
  - assertion results
  - input hashes (scenario YAML, URDF, world SDF)
  - tool versions (FreeCAD, Gazebo, MCP, Python)
  - physics settings from project.yaml
  - telemetry summary

Usage::

    from runner.result import write_result, RunResult, load_result

    run_result = RunResult(scenario=scenario, results=assertion_results, ...)
    path = write_result(run_result)      # -> Path
    loaded = load_result(path)           # -> dict (raw YAML)
"""
from __future__ import annotations

import datetime
import hashlib
import platform
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, TYPE_CHECKING

if TYPE_CHECKING:
    from runner.scenario import Scenario
    from runner.assertions import AssertionResult, Telemetry


# ---------------------------------------------------------------------------
# RunResult dataclass
# ---------------------------------------------------------------------------

@dataclass
class RunResult:
    """Aggregated result of a single test scenario run."""
    scenario:          "Scenario"
    assertion_results: list["AssertionResult"] = field(default_factory=list)
    telemetry:         Optional["Telemetry"]   = None
    status:            str                     = "unknown"  # pass / fail / error
    error_message:     str                     = ""         # set on error
    run_id:            str                     = ""         # timestamp_scenarioname
    run_dir:           Optional[Path]          = None       # sim_runs/<run_id>/
    metadata:          dict                    = field(default_factory=dict)

    def __post_init__(self):
        if not self.run_id:
            ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
            self.run_id = f"{ts}_{self.scenario.name}"
        if self.status == "unknown":
            if self.error_message:
                self.status = "error"
            elif self.assertion_results:
                self.status = "pass" if all(r.passed for r in self.assertion_results) else "fail"

    def summary(self) -> str:
        total  = len(self.assertion_results)
        passed = sum(1 for r in self.assertion_results if r.passed)
        return (
            f"[{self.status.upper()}] {self.scenario.name} — "
            f"{passed}/{total} assertions passed"
        )


# ---------------------------------------------------------------------------
# Writer
# ---------------------------------------------------------------------------

def write_result(
    run_result: "RunResult",
    sim_runs_dir: Optional[Path] = None,
) -> Path:
    """
    Write result.yaml (and telemetry.yaml if available) to disk.

    Returns the path to ``result.yaml``.
    """
    if sim_runs_dir is None:
        sim_runs_dir = _find_sim_runs_dir()

    from bridge.run_context import current_run

    ctx = current_run()
    if ctx is not None and ctx.run_id == run_result.run_id:
        run_dir = ctx.run_dir
    else:
        run_dir = Path(sim_runs_dir) / run_result.run_id
        run_dir.mkdir(parents=True, exist_ok=True)
    run_result.run_dir = run_dir

    from bridge.permissions import PermissionDenied, WriteOperation, assert_write_allowed
    from bridge.schema_validate import SchemaValidationError, validate_instance

    try:
        assert_write_allowed(
            WriteOperation.RUNNER_WRITE_RESULT,
            target=run_dir,
        )
    except PermissionDenied as exc:
        raise OSError(str(exc)) from exc

    result_data = _build_result_dict(run_result)
    try:
        validate_instance(result_data, "result", instance_label=str(run_dir / "result.yaml"))
    except SchemaValidationError as exc:
        raise ValueError(str(exc)) from exc

    result_path = run_dir / "result.yaml"
    _write_yaml(result_data, result_path)

    if run_result.telemetry is not None:
        _write_yaml(_build_telemetry_dict(run_result.telemetry), run_dir / "telemetry.yaml")

    return result_path


def load_result(path: str | Path) -> dict:
    """Load a result.yaml file and return the raw dict."""
    import yaml  # type: ignore
    p = Path(path)
    return yaml.safe_load(p.read_text(encoding="utf-8")) or {}


# ---------------------------------------------------------------------------
# Internal helpers
# ---------------------------------------------------------------------------

def _build_result_dict(r: "RunResult") -> dict:
    assertions = [
        {
            "type":    ar.assertion_type,
            "passed":  ar.passed,
            "message": ar.message,
            "detail":  ar.detail,
        }
        for ar in r.assertion_results
    ]

    versions = _collect_versions()

    input_hashes: dict = {}
    if r.scenario.source_hash:
        input_hashes["scenario_yaml"] = r.scenario.source_hash
    input_hashes.update(_hash_robot_world(r.scenario))
    meta = dict(r.metadata) if r.metadata else {}
    if not meta:
        from bridge.run_context import metadata_for_result

        meta = metadata_for_result()
    for key, digest in (meta.get("file_hashes") or {}).items():
        input_hashes.setdefault(key, digest)
    for key, digest in (meta.get("source_hashes") or {}).items():
        input_hashes.setdefault(key, digest)

    telem_summary: dict = {}
    if r.telemetry is not None:
        t = r.telemetry
        telem_summary = {
            "sim_duration_s":   t.sim_duration,
            "avg_rtf":          t.avg_rtf,
            "ee_pose_samples":  len(t.ee_poses),
            "joint_samples":    len(t.joint_states),
            "contact_events":   len(t.contacts),
        }

    out: dict = {
        "run_id":           r.run_id,
        "status":           r.status,
        "error":            r.error_message or None,
        "scenario":         r.scenario.name,
        "robot":            r.scenario.robot,
        "world":            r.scenario.world,
        "timestamp":        datetime.datetime.now().isoformat(),
        "versions":         versions,
        "input_hashes":     input_hashes,
        "telemetry_summary":telem_summary,
        "assertions":       assertions,
    }
    if meta:
        out["metadata"] = meta
    return out


def _build_telemetry_dict(t: "Telemetry") -> dict:
    return {
        "scenario_name": t.scenario_name,
        "sim_duration":  t.sim_duration,
        "avg_rtf":       t.avg_rtf,
        "ee_poses": [
            {"sim_time": s.sim_time, "x": s.x, "y": s.y, "z": s.z}
            for s in t.ee_poses
        ],
        "joint_states": [
            {
                "sim_time": s.sim_time,
                "names":    s.names,
                "positions": s.positions,
                "efforts":  s.efforts,
            }
            for s in t.joint_states
        ],
        "contacts": [
            {"sim_time": c.sim_time, "link_a": c.link_a, "link_b": c.link_b}
            for c in t.contacts
        ],
    }


def _collect_versions() -> dict:
    versions: dict = {
        "python": sys.version.split()[0],
        "platform": platform.system(),
    }
    try:
        import yaml  # type: ignore
        versions["pyyaml"] = yaml.__version__
    except Exception:
        pass
    try:
        import mcp  # type: ignore
        versions["mcp"] = getattr(mcp, "__version__", "unknown")
    except Exception:
        pass
    return versions


def _hash_file(path: Path) -> Optional[str]:
    if not path.is_file():
        return None
    try:
        return hashlib.sha256(path.read_bytes()).hexdigest()
    except OSError:
        return None


def _hash_robot_world(scenario: "Scenario") -> dict:
    """Compute SHA-256 of robot URDF and world SDF (robots/, worlds/, generated/)."""
    hashes: dict = {}
    try:
        from bridge.project import load_project
        cfg = load_project()
        root = Path(cfg.paths.root)
        fcstd = root / "robots" / f"{scenario.robot}.FCStd"
        if fcstd.is_file():
            digest = _hash_file(fcstd)
            if digest:
                hashes["fcstd"] = digest

        candidates = [
            ("robot_urdf", root / "robots" / f"{scenario.robot}.urdf"),
            ("world_sdf", root / "worlds" / f"{scenario.world}.sdf"),
            (
                "export_urdf",
                root / "generated" / scenario.robot / f"{scenario.robot}_description"
                / f"{scenario.robot}_description" / "urdf" / f"{scenario.robot}.urdf",
            ),
            ("world_staged", root / "generated" / scenario.world / f"{scenario.world}.sdf"),
        ]
        for key, path in candidates:
            digest = _hash_file(path)
            if digest:
                hashes[key] = digest
    except Exception:
        pass
    return hashes


def _find_sim_runs_dir() -> Path:
    """Locate or create sim_runs/ relative to project root."""
    try:
        from bridge.project import load_project
        cfg = load_project()
        d = Path(cfg.root) / "sim_runs"
    except Exception:
        d = Path.cwd() / "sim_runs"
    d.mkdir(exist_ok=True)
    return d


def _write_yaml(data: dict, path: Path) -> None:
    import yaml  # type: ignore
    path.write_text(yaml.dump(data, default_flow_style=False, sort_keys=True),
                    encoding="utf-8")
