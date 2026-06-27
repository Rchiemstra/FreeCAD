"""
Per-run logging and metadata for sim_runs/<run_id>/.

Creates ``run.log``, ``run_events.yaml``, and supplies metadata merged into
``result.yaml`` (policy mode, paths, hashes, lifecycle events).

Use :func:`begin_run` at scenario entry and :func:`finalize_run` in ``finally``.
"""

from __future__ import annotations

import datetime
import hashlib
import logging
import os
from contextvars import ContextVar
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Dict, List, Optional

_current: ContextVar[Optional["RunContext"]] = ContextVar("bridge_run_context", default=None)

_LOG = logging.getLogger(__name__)


@dataclass
class RunContext:
    run_id: str
    run_dir: Path
    scenario_name: str
    events: List[Dict[str, Any]] = field(default_factory=list)
    paths: Dict[str, str] = field(default_factory=dict)
    file_hashes: Dict[str, str] = field(default_factory=dict)
    source_hashes: Dict[str, str] = field(default_factory=dict)
    _handler: Optional[logging.Handler] = field(default=None, repr=False)

    def record_event(
        self,
        category: str,
        message: str,
        *,
        level: str = "info",
        **fields: Any,
    ) -> None:
        entry: Dict[str, Any] = {
            "ts": datetime.datetime.now().isoformat(timespec="seconds"),
            "category": category,
            "level": level,
            "message": message,
        }
        for key, val in fields.items():
            if val is not None:
                entry[key] = val
        self.events.append(entry)
        _LOG.log(
            getattr(logging, level.upper(), logging.INFO),
            "[%s] %s %s",
            category,
            message,
            " ".join(f"{k}={v}" for k, v in fields.items()) if fields else "",
        )

    def set_path(self, key: str, path: str | Path | None) -> None:
        if path is None:
            return
        p = Path(path)
        self.paths[key] = str(p.resolve() if p.exists() or p.is_absolute() else p)
        self.add_file_hash(key, p)

    def add_file_hash(self, key: str, path: str | Path) -> None:
        p = Path(path)
        if not p.is_file():
            return
        try:
            self.file_hashes[key] = hashlib.sha256(p.read_bytes()).hexdigest()
        except OSError:
            pass

    def build_metadata(self) -> Dict[str, Any]:
        from bridge.permissions import effective_write_policy

        meta: Dict[str, Any] = {
            "write_policy": effective_write_policy(),
            "paths": dict(self.paths),
            "lifecycle_events": [
                e for e in self.events if e.get("category") == "lifecycle"
            ],
            "event_count": len(self.events),
        }
        if self.file_hashes:
            meta["file_hashes"] = dict(self.file_hashes)
        if self.source_hashes:
            meta["source_hashes"] = dict(self.source_hashes)
        parent = os.environ.get("E2E_RUN_DIR", "").strip()
        if parent:
            meta["e2e_run_dir"] = parent
        bridge = os.environ.get("E2E_BRIDGE_MODULE", "").strip()
        if bridge:
            meta["bridge_module"] = bridge
        return meta

    def finalize(self) -> None:
        """Write run_events.yaml and detach the run log handler."""
        if self._handler is not None:
            root = logging.getLogger()
            root.removeHandler(self._handler)
            self._handler.close()
            self._handler = None

        events_path = self.run_dir / "run_events.yaml"
        try:
            import yaml  # type: ignore

            payload = {
                "run_id": self.run_id,
                "scenario": self.scenario_name,
                "events": self.events,
            }
            events_path.write_text(
                yaml.dump(payload, default_flow_style=False, sort_keys=False),
                encoding="utf-8",
            )
        except Exception as exc:
            _LOG.warning("Could not write %s: %s", events_path, exc)

        _current.set(None)


class _RunFileHandler(logging.Handler):
    """Append formatted log lines to sim_runs/<run_id>/run.log."""

    def __init__(self, log_path: Path) -> None:
        super().__init__()
        self._log_path = log_path
        self.setFormatter(
            logging.Formatter(
                "%(asctime)s %(levelname)-5s %(name)-30s %(message)s",
                datefmt="%Y-%m-%d %H:%M:%S",
            )
        )

    def emit(self, record: logging.LogRecord) -> None:
        try:
            line = self.format(record)
            with self._log_path.open("a", encoding="utf-8") as fh:
                fh.write(line + "\n")
                fh.flush()
            ctx = _current.get()
            if ctx is not None and record.levelno >= logging.INFO:
                ctx.events.append(
                    {
                        "ts": datetime.datetime.now().isoformat(timespec="seconds"),
                        "category": "log",
                        "level": record.levelname.lower(),
                        "message": record.getMessage(),
                        "logger": record.name,
                    }
                )
        except Exception:
            self.handleError(record)


def current_run() -> Optional[RunContext]:
    return _current.get()


def begin_run(
    scenario_name: str,
    sim_runs_dir: Optional[Path] = None,
    *,
    run_id: Optional[str] = None,
) -> RunContext:
    """
    Start a run directory under ``sim_runs/`` and attach per-run file logging.

    Idempotent if a context is already active (returns existing context).
    """
    existing = _current.get()
    if existing is not None:
        return existing

    if sim_runs_dir is None:
        from runner.result import _find_sim_runs_dir

        sim_runs_dir = _find_sim_runs_dir()

    if not run_id:
        ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
        run_id = f"{ts}_{scenario_name}"

    run_dir = Path(sim_runs_dir) / run_id
    run_dir.mkdir(parents=True, exist_ok=True)

    ctx = RunContext(
        run_id=run_id,
        run_dir=run_dir,
        scenario_name=scenario_name,
    )
    ctx.record_event(
        "lifecycle",
        "run_started",
        scenario=scenario_name,
        run_dir=str(run_dir),
    )

    from bridge.logging_config import configure_logging

    if not logging.getLogger().handlers:
        configure_logging()

    log_path = run_dir / "run.log"
    log_path.write_text(
        f"# run_id={run_id} scenario={scenario_name}\n",
        encoding="utf-8",
    )
    handler = _RunFileHandler(log_path)
    handler.setLevel(logging.DEBUG)
    root = logging.getLogger()
    root.addHandler(handler)
    if root.level > logging.DEBUG:
        root.setLevel(logging.DEBUG)
    ctx._handler = handler

    _current.set(ctx)
    return ctx


def finalize_run() -> None:
    """Finalize the active run context, if any."""
    ctx = _current.get()
    if ctx is not None:
        ctx.record_event("lifecycle", "run_finished")
        ctx.finalize()


def record_event(category: str, message: str, **fields: Any) -> None:
    ctx = _current.get()
    if ctx is not None:
        ctx.record_event(category, message, **fields)


def record_path(key: str, path: str | Path | None) -> None:
    ctx = _current.get()
    if ctx is not None:
        ctx.set_path(key, path)


def record_lifecycle(phase: str, **fields: Any) -> None:
    record_event("lifecycle", phase, **fields)


def metadata_for_result() -> Dict[str, Any]:
    ctx = _current.get()
    return ctx.build_metadata() if ctx is not None else {}
