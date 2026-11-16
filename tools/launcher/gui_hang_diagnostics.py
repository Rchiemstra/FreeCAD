# SPDX-License-Identifier: LGPL-2.1-or-later
"""Opt-in GUI liveness evidence. No model access, RPC, process stop or restart.

The timeout runs in CPython's native faulthandler watchdog, so it can dump even
when Python code or a Coin call holds the GIL. Only a real Qt timer callback
advances the heartbeat. Runtime imports of FreeCAD/PySide occur in install().
"""
from __future__ import annotations

import hashlib
import json
import math
import os
from pathlib import Path
import sys
import tempfile
import time

HEARTBEAT_INTERVAL_MS = 2000
STALL_SECONDS = 30.0
MAX_STATUS_BYTES = 16 * 1024
# CPython 3.10-3.13 limits each dump to 100 threads, 100 frames and two strings
# of 500 characters/frame. Even 10-byte Unicode escapes fit below 128 MiB.
# C-stack dumping introduced by newer runtimes is deliberately not enabled.
MAX_TRACE_BYTES = 128 * 1024 * 1024
SOURCE_MAX_BYTES = 256 * 1024


def _status(path, value):
    raw = (json.dumps(value, sort_keys=True, allow_nan=False) + "\n").encode()
    if len(raw) > MAX_STATUS_BYTES:
        raise ValueError("GUI diagnostics status exceeds its fixed byte bound")
    temporary = path.with_suffix(".tmp")
    with temporary.open("wb") as stream:
        stream.write(raw)
    temporary.replace(path)


def prepare_launch(profile, arguments):
    """Create a separate run directory and put its macro before model arguments."""
    profile = Path(profile).expanduser().resolve()
    base = profile / "diagnostics"
    base.mkdir(parents=True, exist_ok=True)
    directory = Path(tempfile.mkdtemp(prefix="gui-", dir=base))
    runtime = directory / "gui_hang_diagnostics.py"
    with Path(__file__).open("rb") as stream:
        code = stream.read(SOURCE_MAX_BYTES + 1)
    if len(code) > SOURCE_MAX_BYTES:
        raise ValueError("GUI diagnostics runtime exceeds its bounded source size")
    with runtime.open("xb") as stream:
        stream.write(code)
    macro = directory / "GuiDiagnostics.FCMacro"
    # repr quotes paths safely, including spaces and Windows backslashes. No
    # model filenames or authentication material enter this bootstrap.
    text = (
        "import runpy as _diagnostics_runpy\n"
        "try:\n"
        f"    _diagnostics_runtime = _diagnostics_runpy.run_path({str(runtime)!r})\n"
        f"    _diagnostics_runtime['install']({str(directory)!r})\n"
        "except Exception as _diagnostics_error:\n"
        "    print('GUI diagnostics installation failed: ' + type(_diagnostics_error).__name__)\n"
    )
    with macro.open("x", encoding="utf-8") as stream:
        stream.write(text)
    argv = list(arguments)
    # Respect a user-selected native log rather than changing its destination.
    user_log = any(a in ("--log-file", "--write-log", "-l") or a.startswith("--log-file=") for a in argv)
    prefix = [] if user_log else ["--log-file", str(directory / "FreeCAD.log")]
    # The isolated launcher removes its own options wherever they occur; its
    # supervision/timeouts/profile flags are otherwise retained verbatim.
    argv = [*prefix, str(macro), *argv]
    _status(directory / "session.json", {
        "schema_version": 1, "mode": "opt_in_gui_diagnostics",
        "runtime_sha256": hashlib.sha256(code).hexdigest(),
        "heartbeat_interval_ms": HEARTBEAT_INTERVAL_MS, "stall_seconds": STALL_SECONDS,
        "maximum_status_bytes": MAX_STATUS_BYTES, "maximum_trace_bytes": MAX_TRACE_BYTES,
        "watchdog_repeat": False, "watchdog_exit": False,
        "native_log_supplied_by_user": user_log,
        "model_data_or_authentication_logged": False,
    })
    return directory, argv


class GuiHeartbeat:
    """One GUI timer and one nonfatal faulthandler timeout, with fixed retention."""

    def __init__(self, directory, *, fault_handler, clock=time.monotonic, stall_seconds=STALL_SECONDS):
        if not math.isfinite(stall_seconds) or stall_seconds <= 0:
            raise ValueError("Positive finite GUI diagnostics timeout required")
        self.directory = Path(directory).resolve()
        self.fault = fault_handler
        self.clock = clock
        self.stall_seconds = float(stall_seconds)
        self.timer = None
        self.closed = False
        self.sequence = 0
        self.recoveries = 0
        self.arms = 0
        self.last_gap = 0.0
        self.last_tick = float(clock())
        if not math.isfinite(self.last_tick):
            raise ValueError("GUI diagnostics monotonic clock is invalid")
        self.last_trace_bytes = 0
        self.phase = "installed_waiting_for_qt_tick"
        self.trace = None
        self.crash = None
        self.enabled_faults = False
        try:
            self.trace = (self.directory / "python-timeout-current.txt").open("xb+", buffering=0)
            self.crash = (self.directory / "python-faults.txt").open("xb", buffering=0)
            # Preserve an existing fatal-signal handler and its destination.
            if not self.fault.is_enabled():
                self.fault.enable(file=self.crash, all_threads=True)
                self.enabled_faults = True
            self._arm()
            self._publish()
        except BaseException:
            self.close()
            raise

    def _arm(self):
        self.fault.dump_traceback_later(self.stall_seconds, repeat=False, file=self.trace, exit=False)
        self.arms += 1

    def _publish(self):
        _status(self.directory / "heartbeat.json", {
            "schema_version": 1, "pid": os.getpid(), "phase": self.phase,
            "heartbeat_sequence": self.sequence, "recovery_count": self.recoveries,
            "watchdog_arm_count": self.arms, "last_qt_tick_monotonic": self.last_tick,
            "last_qt_gap_seconds": self.last_gap, "last_timeout_trace_bytes": self.last_trace_bytes,
            "stall_seconds": self.stall_seconds, "watchdog_repeat": False, "watchdog_exit": False,
            "fatal_handler_installed_here": self.enabled_faults,
            "model_data_or_authentication_logged": False,
        })

    def _retain_last_trace(self):
        self.trace.seek(0, os.SEEK_END)
        size = self.trace.tell()
        if not size:
            return False
        self.last_trace_bytes = min(size, MAX_TRACE_BYTES)
        self.trace.seek(0)
        with (self.directory / "python-timeout-last.txt").open("wb") as target:
            remaining = min(size, MAX_TRACE_BYTES)
            while remaining:
                block = self.trace.read(min(remaining, 64 * 1024))
                if not block:
                    break
                target.write(block)
                remaining -= len(block)
        self.trace.seek(0)
        self.trace.truncate(0)
        if size > MAX_TRACE_BYTES:
            # A foreign/unqualified writer must not cause continuing growth.
            raise ValueError("GUI timeout dump exceeded the qualified CPython writer bound")
        return True

    def tick(self):
        if self.closed:
            return
        try:
            now = float(self.clock())
            if not math.isfinite(now) or now < self.last_tick:
                raise ValueError("GUI diagnostics monotonic clock is invalid")
            # Cancellation joins the native watchdog before copying/resetting
            # its FD, so recovery never races an in-progress traceback write.
            self.fault.cancel_dump_traceback_later()
            self.last_gap = now - self.last_tick
            trace_written = self._retain_last_trace()
            if self.last_gap >= self.stall_seconds or trace_written:
                self.recoveries += 1
            self.sequence += 1
            self.last_tick = now
            self.phase = "qt_heartbeat"
            self._arm()
            self._publish()
        except Exception:
            # Diagnostics failure must not escape into the user's Qt callback.
            self.phase = "diagnostics_failed"
            self.close()

    def close(self):
        """Best-effort cleanup; no diagnostic I/O error may escape a Qt signal."""
        if self.closed:
            return
        self.closed = True
        actions = [self.fault.cancel_dump_traceback_later]
        if self.timer is not None:
            actions.insert(0, self.timer.stop)
        if self.enabled_faults:
            actions.append(self.fault.disable)
        for action in actions:
            try:
                action()
            except Exception:
                self.phase = "diagnostics_failed"
        if self.phase != "diagnostics_failed":
            self.phase = "qt_about_to_quit_or_monitor_stopped"
        try:
            self._publish()
        except Exception:
            pass
        for stream in (self.trace, self.crash):
            if stream is not None:
                try:
                    stream.close()
                except Exception:
                    pass


def install(directory, *, qt_core=None, application=None, fault_handler=None, clock=time.monotonic):
    """Install from the startup macro; never inspect or modify a document."""
    if sys.implementation.name != "cpython" or not (3, 10) <= sys.version_info[:2] <= (3, 13):
        raise RuntimeError("Bounded GUI diagnostics currently supports CPython 3.10-3.13")
    if qt_core is None or application is None:
        from PySide import QtCore, QtWidgets
        qt_core = QtCore
        application = QtWidgets.QApplication.instance()
    if application is None:
        raise RuntimeError("GUI diagnostics requires the existing Qt application")
    previous = getattr(application, "_freecad_gui_hang_diagnostics", None)
    if previous is not None and not previous.closed:
        return previous
    if fault_handler is None:
        import faulthandler
        fault_handler = faulthandler
    monitor = GuiHeartbeat(directory, fault_handler=fault_handler, clock=clock)
    try:
        timer = qt_core.QTimer(application)
        monitor.timer = timer
        timer.timeout.connect(monitor.tick)
        application.aboutToQuit.connect(monitor.close)
        setattr(application, "_freecad_gui_hang_diagnostics", monitor)
        timer.start(HEARTBEAT_INTERVAL_MS)
    except BaseException:
        monitor.close()
        raise
    print("GUI diagnostics installed; heartbeat and nonfatal Python traces: " + str(Path(directory).resolve()))
    return monitor
