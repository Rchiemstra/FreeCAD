"""
bridge/logging_config.py — structured logging setup for the bridge/runner/iteration stack.

Usage in any module::

    import logging
    log = logging.getLogger(__name__)

Call ``configure_logging()`` once at the entry-point (runner CLI, MCP server startup,
or FreeCAD workbench Init.py).  Library code only calls ``logging.getLogger(__name__)``
and never calls ``basicConfig`` or ``configure_logging()`` itself.

Log format (stdout, INFO by default):

    2026-05-11 14:32:01 INFO  bridge.freecad_bridge  export_urdf started path=/generated/arm.urdf
    2026-05-11 14:32:03 INFO  runner.executor          scenario=reach_top_shelf step=5/100 rtf=0.98
    2026-05-11 14:32:04 INFO  runner.assertions        PASS reach_target_within dist=0.04m
    2026-05-11 14:32:04 INFO  iteration.loop           set link1_length=0.55

All logged fields are plain strings, no JSON encoding required.  If you need
machine-readable logs, set ``LOG_FORMAT=json`` in the environment to get
newline-delimited JSON on the root logger.

For **bridge / E2E / runner** JSONL events (MCP tool timing, captures, permissions),
see ``bridge/structured_log.py`` and ``BRIDGE_STRUCTLOG_PATH`` / ``E2E_RUN_DIR``.
"""
from __future__ import annotations

import logging
import os
import sys
from typing import Optional


# ---------------------------------------------------------------------------
# Custom formatter
# ---------------------------------------------------------------------------

_TEXT_FMT   = "%(asctime)s %(levelname)-5s %(name)-30s %(message)s"
_TIME_FMT   = "%Y-%m-%d %H:%M:%S"


class _JsonFormatter(logging.Formatter):
    """One-line JSON per log record (no external deps)."""
    def format(self, record: logging.LogRecord) -> str:
        import json
        data = {
            "ts":      self.formatTime(record, self._style._fmt),  # type: ignore[attr-defined]
            "level":   record.levelname,
            "logger":  record.name,
            "msg":     record.getMessage(),
        }
        if record.exc_info:
            data["exc"] = self.formatException(record.exc_info)
        return json.dumps(data)


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------

def configure_logging(
    level:   str           = "INFO",
    fmt:     str           = "text",    # "text" | "json"
    stream                 = None,      # default: sys.stdout
    logfile: Optional[str] = None,
) -> None:
    """
    Configure the root logger for the bridge/runner/iteration stack.

    Parameters
    ----------
    level : str
        Logging level name: "DEBUG", "INFO", "WARNING", "ERROR".
        Overridden by the ``LOG_LEVEL`` environment variable if set.
    fmt : str
        "text" for human-readable output, "json" for newline-delimited JSON.
        Overridden by ``LOG_FORMAT`` environment variable if set.
    stream : IO | None
        Output stream.  Default: sys.stdout.
    logfile : str | None
        Optional path to a log file.  Appended to; not rotated.
    """
    level  = os.environ.get("LOG_LEVEL",  level).upper()
    fmt    = os.environ.get("LOG_FORMAT", fmt).lower()
    stream = stream or sys.stdout

    root = logging.getLogger()
    root.setLevel(level)

    # Remove handlers added by earlier calls (idempotent reconfigure)
    for h in list(root.handlers):
        root.removeHandler(h)

    formatter: logging.Formatter
    if fmt == "json":
        formatter = _JsonFormatter()
    else:
        formatter = logging.Formatter(_TEXT_FMT, datefmt=_TIME_FMT)

    console = logging.StreamHandler(stream)
    console.setFormatter(formatter)
    root.addHandler(console)

    if logfile:
        fh = logging.FileHandler(logfile, encoding="utf-8")
        fh.setFormatter(formatter)
        root.addHandler(fh)

    # Silence noisy third-party loggers
    for noisy in ("urllib3", "paramiko", "asyncio", "git"):
        logging.getLogger(noisy).setLevel(logging.WARNING)


def get_logger(name: str) -> logging.Logger:
    """Convenience wrapper — same as ``logging.getLogger(name)``."""
    return logging.getLogger(name)
