#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-2.1-or-later
"""Part 3 stress coordinator: provision, launch, child spawn, preflight, evidence."""

from __future__ import annotations

import argparse
import contextlib
import importlib.util
import json
import os
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import uuid
from dataclasses import dataclass, field
from datetime import datetime, timezone
from pathlib import Path
from time import monotonic, sleep
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[3]

if str(REPO_ROOT) not in sys.path:
    sys.path.insert(0, str(REPO_ROOT))

# Absolute imports so that both
#   python tests/gui/part3/stress_coordinator.py --stage a
# and
#   python -m tests.gui.part3.stress_coordinator --stage a
# resolve the package; the plan names the first form as the Stage A/B command.
from tests.gui.part3.evidence import (  # noqa: E402
    ALPHA_OBJECT,
    ALPHA_PROPERTY,
    BETA_OBJECT,
    BETA_PROPERTY,
    _paused_read_revisions_are_exact,
    LOCK_ANCHOR_SUFFIX,
    SHUTDOWN_TIMESTAMP_KEYS,
    archive_has_document_xml,
    binary_fingerprint,
    empty_evidence,
    empty_shutdown_record,
    finalize_evidence,
    freecad_binary_paths,
    git_state,
    new_cycle_record,
    print_verdict_line,
    record_check,
    record_conflict,
    record_cycle,
    record_pause_resume,
    record_save,
    scan_artifacts,
    session_ttl_provenance,
    sha256_file,
    shutdown_transitions_are_complete_and_ordered,
    stage_revision_vector_is_exact,
    stamp_shutdown_transition,
    utc_now_iso,
    verdict_from_checks,
    write_evidence,
)
from tests.gui.part3.local_user_driver import (  # noqa: E402
    TOKEN_ENV,
    LocalUserDriver,
    generate_control_token,
    launch_env_for_isolated_profile,
    wait_for_endpoint,
)
from tests.gui.part3.rpc_session_client import authenticate_json_rpc  # noqa: E402
from tests.gui.part3.scenarios import (  # noqa: E402
    COVERAGE_ITEMS,
    StageDefinition,
    resolve_executable_stage,
)

LAUNCHER = REPO_ROOT / "start_freecad.py"
LAUNCHER_IMPL = REPO_ROOT / "tools" / "launcher" / "start_freecad_impl.py"
REMOTE_AGENT_DRIVER = Path(__file__).resolve().parent / "remote_agent_driver.py"
DEFAULT_FREECAD = REPO_ROOT / "build" / "release" / "bin" / "FreeCAD.exe"
SHUTDOWN_DEADLINE_SECONDS = 60
DOCUMENT_READY_BEFORE_FIRST_SAVE_TIMEOUT_SECONDS = 120.0
DOCUMENT_READINESS_POLL_INTERVAL_SECONDS = 0.010
_DOCUMENT_READINESS_POLL_EVENT = threading.Event()
PERSONAL_STATE_ACTIONS = frozenset(
    {
        "set_active_document",
        "rotate_camera",
        "pan_view",
        "zoom_view",
        "fit_all",
        "select_object",
        "expand_tree",
        "collapse_tree",
        "clear_selection",
        "reset_property_editor",
    }
)


def default_freecad_exe(repo_root: Path | None = None) -> Path | None:
    root = (repo_root or REPO_ROOT).resolve()
    windows = sys.platform == "win32"

    def usable(candidate: Path) -> bool:
        if windows:
            return candidate.suffix.lower() == ".exe" and candidate.is_file()
        return candidate.suffix.lower() != ".exe" and candidate.is_file()

    for name in ("FREECAD", "FC_FREECAD", "FREECAD_EXE"):
        value = os.environ.get(name, "").strip()
        if value:
            override = Path(value).expanduser().resolve()
            if usable(override):
                return override

    executable_name = "FreeCAD.exe" if windows else "FreeCAD"
    for build_type in ("release", "debug"):
        candidate = root / "build" / build_type / "bin" / executable_name
        if usable(candidate):
            return candidate
    return None


USER_APPDATA = Path(os.environ.get("APPDATA", ""))


def _load_launcher_module():
    spec = importlib.util.spec_from_file_location("start_freecad_impl", LAUNCHER_IMPL)
    if spec is None or spec.loader is None:
        raise RuntimeError(f"cannot import launcher from {LAUNCHER_IMPL}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def _port_is_open(host: str, port: int) -> bool:
    with contextlib.closing(socket.socket(socket.AF_INET, socket.SOCK_STREAM)) as sock:
        sock.settimeout(0.75)
        return sock.connect_ex((host, port)) == 0


def _windows_owned_pid_inventory(
    root_pid: int, *, exact_pids: list[int] | None = None
) -> dict[str, Any]:
    """Return a bounded, fail-closed inventory of owned or exact PIDs."""

    if exact_pids is not None:
        if not exact_pids or not all(type(pid) is int and pid > 0 for pid in exact_pids):
            return {"complete": False, "existing": [], "queried_pids": [], "diagnostics": "invalid exact PID query", "timed_out": False}
        targets = ",".join(str(pid) for pid in sorted(set(exact_pids)))
        script = (
            "$ErrorActionPreference='Stop'; "
            f"$targets=@({targets}); "
            "$all=Get-CimInstance Win32_Process | Select-Object ProcessId; "
            "$found=@($all | Where-Object {$targets -contains [int]$_.ProcessId} | "
            "ForEach-Object {[int]$_.ProcessId}); "
            "if($found.Count -eq 0){Write-Output '[]'}else{$found | ConvertTo-Json -Compress}"
        )
    else:
        script = (
        "$ErrorActionPreference='Stop'; "
        f"$root={int(root_pid)}; "
        "$all=Get-CimInstance Win32_Process | Select-Object ProcessId,ParentProcessId; "
        "$rootProcess=$all | Where-Object {$_.ProcessId -eq $root}; "
        "if($null -eq $rootProcess){ Write-Output '[]'; exit 0 }; "
        "$seen=New-Object 'System.Collections.Generic.HashSet[int]'; "
        "$todo=New-Object 'System.Collections.Generic.Queue[int]'; $todo.Enqueue($root); "
        "while($todo.Count){$currentPid=$todo.Dequeue(); if($seen.Add($currentPid)){"
        "$all | Where-Object {$_.ParentProcessId -eq $currentPid} | ForEach-Object {$todo.Enqueue([int]$_.ProcessId)}}}; "
            "$seen | ConvertTo-Json -Compress"
        )
    try:
        completed = subprocess.run(
            ["powershell", "-NoProfile", "-NonInteractive", "-Command", script],
            capture_output=True, text=True, check=False, timeout=10.0,
        )
    except subprocess.TimeoutExpired as exc:
        return {"complete": False, "existing": [], "queried_pids": list(exact_pids or []), "diagnostics": str(exc), "timed_out": True}
    if completed.returncode != 0 or completed.stderr.strip():
        return {"complete": False, "existing": [], "queried_pids": list(exact_pids or []), "diagnostics": completed.stderr.strip() or completed.stdout.strip(), "timed_out": False}
    try:
        parsed = json.loads(completed.stdout)
        pids = parsed if isinstance(parsed, list) else [parsed]
        if not all(type(pid) is int and pid > 0 for pid in pids):
            raise ValueError("invalid PID inventory")
    except (json.JSONDecodeError, ValueError) as exc:
        return {"complete": False, "existing": [], "queried_pids": list(exact_pids or []), "diagnostics": str(exc), "timed_out": False}
    return {"complete": True, "existing": sorted(set(pids)), "queried_pids": sorted(set(exact_pids or pids)), "diagnostics": "", "timed_out": False}


def _posix_owned_process_group_inventory(process_group_id: int) -> dict[str, Any]:
    """Return every process in the coordinator-created POSIX process group."""

    if type(process_group_id) is not int or process_group_id <= 0:
        return {
            "complete": False,
            "existing": [],
            "process_group_id": process_group_id,
            "diagnostics": "invalid process group id",
            "timed_out": False,
        }
    command = ["ps", "-eo", "pid=,pgid="]
    try:
        completed = subprocess.run(
            command, capture_output=True, text=True, check=False, timeout=10.0
        )
    except subprocess.TimeoutExpired as exc:
        return {
            "complete": False,
            "existing": [],
            "process_group_id": process_group_id,
            "diagnostics": str(exc),
            "timed_out": True,
        }
    if completed.returncode != 0 or completed.stderr.strip():
        return {
            "complete": False,
            "existing": [],
            "process_group_id": process_group_id,
            "diagnostics": completed.stderr.strip() or completed.stdout.strip(),
            "timed_out": False,
        }
    try:
        pairs = [tuple(int(value) for value in line.split()) for line in completed.stdout.splitlines()]
        if any(len(pair) != 2 or pair[0] <= 0 or pair[1] <= 0 for pair in pairs):
            raise ValueError("invalid ps PID/PGID inventory")
    except ValueError as exc:
        return {
            "complete": False,
            "existing": [],
            "process_group_id": process_group_id,
            "diagnostics": str(exc),
            "timed_out": False,
        }
    return {
        "complete": True,
        "existing": sorted(pid for pid, pgid in pairs if pgid == process_group_id),
        "process_group_id": process_group_id,
        "diagnostics": "",
        "timed_out": False,
    }


def _force_kill_owned_process_tree(process: subprocess.Popen) -> dict[str, Any]:
    """Bounded last resort kill with exact owned-tree verification."""

    if os.name == "nt":
        if process.poll() is not None:
            return {"passed": True, "initial": {"complete": True, "existing": []}, "aftermath": {"complete": True, "existing": []}}
        initial = _windows_owned_pid_inventory(process.pid)
        if not initial["complete"]:
            return {"passed": False, "initial": initial, "aftermath": {"complete": False, "existing": [], "diagnostics": "initial inventory incomplete"}}
        try:
            terminated = subprocess.run(
                ["taskkill", "/PID", str(process.pid), "/T", "/F"],
                capture_output=True, text=True, check=False, timeout=10.0,
            )
            termination = {"return_code": terminated.returncode, "stdout": terminated.stdout, "stderr": terminated.stderr, "timed_out": False}
        except subprocess.TimeoutExpired as exc:
            termination = {"return_code": None, "stdout": "", "stderr": str(exc), "timed_out": True}
        aftermath = _windows_owned_pid_inventory(
            process.pid, exact_pids=initial["existing"]
        )
        passed = bool(
            not termination["timed_out"]
            and termination["return_code"] == 0
            and aftermath["complete"]
            and not aftermath["existing"]
            and process.poll() is not None
        )
        return {"passed": passed, "initial": initial, "termination": termination, "aftermath": aftermath}

    # ``launch_freecad`` starts the launcher as the leader of a new session,
    # so the launcher, pixi (when present), FreeCAD, and descendants inherit
    # this exact process-group id. Killing only the launcher lets FreeCAD be
    # reparented to PID 1; operating on the isolated group remains exact even
    # after the group leader exits.
    process_group_id = int(process.pid)
    initial = _posix_owned_process_group_inventory(process_group_id)
    if not initial["complete"]:
        return {
            "passed": False,
            "initial": initial,
            "aftermath": {
                "complete": False,
                "existing": [],
                "diagnostics": "initial process-group inventory incomplete",
            },
        }
    if not initial["existing"]:
        exited = process.poll() is not None
        return {
            "passed": exited,
            "initial": initial,
            "aftermath": {
                "complete": True,
                "existing": [],
                "diagnostics": "" if exited else "live launcher is not its process-group leader",
            },
        }

    termination: dict[str, Any] = {
        "process_group_id": process_group_id,
        "term_sent": False,
        "kill_sent": False,
        "diagnostics": "",
    }
    try:
        os.killpg(process_group_id, signal.SIGTERM)
        termination["term_sent"] = True
    except OSError as exc:
        termination["diagnostics"] = str(exc)

    deadline = monotonic() + 10.0
    aftermath = initial
    while termination["term_sent"] and monotonic() < deadline:
        process.poll()
        aftermath = _posix_owned_process_group_inventory(process_group_id)
        if not aftermath["complete"] or not aftermath["existing"]:
            break
        sleep(0.05)

    if aftermath.get("existing"):
        try:
            os.killpg(process_group_id, signal.SIGKILL)
            termination["kill_sent"] = True
        except OSError as exc:
            termination["diagnostics"] = str(exc)
        deadline = monotonic() + 5.0
        while termination["kill_sent"] and monotonic() < deadline:
            process.poll()
            aftermath = _posix_owned_process_group_inventory(process_group_id)
            if not aftermath["complete"] or not aftermath["existing"]:
                break
            sleep(0.05)

    process.poll()
    passed = bool(
        aftermath["complete"]
        and not aftermath["existing"]
        and process.poll() is not None
    )
    return {
        "passed": passed,
        "initial": initial,
        "termination": termination,
        "aftermath": aftermath,
    }


def _wait_for_process_exit(process: subprocess.Popen, deadline_s: float) -> bool:
    deadline = time_monotonic() + deadline_s
    while time_monotonic() < deadline:
        if process.poll() is not None:
            return True
    return process.poll() is not None


def _document_names_from_list_documents(payload: object) -> list[str]:
    if isinstance(payload, list):
        names: list[str] = []
        for item in payload:
            if isinstance(item, str):
                names.append(item)
            elif isinstance(item, dict):
                name = item.get("name") or item.get("Name") or item.get("document")
                if name:
                    names.append(str(name))
        return names
    if isinstance(payload, dict):
        documents = payload.get("documents")
        if isinstance(documents, list):
            return _document_names_from_list_documents(documents)
    return []


def _load_shutdown_payload(evidence_path: Path | None) -> dict[str, Any]:
    shutdown = empty_shutdown_record()
    if evidence_path is None or not evidence_path.is_file():
        return shutdown
    payload = json.loads(evidence_path.read_text(encoding="utf-8"))
    existing = payload.get("shutdown")
    if isinstance(existing, dict):
        shutdown.update(existing)
    return shutdown


def _persist_shutdown_evidence(
    evidence_path: Path | None,
    shutdown: dict[str, Any],
    *,
    verdict: str | None = None,
) -> None:
    if evidence_path is None or not evidence_path.is_file():
        return
    payload = json.loads(evidence_path.read_text(encoding="utf-8"))
    payload["shutdown"] = shutdown
    if verdict is not None:
        payload = finalize_evidence(payload, verdict=verdict)
    write_evidence(evidence_path, payload)


def _stamp_evidence_mode(evidence_path: Path, mode: str) -> None:
    """Mark which kind of run produced this envelope (GRK-P3-077).

    A preflight-only artifact carries ``stage: null`` and ``mode:
    "preflight_only"`` so it can never be mistaken for a Stage A/B result.
    """

    if not evidence_path.is_file():
        return
    payload = json.loads(evidence_path.read_text(encoding="utf-8"))
    payload["mode"] = mode
    write_evidence(evidence_path, payload)


def graceful_shutdown_owned_session(
    *,
    process: subprocess.Popen,
    profile_root: Path,
    launcher_module: Any,
    repo_root: Path,
    mcp_port: int,
    local_driver: LocalUserDriver | None = None,
    evidence_path: Path | None = None,
    deadline_seconds: int = SHUTDOWN_DEADLINE_SECONDS,
    skip_window_close: bool = False,
    skip_rpc_shutdown: bool = False,
    skip_document_close: bool = False,
    success_verdict: str = "PASSED",
) -> dict[str, Any]:
    """ADR §9 ordered shutdown; force-kill only after stall with forced:true.

    ``success_verdict`` is the verdict stamped when the owned process exits
    inside the deadline. A stage that already failed its own checks passes
    ``FAILED`` so a clean teardown can never upgrade a failed stage, and a
    forced kill always stamps ``FAILED`` regardless of it.
    """

    shutdown = _load_shutdown_payload(evidence_path)
    shutdown["deadline_seconds"] = int(deadline_seconds)
    shutdown["forced"] = False
    shutdown["stalled_stage"] = None
    stamp_shutdown_transition(shutdown, "requested_utc")
    _persist_shutdown_evidence(evidence_path, shutdown)

    stalled_stage: str | None = None
    rpc = None
    # Which step of the RPC phase is in progress. Any failure in this block
    # used to be recorded as ``stalled_stage: "rpc_shutdown"`` - a false
    # statement in the acceptance evidence whenever the phase stopped before
    # the shutdown verb was ever sent, and the statement that misdirected the
    # first triage of GRK-P3-096. Both ``stalled_stage`` and ``failed_step``
    # now name the step the phase really stopped on (GRK-P3-100). WP09's
    # forced-kill gate passes skip_window_close, skip_rpc_shutdown AND
    # skip_document_close, so it is satisfied by the skip branches below and
    # never enters this ``except``; its accepted set is untouched.
    active_step = "rpc_connect"
    try:
        base_client = launcher_module.JsonRpcClient(host="127.0.0.1", port=mcp_port)
        rpc = authenticate_json_rpc(
            base_client,
            profile_root,
            json_rpc_error=launcher_module.JsonRpcError,
            json_rpc_transport_error=launcher_module.JsonRpcTransportError,
        )

        active_step = "document_close"
        documents = _document_names_from_list_documents(
            rpc.call("list_documents", timeout=15.0)
        )
        if not skip_document_close:
            for document_name in documents:
                # JSON-RPC params must be a structured value. A bare string is
                # neither an array nor an object, so the framing layer answers
                # -32600 Invalid Request before dispatch runs at all, and the
                # ordered shutdown aborts on the first open document.
                rpc.call("close_document", [document_name], timeout=30.0)
        stamp_shutdown_transition(shutdown, "documents_closed_utc")
        _persist_shutdown_evidence(evidence_path, shutdown)

        active_step = "rpc_shutdown"
        if not skip_rpc_shutdown:
            rpc.call("shutdown_rpc_server", timeout=30.0)
            stamp_shutdown_transition(shutdown, "rpc_admission_closed_utc")
            stamp_shutdown_transition(shutdown, "worker_shutdown_utc")
            stamp_shutdown_transition(shutdown, "listener_shutdown_utc")
            _persist_shutdown_evidence(evidence_path, shutdown)
    except Exception as exc:
        stalled_stage = active_step
        shutdown["stalled_stage"] = stalled_stage
        shutdown["failed_step"] = active_step
        shutdown["rpc_error"] = str(exc)
        _persist_shutdown_evidence(evidence_path, shutdown)

    if stalled_stage is None and local_driver is not None and not skip_window_close:
        try:
            local_driver.invoke("close_main_window", timeout=15.0)
            stamp_shutdown_transition(shutdown, "window_closed_utc")
            _persist_shutdown_evidence(evidence_path, shutdown)
        except Exception as exc:
            stalled_stage = "window_close"
            shutdown["stalled_stage"] = stalled_stage
            shutdown["window_error"] = str(exc)
            _persist_shutdown_evidence(evidence_path, shutdown)
    elif stalled_stage is None and skip_window_close:
        stalled_stage = "window_close"
        shutdown["stalled_stage"] = stalled_stage
        shutdown["window_skipped"] = True
        _persist_shutdown_evidence(evidence_path, shutdown)
    elif stalled_stage is None and skip_rpc_shutdown:
        stalled_stage = "rpc_shutdown"
        shutdown["stalled_stage"] = stalled_stage
        shutdown["rpc_shutdown_skipped"] = True
        _persist_shutdown_evidence(evidence_path, shutdown)

    wait_deadline = float(deadline_seconds)
    if _wait_for_process_exit(process, wait_deadline):
        stamp_shutdown_transition(shutdown, "process_exit_utc")
        shutdown["forced"] = False
        shutdown["stalled_stage"] = None
        _persist_shutdown_evidence(evidence_path, shutdown, verdict=success_verdict)
        return {
            "success": True,
            "forced": False,
            "stalled_stage": None,
            "shutdown": shutdown,
        }

    if stalled_stage is None:
        stalled_stage = "process_exit"
    shutdown["stalled_stage"] = stalled_stage
    shutdown["forced"] = True
    _persist_shutdown_evidence(evidence_path, shutdown)
    cleanup = _force_kill_owned_process_tree(process)
    if not isinstance(cleanup, dict):
        # Compatibility for old test/embedding hooks: an unstructured result
        # is never proof of cleanup, but it must not crash teardown reporting.
        cleanup = {"passed": False, "diagnostics": "cleanup helper returned no structured result"}
    shutdown["forced_cleanup"] = cleanup
    if cleanup.get("passed") and process.poll() is not None:
        stamp_shutdown_transition(shutdown, "process_exit_utc")
    elif not cleanup.get("passed"):
        shutdown["forced_cleanup_failed"] = True
        shutdown["forced_cleanup_error"] = str(cleanup)
    _persist_shutdown_evidence(evidence_path, shutdown, verdict="FAILED")
    return {
        "success": False,
        "forced": True,
        "stalled_stage": stalled_stage,
        "shutdown": shutdown,
    }


@dataclass
class CoordinatorHandoff:
    """Actors after provisioning; the coordinator must not retain these for remote ops."""

    control_token: str
    local_driver: LocalUserDriver
    endpoint: dict[str, Any]


class StressCoordinator:
    """Provision isolated profile, launch FreeCAD, spawn actors, collect evidence hooks."""

    def __init__(
        self,
        *,
        repo_root: Path | None = None,
        run_root: Path | None = None,
    ) -> None:
        self.repo_root = (repo_root or REPO_ROOT).resolve()
        self.run_root = (run_root or Path(tempfile.mkdtemp(prefix="part3-stress-"))).resolve()
        self.profile_root = self.run_root / "profile"
        self.endpoint_dir = self.run_root / "control"
        self.evidence_dir = self.run_root / "evidence"
        self.evidence_path = self.evidence_dir / "evidence.json"
        self._launcher_module = _load_launcher_module()
        self._control_token: str | None = None
        self._launch_env: dict[str, str] | None = None
        self._launcher_process: subprocess.Popen | None = None
        self._handoff: CoordinatorHandoff | None = None
        self._rpc_session: Any | None = None

    @property
    def launcher_path(self) -> Path:
        return (self.repo_root / "start_freecad.py").resolve()

    @property
    def mcp_rpc_port(self) -> int:
        return int(self._launcher_module.MCP_RPC_PORT)

    def holds_control_token(self) -> bool:
        return self._control_token is not None

    def holds_rpc_session(self) -> bool:
        """True while this process retains an authenticated typed RPC session.

        ADR §1/§1.1 place that session in the RemoteAgentDriver child. The
        P3-WP10 Stage A/B path issues its typed calls from this process instead
        (the deviation recorded in ADR §1.4), so this predicate reports the state
        the coordinator actually has rather than the contract's ideal one.
        """

        return self._rpc_session is not None

    def assert_tracked_launcher(self) -> None:
        if not self.launcher_path.is_file():
            raise FileNotFoundError(f"tracked launcher missing: {self.launcher_path}")
        if self.launcher_path.name != "start_freecad.py":
            raise RuntimeError("FreeCAD must launch only through start_freecad.py")

    def assert_port_free(self, host: str = "127.0.0.1", port: int | None = None) -> None:
        rpc_port = self.mcp_rpc_port if port is None else port
        if _port_is_open(host, rpc_port):
            raise RuntimeError(
                f"refusing to attach: MCP port {host}:{rpc_port} is already occupied"
            )

    def assert_isolated_profile(self) -> None:
        profile = self.profile_root.resolve()
        if USER_APPDATA and profile == USER_APPDATA.resolve():
            raise RuntimeError("refusing to use the normal user APPDATA profile")
        mod_dir = profile / "FreeCAD" / "Mod" / "Part3LocalDriver"
        if mod_dir.is_dir() and not str(mod_dir).startswith(str(profile)):
            raise RuntimeError("Part3LocalDriver must install only under the isolated profile")

    def provision(self) -> str:
        """Create isolated profile layout, auth secret, control token, evidence dir."""

        self.assert_tracked_launcher()
        self.assert_port_free()
        self.profile_root.mkdir(parents=True, exist_ok=True)
        self.endpoint_dir.mkdir(parents=True, exist_ok=True)
        self.evidence_dir.mkdir(parents=True, exist_ok=True)
        token = generate_control_token()
        self._control_token = token
        self._launch_env = launch_env_for_isolated_profile(
            self.profile_root,
            control_token=token,
            endpoint_dir=self.endpoint_dir,
            repo_root=self.repo_root,
        )
        self.assert_isolated_profile()
        evidence = empty_evidence(stage=None)
        evidence["started_utc"] = utc_now_iso()
        evidence["environment"].update(
            {
                "launcher": str(self.launcher_path),
                "python": sys.executable,
                "isolated_profile": str(self.profile_root),
                "isolated_mod_dir": str(self.profile_root / "FreeCAD" / "Mod"),
                "mcp_host": "127.0.0.1",
                "mcp_port": self.mcp_rpc_port,
            }
        )
        write_evidence(self.evidence_path, evidence)
        return token

    def build_launch_command(self, freecad_exe: Path) -> list[str]:
        self.assert_tracked_launcher()
        return [
            sys.executable,
            str(self.launcher_path),
            "--force-new",
            "--freecad",
            str(freecad_exe),
            "--mcp-timeout",
            "120",
            "--wait",
        ]

    def launch_env(self) -> dict[str, str]:
        if self._launch_env is None:
            raise RuntimeError("call provision() before launch_env()")
        return dict(self._launch_env)

    def launch_freecad(self, freecad_exe: Path | None = None) -> subprocess.Popen:
        if self._launch_env is None or self._control_token is None:
            raise RuntimeError("call provision() before launch_freecad()")
        exe = freecad_exe or default_freecad_exe(self.repo_root)
        if exe is None or not exe.is_file():
            raise FileNotFoundError(
                "FreeCAD GUI binary not found via launcher overrides or "
                "under build/release or build/debug"
            )
        self.assert_port_free()
        launcher_log = self.run_root / "launcher.log"
        command = self.build_launch_command(exe)
        with launcher_log.open("wb") as log_handle:
            process = subprocess.Popen(
                command,
                cwd=str(self.repo_root),
                env=self.launch_env(),
                stdout=log_handle,
                stderr=subprocess.STDOUT,
                start_new_session=os.name != "nt",
            )
        self._launcher_process = process
        return process

    def wait_for_launcher_ready(self, timeout_s: float = 120.0) -> None:
        if self._launcher_process is None:
            raise RuntimeError("call launch_freecad() first")
        deadline = time_monotonic() + timeout_s
        while time_monotonic() < deadline:
            if self._launcher_process.poll() is not None:
                raise RuntimeError("launcher exited before MCP became ready")
            if _port_is_open("127.0.0.1", self.mcp_rpc_port):
                try:
                    if self._launcher_module.JsonRpcClient().call("ping", timeout=2.0):
                        return
                except Exception:
                    pass
        raise TimeoutError(
            f"MCP RPC did not become ready on 127.0.0.1:{self.mcp_rpc_port} within {timeout_s}s"
        )

    def connect_local_driver(self) -> CoordinatorHandoff:
        if self._control_token is None:
            raise RuntimeError("call provision() before connect_local_driver()")
        endpoint = wait_for_endpoint(self.endpoint_dir)
        local_driver = LocalUserDriver(self._control_token, endpoint)
        handoff = CoordinatorHandoff(
            control_token=self._control_token,
            local_driver=local_driver,
            endpoint=endpoint,
        )
        self._handoff = handoff
        self._control_token = None
        return handoff

    def run_preflight(self, handoff: CoordinatorHandoff) -> dict[str, Any]:
        return handoff.local_driver.preflight()

    def authenticate_typed_session(self, *, authenticator: Any = None) -> Any:
        """Create and retain the authenticated typed RPC session (ADR §1.4).

        ``authenticator`` exists so the boundary predicate can be exercised
        offline; the stage path always uses the real handshake.
        """

        authenticate = authenticator or authenticate_json_rpc
        session = authenticate(
            self._launcher_module.JsonRpcClient(),
            self.profile_root,
            json_rpc_error=self._launcher_module.JsonRpcError,
            json_rpc_transport_error=self._launcher_module.JsonRpcTransportError,
        )
        self._rpc_session = session
        return session

    def release_typed_session(self) -> None:
        """Drop the retained session so the predicate stops claiming to hold one."""

        self._rpc_session = None

    def spawn_remote_agent_child(
        self,
        *,
        inspect_only: bool = True,
        parent_env: dict[str, str] | None = None,
    ) -> subprocess.CompletedProcess[str]:
        """Spawn RemoteAgentDriver without leaking the control token to the child."""

        env = dict(parent_env or os.environ)
        if self._handoff is not None:
            token = self._handoff.control_token
        elif self._control_token is not None:
            token = self._control_token
        else:
            token = env.get(TOKEN_ENV, "")
        env.pop(TOKEN_ENV, None)
        if token:
            for key, value in list(env.items()):
                if token in value:
                    env[key] = value.replace(token, "")
        argv = [
            sys.executable,
            str(REMOTE_AGENT_DRIVER),
        ]
        if inspect_only:
            argv.append("--inspect-token-absence")
        # The child checks only a fixed non-secret sentinel. Passing the real
        # bearer on stdin made the absence proof itself a token disclosure.
        completed = subprocess.run(
            argv,
            input=(
                json.dumps({"token": TOKEN_ABSENCE_SENTINEL})
                if inspect_only
                else None
            ),
            env=env,
            capture_output=True,
            text=True,
            check=False,
            timeout=30.0,
        )
        return completed

    def record_preflight_check(self, preflight: dict[str, Any]) -> None:
        """Record the local-driver preflight as a real check (GRK-P3-082).

        The result is derived from what ``local_driver/actions.py::_preflight``
        actually returns - ``pause_checkbox_wired`` is True and
        ``pause_checkbox_visible`` was observed as a boolean - and never from a
        ``ready`` key no driver produces. It is recorded through
        ``record_check`` so a failure lands in ``failed_checks`` and can decide
        the verdict, instead of being appended to ``checks`` where nothing
        reads it.
        """

        payload = json.loads(self.evidence_path.read_text(encoding="utf-8"))
        wired = preflight.get("pause_checkbox_wired") is True
        visibility_observed = isinstance(preflight.get("pause_checkbox_visible"), bool)
        record_check(
            payload,
            "local_driver_preflight",
            wired and visibility_observed,
            preflight,
        )
        payload["environment"]["control_endpoint"] = self._handoff.endpoint if self._handoff else {}
        write_evidence(self.evidence_path, payload)

    def shutdown_launcher(self, *, success_verdict: str = "PASSED") -> dict[str, Any]:
        """Run ADR §9 shutdown; success is True only when forced:false."""

        if self._launcher_process is None:
            return {"success": True, "forced": False, "stalled_stage": None, "shutdown": {}}
        process = self._launcher_process
        local_driver = self._handoff.local_driver if self._handoff is not None else None
        result = graceful_shutdown_owned_session(
            process=process,
            profile_root=self.profile_root,
            launcher_module=self._launcher_module,
            repo_root=self.repo_root,
            mcp_port=self.mcp_rpc_port,
            local_driver=local_driver,
            evidence_path=self.evidence_path,
            deadline_seconds=SHUTDOWN_DEADLINE_SECONDS,
            success_verdict=success_verdict,
        )
        self._launcher_process = None
        self.release_typed_session()
        return result


def time_monotonic() -> float:
    import time

    return time.monotonic()


HISTORY_HEAD_PROBE = "part3-history-head-probe"
HISTORY_HEAD_REJECTED = "HISTORY_HEAD_REJECTED"


REMOTE_ACTOR_MODE = "in_process_typed_session"
TOKEN_ABSENCE_SENTINEL = "part3-child-token-absence-sentinel"


def _remote_actor_record(*, child_token_absence_proved: bool) -> dict[str, Any]:
    """Report the actor boundary the stage path actually has (GRK-P3-076).

    ADR §1/§1.1 put the authenticated typed session in the RemoteAgentDriver
    child process. The P3-WP10 stage path issues its typed calls from the
    coordinator process instead; the child is still spawned and still proves the
    local control token never reaches it. The deviation is written into the
    evidence so no reader of evidence.json can mistake this run for the ADR's
    literal actor split. It is recorded as a decision in ADR §1.4.
    """

    return {
        "mode": REMOTE_ACTOR_MODE,
        "child_token_absence_proved": bool(child_token_absence_proved),
        "adr_deviation": "section 1.1",
        "adr_deviation_recorded_in": (
            "doc/part3-gui-collaboration-stress-design.md section 1.4"
        ),
    }


class StageCheckFailed(RuntimeError):
    """One Stage A/B acceptance check failed; the stage stops and reports FAILED."""


@dataclass
class StageContext:
    """Live actors and evidence for one Stage A/B program."""

    definition: StageDefinition
    payload: dict[str, Any]
    evidence_path: Path
    rpc: Any
    local: LocalUserDriver
    launcher_module: Any
    documents_dir: Path
    copies_dir: Path
    primary: str
    secondary: str
    document_paths: dict[str, Path] = field(default_factory=dict)
    coverage: set[str] = field(default_factory=set)
    last_view: dict[str, Any] = field(default_factory=dict)
    active_save_record: dict[str, Any] | None = None
    active_document: str | None = None


def _persist(context: StageContext) -> None:
    write_evidence(context.evidence_path, context.payload)


RECORDED_SCENARIO_CHECKS = frozenset({
    "head_bound_undo_reverts_the_local_transaction",
    "redo_stack_records_the_reverted_transaction",
    "head_bound_redo_restores_the_reverted_transaction",
    "remote_write_refused_while_paused",
    "reads_remain_available_while_paused",
    "next_typed_mutation_succeeds_after_resume",
})


def _require(
    context: StageContext,
    name: str,
    passed: bool,
    detail: Any = None,
) -> None:
    """Raise on failure and retain named scenario passes required by the packet."""

    if passed:
        if name in RECORDED_SCENARIO_CHECKS:
            record_check(context.payload, name, True, detail)
        return
    record_check(context.payload, name, False, detail)
    _persist(context)
    raise StageCheckFailed(f"{name}: {detail!r}")


def _rpc_failure_payload(exc: Any) -> dict[str, Any]:
    data = getattr(exc, "data", None)
    data = data if isinstance(data, dict) else {}
    return {
        "success": False,
        "error": str(exc),
        "error_code": data.get("error_code"),
        "data": data,
    }


def _call_expecting_failure(
    context: StageContext,
    method: str,
    params: Any,
    *,
    timeout: float = 60.0,
) -> dict[str, Any]:
    """Call a typed RPC that is expected to be refused; normalise the refusal."""

    try:
        result = context.rpc.call(method, params, timeout=timeout)
    except context.launcher_module.JsonRpcError as exc:
        return _rpc_failure_payload(exc)
    except context.launcher_module.JsonRpcTransportError as exc:
        return _rpc_failure_payload(exc)
    if isinstance(result, dict) and result.get("success") is False:
        return {
            "success": False,
            "error": str(result.get("error") or ""),
            "error_code": result.get("error_code"),
            "data": result,
        }
    return {"success": True, "result": result}


def _view_state(
    context: StageContext, document_name: str | None = None
) -> dict[str, Any]:
    if document_name:
        response = context.local.invoke(
            "view_state", {"document": document_name}, timeout=30.0
        )
    else:
        response = context.local.invoke("view_state", timeout=30.0)
    result = response.get("result")
    return result if isinstance(result, dict) else {}


def _local_action(
    context: StageContext,
    cycle: dict[str, Any] | None,
    action: str,
    params: dict[str, Any] | None = None,
    *,
    coverage: str | None = None,
    timeout: float = 30.0,
    observe_view: bool = True,
) -> dict[str, Any]:
    """Drive one local action; classified personal actions carry exact proof."""

    action_params = params or {}
    personal_documents: list[str] = []
    personal_before: dict[str, Any] = {}
    left_document = context.active_document
    if action in PERSONAL_STATE_ACTIONS:
        personal_documents = _personal_action_documents(context, action, action_params)
        for document_name in personal_documents:
            _ensure_document_clean_for_personal_view(context, document_name)
        personal_before = {
            document_name: _personal_document_snapshot(context, document_name)
            for document_name in personal_documents
        }
    operation_id = str(uuid.uuid4())
    response = context.local.invoke(
        action,
        action_params,
        operation_id=operation_id,
        timeout=timeout,
    )
    entry: dict[str, Any] = {
        "operation_id": operation_id,
        "action": action,
        "parameters": dict(action_params),
        "ack_utc": utc_now_iso(),
        "observed": response.get("result"),
    }
    if observe_view:
        after = _view_state(context)
        entry["view_state_changed"] = after != context.last_view
        context.last_view = after
    if coverage:
        context.coverage.add(coverage)
    if cycle is not None:
        cycle["local_actions"].append(entry)
    else:
        out_of_cycle = context.payload.setdefault("out_of_cycle_local_actions", [])
        entry["out_of_cycle_index"] = len(out_of_cycle)
        out_of_cycle.append(entry)
    if action in PERSONAL_STATE_ACTIONS:
        if action == "set_active_document":
            context.active_document = str(action_params["document"])
        personal_after = {
            document_name: _personal_document_snapshot(context, document_name)
            for document_name in personal_documents
        }
        clean_before = all(
            _file_change_state_is_clean(snapshot.get("file_change_state"))
            for snapshot in personal_before.values()
        )
        clean_after = all(
            _file_change_state_is_clean(snapshot.get("file_change_state"))
            for snapshot in personal_after.values()
        )
        revisions_unchanged = all(
            personal_before[document_name].get("semantic_revisions")
            == personal_after[document_name].get("semantic_revisions")
            for document_name in personal_documents
        )
        proof = {
            "index": len(context.payload.setdefault("personal_action_proofs", [])),
            "operation_id": operation_id,
            "action": action,
            "documents": personal_documents,
            "left_document": left_document if action == "set_active_document" else None,
            "activated_document": (
                context.active_document if action == "set_active_document" else None
            ),
            "before": personal_before,
            "after": personal_after,
            "clean_before": clean_before,
            "clean_after": clean_after,
            "semantic_revisions_unchanged": revisions_unchanged,
            "passed": bool(clean_before and clean_after and revisions_unchanged),
        }
        context.payload["personal_action_proofs"].append(proof)
        entry["personal_action_proof_index"] = proof["index"]
        _require(
            context,
            "personal_action_has_exact_clean_revision_proof",
            bool(proof["passed"]),
            proof,
        )
    return entry


def _remote_action(
    context: StageContext,
    cycle: dict[str, Any] | None,
    method: str,
    params: Any,
    *,
    coverage: str | None = None,
    timeout: float = 60.0,
    operation_id: str | None = None,
    committed_once: bool | None = None,
) -> Any:
    """Call one typed JSON-RPC verb and record its result envelope."""

    result = context.rpc.call(method, params, timeout=timeout)
    entry: dict[str, Any] = {
        "operation_id": operation_id,
        "method": method,
        "parameters": params,
        "ack_utc": utc_now_iso(),
        "result_envelope": result,
    }
    if committed_once is not None:
        entry["committed_once"] = bool(committed_once)
    if coverage:
        context.coverage.add(coverage)
    if cycle is not None:
        cycle["remote_actions"].append(entry)
    return result


def _identity_selector_is_exact(selector: object, document_name: str) -> bool:
    if not isinstance(selector, dict):
        return False
    instance_id = selector.get("document_instance_id")
    lifecycle_epoch = selector.get("lifecycle_epoch")
    return (
        isinstance(selector.get("document_uid"), str)
        and bool(selector.get("document_uid"))
        and isinstance(instance_id, int)
        and not isinstance(instance_id, bool)
        and isinstance(lifecycle_epoch, int)
        and not isinstance(lifecycle_epoch, bool)
        and selector.get("document_name") == document_name
    )


def _identity_selector(context: StageContext, document_name: str) -> dict[str, Any]:
    state = _view_state(context, document_name)
    context.last_view = state
    _require(
        context,
        "identity_selector_observed_document",
        state.get("observed_document") == document_name,
        {"expected": document_name, "state": state},
    )
    selector = state.get("identity_selector")
    _require(
        context,
        "identity_selector_present",
        _identity_selector_is_exact(selector, document_name),
        selector,
    )
    return selector


def _file_change_state(context: StageContext, document_name: str) -> dict[str, Any]:
    state = _view_state(context, document_name)
    context.last_view = state
    _require(
        context,
        "file_change_state_observed_document",
        state.get("observed_document") == document_name,
        {"expected": document_name, "state": state},
    )
    file_state = state.get("file_change_state")
    return file_state if isinstance(file_state, dict) else {}


def _property_key(object_name: str, property_name: str) -> dict[str, str]:
    return {
        "kind": "ObjectProperty",
        "subject": object_name,
        "property_name": property_name,
    }


def _stage_revision_keys() -> list[dict[str, str]]:
    return [
        _property_key(ALPHA_OBJECT, ALPHA_PROPERTY),
        _property_key(BETA_OBJECT, BETA_PROPERTY),
        {"kind": "ObjectModel", "subject": ALPHA_OBJECT},
        {"kind": "ObjectModel", "subject": BETA_OBJECT},
    ]


def _semantic_revisions(
    context: StageContext,
    selector: dict[str, Any],
    keys: list[dict[str, str]],
) -> list[dict[str, Any]]:
    result = context.rpc.call(
        "get_semantic_revisions",
        {"doc_selector": selector, "revision_keys": keys},
        timeout=30.0,
    )
    revisions = result.get("revisions") if isinstance(result, dict) else None
    _require(
        context,
        "semantic_revisions_readable",
        isinstance(revisions, list) and bool(revisions),
        result,
    )
    return list(revisions)


def _property_revision(
    context: StageContext,
    selector: dict[str, Any],
    object_name: str,
    property_name: str,
) -> int:
    revisions = _semantic_revisions(
        context,
        selector,
        [_property_key(object_name, property_name)],
    )
    return int(revisions[0]["revision"])


def _property_value(
    context: StageContext,
    document_name: str,
    object_name: str,
    property_name: str,
) -> int:
    payload = context.rpc.call(
        "get_object",
        {"doc_name": document_name, "obj_name": object_name},
        timeout=30.0,
    )
    properties = {}
    if isinstance(payload, dict):
        properties = payload.get("properties") or payload.get("Properties") or {}
    value = properties.get(property_name) if isinstance(properties, dict) else None
    if isinstance(value, dict) and "value" in value:
        return int(value["value"])
    return int(value)


def _readiness_flag(readiness: dict[str, Any], field_name: str) -> Any:
    value = readiness.get(field_name)
    if value is not None:
        return value
    documents = readiness.get("documents")
    if isinstance(documents, list) and documents:
        first = documents[0]
        if isinstance(first, dict):
            return first.get(field_name)
    return None


def _mutation_readiness(
    context: StageContext,
    document_name: str,
    *,
    timeout_seconds: float = 30.0,
) -> dict[str, Any]:
    readiness = context.rpc.call(
        "get_mutation_readiness",
        {"doc_name": document_name},
        timeout=timeout_seconds,
    )
    return readiness if isinstance(readiness, dict) else {}


def _wait_for_document_ready_before_first_save(
    context: StageContext,
    document_name: str,
    *,
    timeout_seconds: float = DOCUMENT_READY_BEFORE_FIRST_SAVE_TIMEOUT_SECONDS,
) -> dict[str, Any]:
    """Wait boundedly for the typed mutation boundary before the canonical save."""

    deadline = monotonic() + timeout_seconds
    last_readiness: dict[str, Any] = {}
    while True:
        remaining = deadline - monotonic()
        if remaining <= 0:
            raise TimeoutError(
                "stage document did not become ready before first save within "
                f"{timeout_seconds}s: document={document_name!r}, "
                f"last_readiness={last_readiness!r}"
            )
        last_readiness = _mutation_readiness(
            context,
            document_name,
            timeout_seconds=min(30.0, remaining),
        )
        quarantined = _readiness_flag(last_readiness, "quarantined") is True
        poisoned = (
            _readiness_flag(last_readiness, "collaboration_poisoned") is True
        )
        if quarantined or poisoned:
            raise StageCheckFailed(
                "stage document cannot become ready before first save: "
                f"document={document_name!r}, readiness={last_readiness!r}"
            )
        if _readiness_flag(last_readiness, "ready") is True:
            return last_readiness
        remaining = deadline - monotonic()
        if remaining <= 0:
            raise TimeoutError(
                "stage document did not become ready before first save within "
                f"{timeout_seconds}s: document={document_name!r}, "
                f"last_readiness={last_readiness!r}"
            )
        _DOCUMENT_READINESS_POLL_EVENT.wait(
            min(DOCUMENT_READINESS_POLL_INTERVAL_SECONDS, remaining)
        )


def _begin_checked_edit(
    context: StageContext,
    cycle: dict[str, Any] | None,
    selector: dict[str, Any],
    revision_keys: list[dict[str, str]],
    capture: dict[str, Any] | None = None,
) -> str:
    operation_id = f"{uuid.uuid4()}-begin"
    begin = _remote_action(
        context,
        cycle,
        "begin_checked_edit",
        {
            "doc_selector": selector,
            "revision_keys": revision_keys,
            "operation_id": operation_id,
        },
        operation_id=operation_id,
    )
    _require(
        context,
        "begin_checked_edit_succeeds",
        isinstance(begin, dict) and begin.get("success") is True,
        begin,
    )
    if capture is not None:
        capture["begin"] = {
            "operation_id": operation_id,
            "method": "begin_checked_edit",
            "parameters": {
                "doc_selector": selector,
                "revision_keys": revision_keys,
                "operation_id": operation_id,
            },
            "result": begin,
        }
    return str(begin["session_id"])


def _commit_checked_integer(
    context: StageContext,
    cycle: dict[str, Any] | None,
    *,
    session_id: str,
    selector: dict[str, Any],
    object_name: str,
    property_name: str,
    value: int,
    prove_exactly_once: bool,
    capture: dict[str, Any] | None = None,
    capture_key: str = "commit_operation",
) -> dict[str, Any]:
    """Commit one typed integer property, optionally replaying the operation id."""

    operation_id = str(uuid.uuid4())
    commit_params = {
        "session_id": session_id,
        "doc_selector": selector,
        "object_name": object_name,
        "property_name": property_name,
        "value_type": "integer",
        "value": str(int(value)),
        "operation_id": operation_id,
    }
    first = _remote_action(
        context,
        cycle,
        "commit_checked_property",
        commit_params,
        coverage="typed_model_mutation",
        operation_id=operation_id,
    )
    _require(
        context,
        "typed_mutation_commits",
        isinstance(first, dict)
        and first.get("success") is True
        and first.get("committed") is True,
        first,
    )
    if capture is not None:
        capture[capture_key] = {
            "operation_id": operation_id,
            "method": "commit_checked_property",
            "parameters": commit_params,
            "result": first,
        }
    if not prove_exactly_once:
        return {"operation_id": operation_id, "first": first, "replay": None, "committed_once": None}
    replay = _remote_action(
        context,
        cycle,
        "commit_checked_property",
        commit_params,
        operation_id=operation_id,
        committed_once=True,
        timeout=60.0,
    )
    _require(
        context,
        "typed_mutation_replay_returns_stored_terminal",
        replay == first,
        {"first": first, "replay": replay},
    )
    if capture is not None:
        capture[f"{capture_key}_replay"] = {
            "operation_id": operation_id,
            "method": "commit_checked_property",
            "parameters": commit_params,
            "result": replay,
            "replay_of_operation_id": operation_id,
        }
    return {"operation_id": operation_id, "first": first, "replay": replay, "committed_once": True}


def _provision_stage_documents(context: StageContext) -> None:
    """Create both disposable documents through typed lifecycle plus local fixture."""

    for document_name in (context.primary, context.secondary):
        try:
            context.rpc.call("create_document", {"name": document_name}, timeout=60.0)
        except context.launcher_module.JsonRpcError:
            context.rpc.call("create_document", [document_name], timeout=60.0)
        context.active_document = document_name
        _local_action(
            context,
            None,
            "provision_alpha_beta_fixture",
            {"document": document_name, "alpha": 0, "beta": 0},
            timeout=120.0,
            observe_view=False,
        )
        readiness = _wait_for_document_ready_before_first_save(
            context, document_name
        )
        _require(
            context,
            "stage_document_ready_before_first_save",
            _readiness_flag(readiness, "ready") is True,
            {"document": document_name, "readiness": readiness},
        )
        destination = context.documents_dir / f"{document_name}.FCStd"
        context.document_paths[document_name] = destination
        saved = context.rpc.call(
            "save_document_as",
            {
                "selector": {"document_name": document_name},
                "destination": str(destination),
                "overwrite": True,
            },
            timeout=120.0,
        )
        _require(
            context,
            "stage_document_first_save",
            isinstance(saved, dict) and saved.get("saved") is True,
            saved,
        )
        _local_action(
            context,
            None,
            "set_active_document",
            {"document": document_name},
            coverage="active_view_switching",
            observe_view=False,
        )
    context.coverage.add("two_documents")


def _file_change_state_is_clean(state: object) -> bool:
    """ADR §4 exact clean observation, not equality of two dirty states."""

    return (
        isinstance(state, dict)
        and state.get("pending_changes") == []
        and state.get("has_pending_file_changes") is False
    )


def _ensure_document_clean_for_personal_view(
    context: StageContext,
    document: str,
) -> None:
    """Persist a dirty document and bind that extra save to its save-cycle record."""

    before_state = _file_change_state(context, document)
    if _file_change_state_is_clean(before_state):
        return
    if context.active_save_record is None:
        raise RuntimeError("personal-view cleaning save has no owning save-cycle record")

    path = context.document_paths[document]
    sha_before = sha256_file(path)
    result = context.rpc.call(
        "save_document",
        {"selector": {"document_name": document}},
        timeout=120.0,
    )
    sha_after = sha256_file(path)
    after_state = _file_change_state(context, document)
    truthful = bool(
        isinstance(result, dict)
        and str(result.get("save_disposition") or "").lower() == "written"
        and result.get("file_written") is True
        and result.get("durability_verified") is True
        and result.get("saved") is True
        and bool(sha_after)
        and sha_after != sha_before
        and _file_change_state_is_clean(after_state)
    )
    operation = {
        "kind": "pre_personal_view_clean_save",
        "document": document,
        "canonical_path": str(path),
        "before_file_change_state": before_state,
        "after_file_change_state": after_state,
        "sha256_before": sha_before,
        "sha256_after": sha_after,
        "disposition": result.get("save_disposition") if isinstance(result, dict) else None,
        "file_written": result.get("file_written") if isinstance(result, dict) else None,
        "durability_verified": (
            result.get("durability_verified") if isinstance(result, dict) else None
        ),
        "truthful": truthful,
        "result": result,
    }
    context.active_save_record.setdefault("actual_save_operations", []).append(operation)
    context.active_save_record["truthful"] = bool(
        context.active_save_record.get("truthful") and truthful
    )
    _require(
        context,
        "pre_personal_view_clean_save_is_recorded_and_truthful",
        truthful,
        operation,
    )
    _persist(context)


def _personal_action_documents(
    context: StageContext,
    action: str,
    params: dict[str, Any],
) -> list[str]:
    """Return every stage document whose personal state the action can observe/change."""

    candidates: list[str | None]
    if action == "set_active_document":
        candidates = [context.active_document, str(params.get("document") or "")]
    elif action in {"select_object", "expand_tree", "collapse_tree"}:
        candidates = [str(params.get("document") or "")]
    elif action in {"clear_selection", "reset_property_editor"}:
        candidates = list(context.document_paths)
    else:
        candidates = [context.active_document]
    documents: list[str] = []
    for candidate in candidates:
        if candidate and candidate in context.document_paths and candidate not in documents:
            documents.append(candidate)
    if not documents:
        raise RuntimeError(f"personal action {action!r} is not bound to a stage document")
    return documents


def _personal_document_snapshot(
    context: StageContext, document_name: str
) -> dict[str, Any]:
    """Observe exact state for one named document, failing closed otherwise."""

    state = _view_state(context, document_name)
    _require(
        context,
        "personal_action_observation_document_bound",
        state.get("observed_document") == document_name,
        {"expected": document_name, "state": state},
    )
    selector = state.get("identity_selector")
    _require(
        context,
        "personal_action_identity_selector_present",
        _identity_selector_is_exact(selector, document_name),
        {"document": document_name, "selector": selector},
    )
    file_state = state.get("file_change_state")
    return {
        "observed_document": state.get("observed_document"),
        "identity_selector": selector,
        "file_change_state": file_state if isinstance(file_state, dict) else {},
        "semantic_revisions": _semantic_revisions(
            context, dict(selector), _stage_revision_keys()
        ),
    }


def _run_view_mutation_cycle(context: StageContext, index: int) -> None:
    """One ADR §13 view/mutation cycle: personal view actions then a typed mutation."""

    cycle = new_cycle_record(index)
    document = context.primary if index % 2 == 0 else context.secondary
    other = context.secondary if index % 2 == 0 else context.primary
    cycle["document"] = document

    _ensure_document_clean_for_personal_view(context, document)

    selector = _identity_selector(context, document)
    keys = _stage_revision_keys()
    cycle["revisions_before"] = _semantic_revisions(context, selector, keys)
    cycle["file_change_state_before"] = _file_change_state(context, document)

    view_before = _view_state(context)
    context.last_view = view_before
    _local_action(
        context,
        cycle,
        "set_active_document",
        {"document": other},
        coverage="active_view_switching",
    )
    _local_action(
        context,
        cycle,
        "set_active_document",
        {"document": document},
        coverage="active_view_switching",
    )
    _local_action(
        context,
        cycle,
        "rotate_camera",
        {"yaw": 7.0 + index, "pitch": 5.0 + (index % 7), "roll": 0.0},
        coverage="camera_rotation",
    )
    _local_action(
        context,
        cycle,
        "pan_view",
        {"dx": 0.1 + (index % 5) * 0.05, "dy": 0.02, "dz": 0.0},
        coverage="pan",
    )
    _local_action(
        context,
        cycle,
        "zoom_view",
        {"direction": "in" if index % 2 == 0 else "out"},
        coverage="zoom",
    )
    _local_action(context, cycle, "fit_all", {"factor": 1.0}, coverage="fit")
    _local_action(
        context,
        cycle,
        "select_object",
        {"document": document, "object": ALPHA_OBJECT},
        coverage="selection",
    )
    _local_action(
        context,
        cycle,
        "expand_tree",
        {"document": document, "object": ALPHA_OBJECT},
        coverage="tree_expand",
    )
    _local_action(
        context,
        cycle,
        "collapse_tree",
        {"document": document, "object": ALPHA_OBJECT},
        coverage="tree_collapse",
    )
    _local_action(context, cycle, "clear_selection", coverage="selection")
    view_after = _view_state(context)
    context.last_view = view_after

    cycle["revisions_after_personal_view"] = _semantic_revisions(context, selector, keys)
    cycle["file_change_state_after_personal_view"] = _file_change_state(context, document)
    inert = cycle["revisions_after_personal_view"] == cycle["revisions_before"]
    not_dirtied = _file_change_state_is_clean(
        cycle["file_change_state_before"]
    ) and _file_change_state_is_clean(cycle["file_change_state_after_personal_view"])
    camera_changed = (
        view_after.get("camera_orientation") != view_before.get("camera_orientation")
        or view_after.get("view_position") != view_before.get("view_position")
    )
    cycle["checks"] = {
        "personal_view_state_inert": bool(inert),
        "personal_view_state_not_dirtying": bool(not_dirtied),
        "camera_changed": bool(camera_changed),
    }
    _require(
        context,
        "personal_view_activity_never_changes_semantic_revisions",
        inert,
        {
            "cycle": index,
            "before": cycle["revisions_before"],
            "after": cycle["revisions_after_personal_view"],
        },
    )
    _require(
        context,
        "personal_view_activity_never_sets_model_dirty",
        not_dirtied,
        {
            "cycle": index,
            "before": cycle["file_change_state_before"],
            "after": cycle["file_change_state_after_personal_view"],
        },
    )
    _require(
        context,
        "personal_view_activity_is_observable",
        camera_changed,
        {"cycle": index, "before": view_before, "after": view_after},
    )

    beta_before = _property_revision(context, selector, BETA_OBJECT, BETA_PROPERTY)
    session_id = _begin_checked_edit(
        context,
        cycle,
        selector,
        [_property_key(BETA_OBJECT, BETA_PROPERTY)],
    )
    committed = _commit_checked_integer(
        context,
        cycle,
        session_id=session_id,
        selector=selector,
        object_name=BETA_OBJECT,
        property_name=BETA_PROPERTY,
        value=100 + index,
        prove_exactly_once=True,
    )
    beta_after = _property_revision(context, selector, BETA_OBJECT, BETA_PROPERTY)
    cycle["checks"]["typed_mutation_committed_once"] = beta_after == beta_before + 1
    cycle["typed_mutation"] = {
        "operation_id": committed["operation_id"],
        "revision_before": beta_before,
        "revision_after": beta_after,
        "expected_value": 100 + index,
        "landed_value": _property_value(context, document, BETA_OBJECT, BETA_PROPERTY),
        "first_result": committed["first"],
        "replay_result": committed["replay"],
    }
    _require(
        context,
        "typed_mutation_advances_revision_exactly_once",
        beta_after == beta_before + 1,
        {
            "cycle": index,
            "before": beta_before,
            "after": beta_after,
            "commit": committed["first"],
        },
    )
    _require(
        context,
        "typed_mutation_value_landed",
        _property_value(context, document, BETA_OBJECT, BETA_PROPERTY) == 100 + index,
        {"cycle": index, "expected": 100 + index},
    )

    recompute_result = _remote_action(
        context,
        cycle,
        "recompute_document",
        [document],
        coverage="recompute",
        timeout=120.0,
        operation_id=str(uuid.uuid4()),
    )
    _require(
        context,
        "cycle_recompute_succeeds",
        isinstance(recompute_result, dict) and recompute_result.get("success") is True,
        recompute_result,
    )
    readiness = _mutation_readiness(context, document)
    cycle["readiness"] = readiness
    _require(
        context,
        "no_quarantine_during_cycles",
        _readiness_flag(readiness, "quarantined") is False
        and _readiness_flag(readiness, "collaboration_poisoned") is not True,
        {"cycle": index, "readiness": readiness},
    )

    cycle["revisions_after"] = _semantic_revisions(context, selector, keys)
    cycle["typed_mutation"]["revisions_before"] = cycle["revisions_before"]
    cycle["typed_mutation"]["revisions_after"] = cycle["revisions_after"]
    cycle["file_change_state_after"] = _file_change_state(context, document)
    record_cycle(context.payload, cycle)
    _persist(context)


def _save_copy_is_truthful(
    result: object,
    *,
    destination: Path,
    canonical_sha256_before: str,
    canonical_sha256_after: str,
    copy_sha256: str,
    readable_archive: bool,
) -> bool:
    """Bind Save Copy claims to both files without requiring identical ZIP bytes."""

    if not isinstance(result, dict):
        return False
    evidence = result.get("file_evidence")
    evidence = evidence if isinstance(evidence, dict) else {}
    archive = evidence.get("archive")
    archive = archive if isinstance(archive, dict) else {}
    expected_destination = os.path.normcase(os.path.realpath(destination))
    return bool(
        str(result.get("save_disposition") or "").lower() == "copy_written"
        and result.get("file_written") is True
        and result.get("saved") is True
        and readable_archive
        and bool(copy_sha256)
        and canonical_sha256_after == canonical_sha256_before
        and evidence.get("sha256") == copy_sha256
        and archive.get("ok") is True
        and os.path.normcase(os.path.realpath(str(result.get("target_path") or "")))
        == expected_destination
        and os.path.normcase(
            os.path.realpath(str(evidence.get("canonical_path") or ""))
        )
        == expected_destination
    )


def _run_save_cycle(context: StageContext, index: int) -> None:
    """One ADR §13 save cycle: written save, Unchanged save and Save Copy."""

    document = context.primary
    path = context.document_paths[document]
    _local_action(
        context,
        None,
        "set_active_document",
        {"document": document},
        coverage="active_view_switching",
        observe_view=False,
    )
    selector = _identity_selector(context, document)
    session_id = _begin_checked_edit(
        context,
        None,
        selector,
        [_property_key(BETA_OBJECT, BETA_PROPERTY)],
    )
    _commit_checked_integer(
        context,
        None,
        session_id=session_id,
        selector=selector,
        object_name=BETA_OBJECT,
        property_name=BETA_PROPERTY,
        value=100000 + index,
        prove_exactly_once=False,
    )

    sha_before = sha256_file(path)
    written = context.rpc.call(
        "save_document",
        {"selector": {"document_name": document}},
        timeout=120.0,
    )
    context.coverage.add("save")
    sha_after = sha256_file(path)
    unchanged = context.rpc.call(
        "save_document",
        {"selector": {"document_name": document}},
        timeout=120.0,
    )
    context.coverage.add("unchanged_save")
    sha_unchanged = sha256_file(path)

    copy_path = context.copies_dir / f"{document}-copy-{index}.FCStd"
    copy = context.rpc.call(
        "save_document_copy",
        {
            "selector": {"document_name": document},
            "destination": str(copy_path),
            "overwrite": True,
        },
        timeout=120.0,
    )
    context.coverage.add("save_copy")
    sha_after_copy = sha256_file(path)
    sha_copy = sha256_file(copy_path)
    copy_readable = archive_has_document_xml(copy_path)

    written_result = written if isinstance(written, dict) else {}
    written_ok = (
        isinstance(written, dict)
        and str(written.get("save_disposition") or "").lower() == "written"
        and written.get("file_written") is True
        and written.get("durability_verified") is True
        and written.get("saved") is True
        and bool(sha_after)
        and sha_after != sha_before
    )
    unchanged_ok = (
        isinstance(unchanged, dict)
        and str(unchanged.get("save_disposition") or "").lower() == "unchanged"
        and unchanged.get("file_written") is False
        and unchanged.get("unchanged") is True
        and sha_unchanged == sha_after
    )
    copy_ok = _save_copy_is_truthful(
        copy,
        destination=copy_path,
        canonical_sha256_before=sha_after,
        canonical_sha256_after=sha_after_copy,
        copy_sha256=sha_copy,
        readable_archive=copy_readable,
    )
    truthful = bool(written_ok and unchanged_ok and copy_ok)
    save_record = {
            "index": int(index),
            "document": document,
            "disposition": str(written_result.get("save_disposition") or ""),
            "file_written": bool(written_result.get("file_written")),
            "durability_verified": written_result.get("durability_verified"),
            "sha256_before": sha_before,
            "sha256_after": sha_after,
            "truthful": truthful,
            "canonical_path": str(path),
            "written_result": written,
            "unchanged_save": {
                "disposition": (
                    unchanged.get("save_disposition") if isinstance(unchanged, dict) else None
                ),
                "file_written": (
                    unchanged.get("file_written") if isinstance(unchanged, dict) else None
                ),
                "sha256_after": sha_unchanged,
                "truthful": bool(unchanged_ok),
                "result": unchanged,
            },
            "save_copy": {
                "destination": str(copy_path),
                "disposition": copy.get("save_disposition") if isinstance(copy, dict) else None,
                "readable_archive": bool(copy_readable),
                "canonical_unchanged": sha_after_copy == sha_after,
                "sha256_after": sha_copy,
                "artifact_sha256": None,
                "truthful": bool(copy_ok),
                "result": copy,
            },
            "canonical_artifact_sha256": None,
            "actual_save_operations": [
                {
                    "kind": "canonical_written_save",
                    "document": document,
                    "canonical_path": str(path),
                    "sha256_before": sha_before,
                    "sha256_after": sha_after,
                    "disposition": written_result.get("save_disposition"),
                    "file_written": written_result.get("file_written"),
                    "durability_verified": written_result.get("durability_verified"),
                    "truthful": bool(written_ok),
                    "result": written,
                },
                {
                    "kind": "canonical_unchanged_save",
                    "document": document,
                    "canonical_path": str(path),
                    "sha256_before": sha_after,
                    "sha256_after": sha_unchanged,
                    "disposition": (
                        unchanged.get("save_disposition")
                        if isinstance(unchanged, dict)
                        else None
                    ),
                    "file_written": (
                        unchanged.get("file_written")
                        if isinstance(unchanged, dict)
                        else None
                    ),
                    "truthful": bool(unchanged_ok),
                    "result": unchanged,
                },
                {
                    "kind": "save_copy",
                    "document": document,
                    "canonical_path": str(path),
                    "destination": str(copy_path),
                    "canonical_sha256_before": sha_after,
                    "canonical_sha256_after": sha_after_copy,
                    "sha256_after": sha_copy,
                    "disposition": (
                        copy.get("save_disposition") if isinstance(copy, dict) else None
                    ),
                    "file_written": (
                        copy.get("file_written") if isinstance(copy, dict) else None
                    ),
                    "truthful": bool(copy_ok),
                    "result": copy,
                },
            ],
        }
    record_save(context.payload, save_record)
    context.active_save_record = save_record
    _require(
        context,
        "written_save_is_truthful_against_observed_sha256",
        written_ok,
        {"index": index, "result": written, "before": sha_before, "after": sha_after},
    )
    _require(
        context,
        "unchanged_save_is_truthful_against_observed_sha256",
        unchanged_ok,
        {"index": index, "result": unchanged, "after": sha_unchanged},
    )
    _require(
        context,
        "save_copy_is_truthful_and_leaves_canonical_intact",
        copy_ok,
        {
            "index": index,
            "result": copy,
            "readable_archive": copy_readable,
            "canonical_sha256_before": sha_after,
            "canonical_sha256_after": sha_after_copy,
            "copy_sha256": sha_copy,
        },
    )
    _ensure_document_clean_for_personal_view(context, context.secondary)
    _persist(context)


def _prepare_local_property_edit(
    context: StageContext,
    document_name: str,
    object_name: str,
) -> None:
    _local_action(context, None, "reset_property_editor", observe_view=False)
    _local_action(context, None, "clear_selection", observe_view=False)
    _local_action(
        context,
        None,
        "set_active_document",
        {"document": document_name},
        coverage="active_view_switching",
        observe_view=False,
    )
    _local_action(
        context,
        None,
        "select_object",
        {"document": document_name, "object": object_name},
        coverage="selection",
        observe_view=False,
    )
    _local_action(
        context,
        None,
        "expand_tree",
        {"document": document_name, "object": object_name},
        coverage="tree_expand",
        observe_view=False,
    )


def _local_property_edit(
    context: StageContext,
    document_name: str,
    object_name: str,
    property_name: str,
    value: int,
) -> dict[str, Any]:
    """Change a model property through the real Property Editor, as a user does."""

    _prepare_local_property_edit(context, document_name, object_name)
    return _local_action(
        context,
        None,
        "local_property_edit",
        {
            "document": document_name,
            "object": object_name,
            "property": property_name,
            "value": int(value),
            "stage_prepared": True,
        },
        timeout=120.0,
        observe_view=False,
    )


def _run_same_property_conflict(context: StageContext) -> None:
    """ADR §6.1: a local edit of the same property makes the remote commit refuse."""

    document = context.primary
    _local_action(
        context,
        None,
        "set_active_document",
        {"document": document},
        observe_view=False,
    )
    selector = _identity_selector(context, document)
    operations: dict[str, Any] = {"selector": selector}
    session_id = _begin_checked_edit(
        context,
        None,
        selector,
        [{"kind": "ObjectModel", "subject": ALPHA_OBJECT}],
        capture=operations,
    )
    _semantic_revisions(context, selector, [_property_key(ALPHA_OBJECT, ALPHA_PROPERTY)])
    local_edit = _local_property_edit(
        context, document, ALPHA_OBJECT, ALPHA_PROPERTY, 42
    )
    operations["local_edit"] = local_edit
    _require(
        context,
        "local_property_edit_landed",
        _property_value(context, document, ALPHA_OBJECT, ALPHA_PROPERTY) == 42,
        {"document": document},
    )

    refusal_params = {
        "session_id": session_id,
        "doc_selector": selector,
        "object_name": ALPHA_OBJECT,
        "property_name": ALPHA_PROPERTY,
        "value_type": "integer",
        "value": "10",
        "operation_id": str(uuid.uuid4()),
    }
    refusal = _call_expecting_failure(
        context,
        "commit_checked_property",
        refusal_params,
    )
    data = refusal.get("data") or {}
    changed = data.get("changed_semantic_keys") or []
    expected = data.get("expected_revisions") or {}
    current = data.get("current_revisions") or {}
    targeted = refusal.get("success") is False and (
        refusal.get("error_code") == "DOCUMENT_CONFLICT"
        or "semantic revisions changed" in str(refusal.get("error") or "")
    )
    named = (
        f"ObjectProperty:{ALPHA_OBJECT}:{ALPHA_PROPERTY}" in changed
        or f"ObjectModel:{ALPHA_OBJECT}" in changed
    )
    readiness = _mutation_readiness(context, document)
    healthy = (
        _readiness_flag(readiness, "quarantined") is False
        and _readiness_flag(readiness, "collaboration_poisoned") is not True
    )
    normalized_refusal = dict(refusal)
    normalized_refusal["data"] = {
        "changed_semantic_keys": changed,
        "expected_revisions": expected,
        "current_revisions": current,
    }
    operations["refused_commit"] = {
        "operation_id": refusal_params["operation_id"],
        "method": "commit_checked_property",
        "parameters": refusal_params,
        "result": normalized_refusal,
    }
    operations["observed"] = {
        "document": document,
        "alpha_value": _property_value(context, document, ALPHA_OBJECT, ALPHA_PROPERTY),
        "expected_revisions": expected,
        "current_revisions": current,
    }
    record_conflict(
        context.payload,
        "same_property",
        {
            "document": document,
            "refusal": normalized_refusal,
            "changed_semantic_keys": changed,
            "expected_revisions": expected,
            "current_revisions": current,
            "readiness": readiness,
            "targeted": bool(targeted and named),
            "write_lane_healthy": bool(healthy),
            "stage_operations": operations,
        },
    )
    context.coverage.add("same_property_conflict")
    _require(context, "same_property_conflict_is_targeted", targeted and named, refusal)
    _require(
        context,
        "healthy_conflict_does_not_poison_write_lane",
        healthy,
        readiness,
    )
    context.rpc.call(
        "cancel_checked_edit",
        {
            "session_id": session_id,
            "reason": "stage conflict cleanup",
            "operation_id": str(uuid.uuid4()),
        },
        timeout=30.0,
    )


def _run_independent_property_success(context: StageContext) -> None:
    """ADR §6.2: a local Alpha edit and a remote Beta commit both land exactly once."""

    document = context.secondary
    _local_action(
        context,
        None,
        "set_active_document",
        {"document": document},
        observe_view=False,
    )
    selector = _identity_selector(context, document)
    operations: dict[str, Any] = {"selector": selector}
    alpha_before = _property_revision(context, selector, ALPHA_OBJECT, ALPHA_PROPERTY)
    beta_before = _property_revision(context, selector, BETA_OBJECT, BETA_PROPERTY)

    local_edit = _local_property_edit(
        context, document, ALPHA_OBJECT, ALPHA_PROPERTY, 11
    )
    operations["local_edit"] = local_edit
    session_id = _begin_checked_edit(
        context,
        None,
        selector,
        [_property_key(BETA_OBJECT, BETA_PROPERTY)],
        capture=operations,
    )
    committed = _commit_checked_integer(
        context,
        None,
        session_id=session_id,
        selector=selector,
        object_name=BETA_OBJECT,
        property_name=BETA_PROPERTY,
        value=30,
        prove_exactly_once=True,
        capture=operations,
    )
    alpha_after = _property_revision(context, selector, ALPHA_OBJECT, ALPHA_PROPERTY)
    beta_after = _property_revision(context, selector, BETA_OBJECT, BETA_PROPERTY)
    both_landed = (
        _property_value(context, document, ALPHA_OBJECT, ALPHA_PROPERTY) == 11
        and _property_value(context, document, BETA_OBJECT, BETA_PROPERTY) == 30
        and alpha_after == alpha_before + 1
        and beta_after == beta_before + 1
    )
    alpha_value = _property_value(context, document, ALPHA_OBJECT, ALPHA_PROPERTY)
    beta_value = _property_value(context, document, BETA_OBJECT, BETA_PROPERTY)
    record_conflict(
        context.payload,
        "independent_property",
        {
            "document": document,
            "alpha_revision_before": alpha_before,
            "alpha_revision_after": alpha_after,
            "beta_revision_before": beta_before,
            "beta_revision_after": beta_after,
            "commit": committed["first"],
            "replay": committed["replay"],
            "committed_once": True,
            "alpha_value": alpha_value,
            "beta_value": beta_value,
            "both_landed": bool(both_landed),
            "stage_operations": {
                **operations,
                "observed": {
                    "document": document,
                    "alpha_revision_before": alpha_before,
                    "alpha_revision_after": alpha_after,
                    "beta_revision_before": beta_before,
                    "beta_revision_after": beta_after,
                    "alpha_value": alpha_value,
                    "beta_value": beta_value,
                },
            },
        },
    )
    context.coverage.add("independent_property_success")
    _require(
        context,
        "independent_property_edits_both_land_once",
        both_landed,
        {
            "alpha": (alpha_before, alpha_after),
            "beta": (beta_before, beta_after),
        },
    )


def _probe_history_head(
    context: StageContext,
    selector: dict[str, Any],
    method: str,
    capture: dict[str, Any] | None = None,
) -> dict[str, Any]:
    """Learn the live history head from a deliberately mismatched typed refusal."""

    params: dict[str, Any] = {
        "doc_selector": selector,
        "operation_id": str(uuid.uuid4()),
    }
    if method == "undo":
        params["expected_undo_count"] = -1
        params["expected_undo_head"] = HISTORY_HEAD_PROBE
    else:
        params["expected_redo_count"] = -1
        params["expected_redo_head"] = HISTORY_HEAD_PROBE
    refusal = _call_expecting_failure(context, method, params)
    data = refusal.get("data") or {}
    _require(
        context,
        "history_action_refuses_mismatched_head",
        refusal.get("success") is False
        and refusal.get("error_code") == HISTORY_HEAD_REJECTED,
        {"method": method, "refusal": refusal},
    )
    envelope: dict[str, Any] = {
        "method": method,
        "success": refusal.get("success"),
        "error_code": refusal.get("error_code"),
        "error": refusal.get("error"),
    }
    if method == "undo":
        envelope["current_undo_count"] = data.get("current_undo_count")
        envelope["current_undo_head"] = data.get("current_undo_head")
        if capture is not None:
            capture["undo_probe"] = {
                "operation_id": params["operation_id"], "method": method,
                "parameters": params, "result": envelope,
            }
        return {
            "count": int(data.get("current_undo_count") or 0),
            "head": str(data.get("current_undo_head") or ""),
            "refusal": envelope,
        }
    envelope["current_redo_count"] = data.get("current_redo_count")
    envelope["current_redo_head"] = data.get("current_redo_head")
    if capture is not None:
        capture["redo_probe"] = {
            "operation_id": params["operation_id"], "method": method,
            "parameters": params, "result": envelope,
        }
    return {
        "count": int(data.get("current_redo_count") or 0),
        "head": str(data.get("current_redo_head") or ""),
        "refusal": envelope,
    }


def _run_history_segment(context: StageContext) -> None:
    """ADR §3.2 history: head-bound remote undo then redo around a local edit."""

    document = context.primary
    _local_action(
        context,
        None,
        "set_active_document",
        {"document": document},
        observe_view=False,
    )
    selector = _identity_selector(context, document)
    operations: dict[str, Any] = {"selector": selector}
    operations["local_edit"] = _local_property_edit(
        context, document, ALPHA_OBJECT, ALPHA_PROPERTY, 77
    )
    value_after_edit = _property_value(context, document, ALPHA_OBJECT, ALPHA_PROPERTY)
    _require(
        context,
        "history_segment_local_edit_landed",
        value_after_edit == 77,
        {"observed": value_after_edit},
    )

    undo_head = _probe_history_head(context, selector, "undo", capture=operations)
    _require(
        context,
        "history_stack_records_the_local_transaction",
        undo_head["count"] > 0,
        undo_head,
    )
    undo_params = {
        "doc_selector": selector,
        "operation_id": str(uuid.uuid4()),
        "expected_undo_count": undo_head["count"],
        "expected_undo_head": undo_head["head"],
    }
    undo_result = _remote_action(
        context,
        None,
        "undo",
        undo_params,
        timeout=120.0,
    )
    operations["undo"] = {
        "operation_id": undo_params["operation_id"], "method": "undo",
        "parameters": undo_params, "result": undo_result,
    }
    context.coverage.add("history_undo")
    value_after_undo = _property_value(context, document, ALPHA_OBJECT, ALPHA_PROPERTY)
    _require(
        context,
        "head_bound_undo_reverts_the_local_transaction",
        isinstance(undo_result, dict)
        and undo_result.get("success") is True
        and value_after_undo != value_after_edit,
        {"result": undo_result, "before": value_after_edit, "after": value_after_undo},
    )

    redo_head = _probe_history_head(context, selector, "redo", capture=operations)
    _require(
        context,
        "redo_stack_records_the_reverted_transaction",
        redo_head["count"] > 0,
        redo_head,
    )
    redo_params = {
        "doc_selector": selector,
        "operation_id": str(uuid.uuid4()),
        "expected_redo_count": redo_head["count"],
        "expected_redo_head": redo_head["head"],
    }
    redo_result = _remote_action(
        context,
        None,
        "redo",
        redo_params,
        timeout=120.0,
    )
    operations["redo"] = {
        "operation_id": redo_params["operation_id"], "method": "redo",
        "parameters": redo_params, "result": redo_result,
    }
    context.coverage.add("history_redo")
    value_after_redo = _property_value(context, document, ALPHA_OBJECT, ALPHA_PROPERTY)
    _require(
        context,
        "head_bound_redo_restores_the_reverted_transaction",
        isinstance(redo_result, dict)
        and redo_result.get("success") is True
        and value_after_redo == value_after_edit,
        {"result": redo_result, "expected": value_after_edit, "after": value_after_redo},
    )
    undo_refusal = undo_head["refusal"]
    redo_refusal = redo_head["refusal"]
    mismatched_head_refused = (
        undo_refusal.get("success") is False
        and undo_refusal.get("error_code") == HISTORY_HEAD_REJECTED
        and redo_refusal.get("success") is False
        and redo_refusal.get("error_code") == HISTORY_HEAD_REJECTED
    )
    context.payload["history"] = {
        "document": document,
        "undo_head": {"count": undo_head["count"], "head": undo_head["head"]},
        "undo_head_refusal": undo_refusal,
        "undo_result": undo_result,
        "value_after_edit": value_after_edit,
        "value_after_undo": value_after_undo,
        "redo_head": {"count": redo_head["count"], "head": redo_head["head"]},
        "redo_head_refusal": redo_refusal,
        "redo_result": redo_result,
        "value_after_redo": value_after_redo,
        "mismatched_head_refused": mismatched_head_refused,
        "stage_operations": {
            **operations,
            "observed": {
                "value_after_edit": value_after_edit,
                "value_after_undo": value_after_undo,
                "value_after_redo": value_after_redo,
            },
        },
    }
    _persist(context)
    _require(
        context,
        "mismatched_history_head_refusal_was_observed",
        mismatched_head_refused,
        {"undo": undo_refusal, "redo": redo_refusal},
    )


def _run_pause_resume_segment(context: StageContext) -> None:
    """ADR §7: pause and resume only through the real pauseAgentWrites checkbox."""

    document = context.primary
    _local_action(
        context,
        None,
        "set_active_document",
        {"document": document},
        observe_view=False,
    )
    selector = _identity_selector(context, document)
    operations: dict[str, Any] = {"document": document, "selector": selector}
    paused = _local_action(context, None, "pause_writes", observe_view=False)
    operations["pause"] = paused
    context.coverage.add("local_pause")
    _require(
        context,
        "pause_checkbox_reports_paused",
        (paused.get("observed") or {}).get("paused") is True,
        paused,
    )
    record_pause_resume(context.payload, "pause", paused)

    try:
        refusal_params = {
            "doc_name": document,
            "obj_name": BETA_OBJECT,
            "properties": {"Properties": {BETA_PROPERTY: 7}},
        }
        refusal = _call_expecting_failure(
            context,
            "edit_object",
            refusal_params,
        )
        operations["refused_write"] = {
            "method": "edit_object", "parameters": refusal_params, "result": refusal,
        }
        refusal_text = f"{refusal.get('error')} {refusal.get('error_code')} {refusal.get('data')}"
        record_pause_resume(context.payload, "refused", refusal)
        _require(
            context,
            "remote_write_refused_while_paused",
            refusal.get("success") is False
            and (
                "AUTOMATION_PAUSED" in refusal_text
                or "paused new MCP writes" in refusal_text
            ),
            refusal,
        )
        read_params = {
            "doc_selector": selector,
            "revision_keys": [_property_key(BETA_OBJECT, BETA_PROPERTY)],
        }
        read_result = _remote_action(
            context,
            None,
            "get_semantic_revisions",
            read_params,
            timeout=30.0,
        )
        reads = read_result.get("revisions") if isinstance(read_result, dict) else None
        operations["paused_read"] = {
            "operation_id": None, "method": "get_semantic_revisions",
            "parameters": read_params, "result": read_result,
        }
        operations["paused_read_result"] = read_result
        _require(
            context,
            "reads_remain_available_while_paused",
            _paused_read_revisions_are_exact(read_result),
            reads,
        )
        readiness = _mutation_readiness(context, document)
        operations["readiness"] = readiness
        context.payload["pause_resume"]["readiness_while_paused"] = readiness
    finally:
        resumed = _local_action(context, None, "resume_writes", observe_view=False)
        operations["resume"] = resumed
        context.coverage.add("local_resume")
        record_pause_resume(context.payload, "resume", resumed)
        _require(
            context,
            "resume_checkbox_reports_resumed",
            (resumed.get("observed") or {}).get("paused") is False,
            resumed,
        )

    session_id = _begin_checked_edit(
        context,
        None,
        selector,
        [_property_key(BETA_OBJECT, BETA_PROPERTY)],
        capture=operations,
    )
    committed = _commit_checked_integer(
        context,
        None,
        session_id=session_id,
        selector=selector,
        object_name=BETA_OBJECT,
        property_name=BETA_PROPERTY,
        value=55,
        prove_exactly_once=False,
        capture=operations,
        capture_key="after_commit",
    )
    value_after_resume = _property_value(context, document, BETA_OBJECT, BETA_PROPERTY)
    operations["value_after_resume"] = value_after_resume
    context.payload["pause_resume"]["stage_operations"] = operations
    record_pause_resume(
        context.payload,
        "after",
        {
            "commit": committed["first"],
            "value": value_after_resume,
        },
    )
    _require(
        context,
        "next_typed_mutation_succeeds_after_resume",
        _property_value(context, document, BETA_OBJECT, BETA_PROPERTY) == 55,
        committed["first"],
    )
    _persist(context)


def _run_cycle_program(context: StageContext) -> None:
    """Interleave ADR §13 view/mutation cycles, save cycles and the named scenarios."""

    definition = context.definition
    _provision_stage_documents(context)

    segments = {
        1: _run_same_property_conflict,
        2: _run_history_segment,
        3: _run_independent_property_success,
        4: _run_pause_resume_segment,
    }
    base_size, extra = divmod(
        definition.view_mutation_cycles, definition.save_cycles
    )
    next_cycle = 0
    for save_index in range(definition.save_cycles):
        _run_save_cycle(context, save_index)
        group_size = base_size + (1 if save_index < extra else 0)
        for _ in range(group_size):
            _run_view_mutation_cycle(context, next_cycle)
            segment = segments.get(next_cycle)
            if segment is not None:
                segment(context)
            next_cycle += 1


CYCLE_COUNT_CHECK = "cycle_count_matches_stage_definition"
SAVE_COUNT_CHECK = "save_count_matches_stage_definition"


def _personal_action_proof_is_exact(proof: object) -> bool:
    if not isinstance(proof, dict):
        return False
    action = proof.get("action")
    documents = proof.get("documents")
    before = proof.get("before")
    after = proof.get("after")
    if (
        action not in PERSONAL_STATE_ACTIONS
        or not isinstance(documents, list)
        or not documents
        or len(documents) != len(set(documents))
        or not isinstance(before, dict)
        or not isinstance(after, dict)
        or set(before) != set(documents)
        or set(after) != set(documents)
    ):
        return False
    if action == "set_active_document" and (
        proof.get("left_document") not in documents
        or proof.get("activated_document") not in documents
    ):
        return False
    for document_name in documents:
        before_snapshot = before.get(document_name)
        after_snapshot = after.get(document_name)
        if not isinstance(before_snapshot, dict) or not isinstance(after_snapshot, dict):
            return False
        if (
            before_snapshot.get("observed_document") != document_name
            or after_snapshot.get("observed_document") != document_name
        ):
            return False
        before_selector = before_snapshot.get("identity_selector")
        after_selector = after_snapshot.get("identity_selector")
        if not _identity_selector_is_exact(
            before_selector, document_name
        ) or not _identity_selector_is_exact(after_selector, document_name):
            return False
        identity_fields = (
            "document_uid",
            "document_instance_id",
            "lifecycle_epoch",
            "document_name",
        )
        if (
            any(
                before_selector.get(field) != after_selector.get(field)
                for field in identity_fields
            )
        ):
            return False
        if not _file_change_state_is_clean(before_snapshot.get("file_change_state")):
            return False
        if not _file_change_state_is_clean(after_snapshot.get("file_change_state")):
            return False
        before_revisions = before_snapshot.get("semantic_revisions")
        after_revisions = after_snapshot.get("semantic_revisions")
        if (
            not stage_revision_vector_is_exact(before_revisions)
            or not stage_revision_vector_is_exact(after_revisions)
            or before_revisions != after_revisions
        ):
            return False
    return (
        proof.get("clean_before") is True
        and proof.get("clean_after") is True
        and proof.get("semantic_revisions_unchanged") is True
        and proof.get("passed") is True
    )


def _record_stage_counts(context: StageContext) -> None:
    """Record the two ADR §13 count checks. Idempotent, once per stage.

    A stage that aborts early never reached ``_record_stage_aggregates``, so
    its artifact carried no check at all about the counts and a reader had to
    count the ``cycles`` and ``saves`` arrays by hand to discover that the
    program never ran - which invites "ran the full stage, failed 2 checks"
    and had to be prevented by hand, in prose, twice (GRK-P3-103).
    ``run_stage`` now calls this on the abort path too. Only the counts move
    there: the coverage and truthfulness aggregates legitimately describe a
    completed program and stay where they are.
    """

    definition = context.definition
    payload = context.payload
    recorded = {entry.get("name") for entry in payload.get("checks") or []}
    if CYCLE_COUNT_CHECK in recorded or SAVE_COUNT_CHECK in recorded:
        return
    record_check(
        payload,
        CYCLE_COUNT_CHECK,
        len(payload.get("cycles") or []) == definition.view_mutation_cycles,
        {
            "expected": definition.view_mutation_cycles,
            "observed": len(payload.get("cycles") or []),
        },
    )
    record_check(
        payload,
        SAVE_COUNT_CHECK,
        len(payload.get("saves") or []) == definition.save_cycles,
        {
            "expected": definition.save_cycles,
            "observed": len(payload.get("saves") or []),
        },
    )


def _record_stage_aggregates(context: StageContext) -> None:
    payload = context.payload
    _record_stage_counts(context)
    missing = sorted(set(COVERAGE_ITEMS) - context.coverage)
    payload["coverage"] = {
        "required": list(COVERAGE_ITEMS),
        "observed": sorted(context.coverage),
        "missing": missing,
    }
    record_check(
        payload,
        "adr_section_13_coverage_complete",
        not missing,
        {"missing": missing},
    )
    saves = payload.get("saves") or []
    record_check(
        payload,
        "every_save_cycle_is_truthful",
        bool(saves) and all(bool(entry.get("truthful")) for entry in saves),
        {
            "untruthful": [
                entry.get("index") for entry in saves if not entry.get("truthful")
            ]
        },
    )
    cycles = payload.get("cycles") or []
    personal_proofs = payload.get("personal_action_proofs") or []
    record_check(
        payload,
        "every_personal_action_has_exact_clean_revision_proof",
        bool(personal_proofs)
        and all(_personal_action_proof_is_exact(entry) for entry in personal_proofs),
        {
            "proof_count": len(personal_proofs),
            "violations": [
                entry.get("index") if isinstance(entry, dict) else None
                for entry in personal_proofs
                if not _personal_action_proof_is_exact(entry)
            ],
        },
    )
    record_check(
        payload,
        "every_cycle_keeps_personal_view_state_inert",
        bool(cycles)
        and all(
            bool((entry.get("checks") or {}).get("personal_view_state_inert"))
            for entry in cycles
        ),
        {
            "violations": [
                entry.get("index")
                for entry in cycles
                if not (entry.get("checks") or {}).get("personal_view_state_inert")
            ]
        },
    )
    record_check(
        payload,
        "every_cycle_starts_and_ends_personal_view_file_clean",
        bool(cycles)
        and all(
            bool((entry.get("checks") or {}).get("personal_view_state_inert"))
            and bool(
                (entry.get("checks") or {}).get(
                    "personal_view_state_not_dirtying"
                )
            )
            for entry in cycles
        ),
        {
            "violations": [
                entry.get("index")
                for entry in cycles
                if not (
                    (entry.get("checks") or {}).get("personal_view_state_inert")
                    and (entry.get("checks") or {}).get(
                        "personal_view_state_not_dirtying"
                    )
                )
            ]
        },
    )
    record_check(
        payload,
        "every_cycle_commits_its_typed_mutation_once",
        bool(cycles)
        and all(
            bool((entry.get("checks") or {}).get("typed_mutation_committed_once"))
            for entry in cycles
        ),
        {
            "violations": [
                entry.get("index")
                for entry in cycles
                if not (entry.get("checks") or {}).get("typed_mutation_committed_once")
            ]
        },
    )


def _record_artifact_snapshot(context: StageContext) -> None:
    """Mid-stage informational snapshot only; it never decides the verdict.

    ADR §8 and §15.6 require the artifact claim to be taken *after* the stage,
    with the documents closed and the owned process gone (GRK-P3-079). The
    snapshot is kept because it helps diagnose a failure, but the checks that
    gate the verdict come from ``_record_post_shutdown_artifact_scan``.
    """

    context.payload["artifacts_in_stage"] = scan_artifacts([context.documents_dir])


def _record_post_shutdown_artifact_scan(
    payload: dict[str, Any],
    documents_dir: Path,
) -> dict[str, Any]:
    """ADR §8 post-stage artifact scan; leftovers from teardown fail the stage."""

    scan = scan_artifacts([documents_dir])
    payload["artifacts"] = scan
    unreadable = [
        entry["path"]
        for entry in scan["documents"]
        if entry.get("readable_archive") is False
    ]
    ordinary = {
        os.path.normcase(os.path.normpath(str(entry.get("path") or ""))).casefold(): entry
        for entry in scan["documents"]
        if str(entry.get("path") or "").lower().endswith(".fcstd")
        and entry.get("readable_archive") is True
        and type(entry.get("size")) is int
        and entry["size"] > 0
    }
    required_save_paths: list[str] = []
    for save in payload.get("saves") or []:
        if not isinstance(save, dict):
            required_save_paths.append("<malformed-save>")
            continue
        canonical = save.get("canonical_path")
        copy = save.get("save_copy")
        destination = copy.get("destination") if isinstance(copy, dict) else None
        required_save_paths.append(canonical if isinstance(canonical, str) and canonical else "<missing-canonical>")
        required_save_paths.append(destination if isinstance(destination, str) and destination else "<missing-save-copy>")
    missing_save_artifacts = [
        path for path in required_save_paths
        if os.path.normcase(os.path.normpath(path)).casefold() not in ordinary
    ]
    ordinary_hashes = {
        os.path.normcase(os.path.normpath(str(entry["path"]))).casefold(): entry.get("sha256")
        for entry in scan["documents"]
        if str(entry.get("path") or "").lower().endswith(".fcstd")
    }
    for save in payload.get("saves") or []:
        if not isinstance(save, dict):
            continue
        canonical = save.get("canonical_path")
        copy = save.get("save_copy")
        if isinstance(canonical, str):
            save["canonical_artifact_sha256"] = ordinary_hashes.get(
                os.path.normcase(os.path.normpath(canonical)).casefold()
            )
        if isinstance(copy, dict) and isinstance(copy.get("destination"), str):
            copy["artifact_sha256"] = ordinary_hashes.get(
                os.path.normcase(os.path.normpath(copy["destination"])).casefold()
            )
    record_check(
        payload,
        "artifact_scan_has_no_unexplained_files",
        not scan["unexplained"],
        {"phase": "post_shutdown", "unexplained": scan["unexplained"]},
    )
    record_check(
        payload,
        "saved_documents_are_ordinarily_readable_archives",
        not unreadable and not missing_save_artifacts,
        {
            "phase": "post_shutdown", "unreadable": unreadable,
            "missing_canonical_or_copy": missing_save_artifacts,
        },
    )
    record_check(
        payload,
        "lock_anchors_are_classified_separately",
        all(
            entry["path"].lower().endswith(LOCK_ANCHOR_SUFFIX)
            for entry in scan["lock_anchors"]
        ),
        {"phase": "post_shutdown", "lock_anchors": scan["lock_anchors"]},
    )
    return scan


def _path_contains(parent: Path, child: str) -> bool:
    """Case-insensitive containment test that tolerates Windows path forms."""

    if not child:
        return False
    try:
        parent_norm = os.path.normcase(os.path.realpath(str(parent)))
        child_norm = os.path.normcase(os.path.realpath(child))
    except (OSError, ValueError):
        return False
    return child_norm == parent_norm or child_norm.startswith(parent_norm + os.sep)


LOCAL_DRIVER_PACKAGE = "tests/gui/part3/local_driver"

COORDINATOR_SOURCE_FILES = (
    "tests/gui/part3/stress_coordinator.py",
    "tests/gui/part3/stage_gate_runner.py",
    "tests/gui/part3/local_driver/actions.py",
    "tests/gui/part3/local_user_driver.py",
    "tests/gui/part3/remote_agent_driver.py",
    "tests/gui/part3/rpc_session_client.py",
    "tests/gui/part3/scenarios.py",
    "tests/gui/part3/evidence.py",
)


def _stage_source_files(repo_root: Path = REPO_ROOT) -> tuple[str, ...]:
    """Every source file that constitutes the stage program.

    ``install_part3_local_driver`` copies the WHOLE ``local_driver`` directory
    into ``<profile>/FreeCAD/Mod/Part3LocalDriver``, so every module in it
    runs inside FreeCAD during the stage: ``InitGui.py`` (the entry point),
    ``driver.py`` (the Qt owner-thread dispatcher this work stream turns on),
    ``control_channel.py`` (the authenticated loopback control server) and
    ``actions.py``. Only ``actions.py`` was fingerprinted, so two runs with
    materially different in-GUI driver code produced identical
    ``stage_sources`` digests (GRK-P3-102). The set is derived from the
    directory that is actually copied so it cannot drift again, and
    ``actions.py`` stays named explicitly because the P3-WP12 cross-check
    reads it by name.
    """

    ordered = list(COORDINATOR_SOURCE_FILES)
    directory = Path(repo_root).joinpath(*LOCAL_DRIVER_PACKAGE.split("/"))
    for path in sorted(directory.glob("*.py")):
        relative = f"{LOCAL_DRIVER_PACKAGE}/{path.name}"
        if relative not in ordered:
            ordered.append(relative)
    return tuple(ordered)


STAGE_SOURCE_FILES = _stage_source_files()
BINARY_INDEPENDENT_PATHS = frozenset({
    *STAGE_SOURCE_FILES,
    "tests/gui/part3/test_part3_stage_gate_runner.py",
    "tests/gui/part3/test_part3_stage_acceptance.py",
})

PROVENANCE_HISTORY_DEPTH = 40


def _git_lines(repo: Path, arguments: list[str]) -> list[str]:
    """Return non-empty stdout lines from one git command, or [] on failure."""

    completed = subprocess.run(
        ["git", "-C", str(repo), *arguments],
        capture_output=True,
        text=True,
        check=False,
    )
    if completed.returncode != 0:
        return []
    return [line for line in completed.stdout.splitlines() if line.strip()]


def _commit_history(repo_root: Path) -> list[tuple[str, int]]:
    """Recent HEAD-ancestry commits as (sha, committer epoch seconds)."""

    history: list[tuple[str, int]] = []
    for line in _git_lines(
        repo_root,
        ["log", f"-{PROVENANCE_HISTORY_DEPTH}", "--format=%H %ct", "HEAD"],
    ):
        fields = line.split()
        if len(fields) == 2 and fields[1].isdigit():
            history.append((fields[0], int(fields[1])))
    return history


def _binary_relevant_history(
    repo_root: Path, history: list[tuple[str, int]]
) -> list[tuple[str, int]]:
    """Return runtime-affecting entries using one fail-closed history query."""

    if not history:
        return []
    lines = _git_lines(
        repo_root,
        [
            "log",
            f"-{PROVENANCE_HISTORY_DEPTH}",
            "--format=%H %ct",
            "--",
            ".",
            *(
                f":(top,exclude){path}"
                for path in sorted(BINARY_INDEPENDENT_PATHS)
            ),
        ],
    )
    if not lines:
        return list(history)
    relevant = {
        fields[0]
        for line in lines
        if len(fields := line.split()) == 2 and fields[1].isdigit()
    }
    return [entry for entry in history if entry[0] in relevant]


def _build_provenance(repo_root: Path, executable: Path) -> dict[str, Any]:
    """ADR §13 provenance: tie the launched build and stage sources to commits.

    §13 requires Stage A/B to use the exact committed parent and nested SHAs
    and to record binary fingerprints. The fingerprints on their own never
    said which commit the build came from, so a run on a build older than
    HEAD was indistinguishable from a run on a build of HEAD (GRK-P3-098).

    ``commits_not_in_binary`` lists binary-relevant HEAD-ancestry commits
    committed after a binary was written. Commits confined to the separately
    hashed stage program or to non-runtime validator tests cannot affect
    compiled FreeCAD bytes and are deliberately excluded. Unknown paths fail
    closed as binary-relevant. Each binary is compared on its own because this
    build tree is mixed rather than uniformly stale.

    What this is NOT (GRK-P3-101): mtime-versus-commit-time ordering is
    necessary but not sufficient for a binding. A binary built later from a
    different branch reports ``predates_head: false`` while containing
    different code, and a binary built from a dirty tree and committed
    afterwards is flagged stale while actually containing the change. Nothing
    here hashes a binary against a build of a known commit, so the record is
    surfaced as a record - ``binary_commit_binding_enforced`` is False and
    ``binaries_predating_head`` sits in the artifact beside it.  Completed
    stage validation rejects a non-empty list as a necessary freshness
    condition, while still refusing to mislabel mtime ordering as a sufficient
    source-to-binary binding.
    """

    root = Path(repo_root)
    history = _commit_history(root)
    binary_history = _binary_relevant_history(root, history)
    head_commit, head_epoch = history[0] if history else ("", 0)

    binaries: dict[str, Any] = {}
    for path in freecad_binary_paths(Path(executable)):
        written = path.stat().st_mtime
        missing = [commit for commit, epoch in binary_history if epoch > written]
        binaries[path.name] = {
            "mtime_utc": _epoch_to_utc_iso(written),
            "commits_not_in_binary": missing,
            "predates_head": bool(missing),
        }

    return {
        "head_commit": head_commit,
        "head_committed_utc": _epoch_to_utc_iso(head_epoch) if head_epoch else "",
        "history_depth": len(history),
        "binaries": binaries,
        "binaries_predating_head": sorted(
            name for name, entry in binaries.items() if entry["predates_head"]
        ),
        "binary_commit_binding_enforced": False,
        "provenance_caveat": (
            "Recorded, not enforced. commits_not_in_binary compares a binary's "
            "mtime with timestamps of commits that change binary-relevant or "
            "unclassified paths. Stage-program paths are hashed independently; "
            "validator tests do not execute in a stage. Neither makes compiled "
            "binaries stale. "
            "This remains an upper bound, not proof of which source bytes "
            "produced a binary; exact source-to-binary binding remains an "
            "operator qualification."
        ),
        "stage_sources": {
            relative: sha256_file(root.joinpath(*relative.split("/")))
            for relative in STAGE_SOURCE_FILES
        },
    }


def _epoch_to_utc_iso(epoch: float) -> str:
    """Render a POSIX timestamp the way the evidence stamps every other time."""

    moment = datetime.fromtimestamp(float(epoch), tz=timezone.utc)
    return moment.replace(microsecond=0).isoformat()


def _observed_auth_state(rpc: Any, info: dict[str, Any]) -> dict[str, Any]:
    """Derive the ADR §8 ``auth`` block from the session in hand, not a constant.

    ``v2_session`` is true only when this process really holds a handshake_v2
    session token and the authenticated ``get_instance_info`` answered with the
    profile identity that session was bound to. Replace the authenticated client
    with a plain one and every field below goes false or empty.
    """

    session_token = str(getattr(rpc, "session_token", "") or "")
    mcp_instance_id = str(getattr(rpc, "mcp_instance_id", "") or "")
    profile_instance_id = info.get("profile_instance_id")
    return {
        "v2_session": bool(session_token)
        and bool(mcp_instance_id)
        and bool(profile_instance_id),
        "profile_instance_id": profile_instance_id,
        "protocol_version": info.get("protocol_version"),
        "mcp_instance_id": mcp_instance_id,
        "session_token_present": bool(session_token),
        "session_token_length": len(session_token),
        "observed_from": "get_instance_info issued over the authenticated session",
    }


def _verify_isolation(context: StageContext, coordinator: StressCoordinator) -> None:
    info = context.rpc.call("get_instance_info", timeout=30.0)
    info = info if isinstance(info, dict) else {}
    reported = str(info.get("profile_path") or "")
    profile_root = str(coordinator.profile_root.resolve())
    isolated = _path_contains(coordinator.profile_root, reported)
    outside_user_profile = not _path_contains(USER_APPDATA, reported)
    auth = _observed_auth_state(context.rpc, info)
    context.payload["environment"].update(
        {
            "reported_user_app_data": reported,
            "freecad_pid": info.get("pid"),
            "freecad_version": info.get("freecad_version"),
            "isolation_verified": bool(isolated and outside_user_profile),
            "auth": auth,
        }
    )
    _require(
        context,
        "authenticated_v2_session_is_in_hand",
        bool(auth["v2_session"]),
        auth,
    )
    _require(
        context,
        "freecad_reports_the_isolated_profile",
        isolated and outside_user_profile,
        {"reported": reported, "isolated_profile": profile_root},
    )


# The phase transitions an ADR §9 ordered shutdown stamps when it really runs
# to completion. ``ordered_shutdown_completed`` requires every one of them.
COMPLETED_SHUTDOWN_TRANSITIONS: tuple[str, ...] = SHUTDOWN_TIMESTAMP_KEYS


def ordered_shutdown_completed(
    result: dict[str, Any] | None,
    shutdown_record: dict[str, Any] | None,
) -> bool:
    """The one gate every caller uses to decide a shutdown really COMPLETED.

    An ALLOWLIST, deliberately. This predicate was built three times as a
    DENYLIST over failure markers - ``forced``, then ``failed_step`` and
    ``rpc_error`` (GRK-P3-106), and ``window_error`` was proposed next
    (GRK-P3-109) - and each time a reviewer found one more route through it.
    That shape cannot terminate: with ``local_driver`` None every window-close
    arm is False, so the helper reaches the deadline branch having recorded NO
    marker of any kind and there is nothing left to deny (GRK-P3-112). The
    positive formulation has no such hole, because a phase that never ran
    cannot stamp its own transition. It is also what this check has always
    been NAMED for, and what ``_assert_green_stage`` already asserts on the
    pytest path.

    ``result`` is what ``shutdown_launcher`` returned. ``shutdown_record`` is
    the block that was actually PERSISTED, and both are required: the returned
    ``success`` is True even for a phase that aborted, and every caller
    inherits that.

    Used at EVERY site that gates on shutdown success. GRK-P3-110 exists
    precisely because ``run_stage`` and ``_run_preflight_only`` were fixed
    independently, so there is one function and the sites call it.

    It only ever tightens. A forced kill fails it exactly as before, the two
    GRK-P3-106 conjuncts are retained rather than replaced, and no aborted or
    skipped shutdown can report success by any route.
    """

    returned = result or {}
    record = shutdown_record or {}
    return (
        bool(returned.get("success"))
        and record.get("forced") is False
        and record.get("failed_step") is None
        and record.get("rpc_error") is None
        and shutdown_transitions_are_complete_and_ordered(record)
    )


def stage_verdict(
    payload: dict[str, Any],
    *,
    stage_ok: bool,
    shutdown_ok: bool,
) -> str:
    """A stage is PASSED only when its checks, its artifacts and its teardown are.

    A forced kill can never be green: ``shutdown_ok`` is False whenever the owned
    process had to be terminated, and that alone yields FAILED.
    """

    if not stage_ok or not shutdown_ok:
        return "FAILED"
    return verdict_from_checks(payload)


def run_stage(
    stage: str | StageDefinition,
    *,
    repo_root: Path | None = None,
    run_root: Path | None = None,
    freecad_exe: Path | None = None,
) -> int:
    """Run one ADR §13 Stage A/B program; 0 pass, 1 stage failure, 2 fatal preflight.

    Stage C is refused before anything is launched: it is the separate P3-WP11
    fresh-session gate.
    """

    definition = resolve_executable_stage(getattr(stage, "stage", stage))
    root = (repo_root or REPO_ROOT).resolve()
    exe = Path(freecad_exe) if freecad_exe is not None else default_freecad_exe(root)
    if exe is None or not exe.is_file():
        print(
            "FreeCAD GUI binary not found via launcher overrides or "
            "under build/release or build/debug",
            file=sys.stderr,
        )
        return 2

    coordinator = StressCoordinator(repo_root=root, run_root=run_root)
    documents_dir = coordinator.run_root / "documents"
    copies_dir = documents_dir / "copies"
    try:
        coordinator.provision()
        documents_dir.mkdir(parents=True, exist_ok=True)
        copies_dir.mkdir(parents=True, exist_ok=True)
    except Exception as exc:
        print(f"stage {definition.stage} provisioning failed: {exc}", file=sys.stderr)
        return 2

    payload = json.loads(coordinator.evidence_path.read_text(encoding="utf-8"))
    payload["stage"] = definition.stage
    payload["mode"] = "stage"
    provenance = _build_provenance(root, exe)
    payload["environment"].update(
        {
            "executable": str(exe),
            "binary_fingerprint": binary_fingerprint(freecad_binary_paths(exe)),
            "git": git_state(root),
            "build_provenance": provenance,
            "documents_dir": str(documents_dir),
            "stage_definition": {
                "view_mutation_cycles": definition.view_mutation_cycles,
                "save_cycles": definition.save_cycles,
            },
            "session_ttl": session_ttl_provenance(root),
        }
    )
    # Named for what it tests. It goes green whenever provenance was RECORDED,
    # whatever the provenance says. It does not claim source-to-binary binding;
    # the shared completed-stage validator separately rejects a binary known
    # to predate HEAD (GRK-P3-101).
    record_check(
        payload,
        "build_provenance_is_recorded_for_binaries_and_stage_sources",
        bool(provenance["head_commit"])
        and bool(provenance["binaries"])
        and all(provenance["stage_sources"].values()),
        {
            "head_commit": provenance["head_commit"],
            "binaries": sorted(provenance["binaries"]),
            "binaries_predating_head": provenance["binaries_predating_head"],
            "binary_commit_binding_enforced": provenance[
                "binary_commit_binding_enforced"
            ],
            "stage_sources_fingerprinted": sorted(
                relative
                for relative, digest in provenance["stage_sources"].items()
                if digest
            ),
        },
    )
    write_evidence(coordinator.evidence_path, payload)
    print(f"evidence: {coordinator.evidence_path}", file=sys.stderr)

    suffix = uuid.uuid4().hex[:8]
    context: StageContext | None = None
    stage_ok = False
    fatal = False
    try:
        coordinator.launch_freecad(exe)
        coordinator.wait_for_launcher_ready()
        child = coordinator.spawn_remote_agent_child()
        child_report: dict[str, Any] = {}
        with contextlib.suppress(json.JSONDecodeError):
            child_report = json.loads(child.stdout or "{}")
        child_token_absent = (
            child.returncode == 0 and child_report.get("absent") is True
        )
        record_check(
            payload,
            "remote_agent_child_never_receives_the_control_token",
            child_token_absent,
            {"returncode": child.returncode, "report": child_report},
        )
        payload["environment"]["remote_actor"] = _remote_actor_record(
            child_token_absence_proved=child_token_absent,
        )
        handoff = coordinator.connect_local_driver()
        preflight = coordinator.run_preflight(handoff)
        payload["environment"]["control_endpoint"] = handoff.endpoint
        record_check(
            payload,
            "real_pause_agent_writes_checkbox_is_wired",
            preflight.get("pause_checkbox_wired") is True,
            preflight,
        )
        if preflight.get("pause_checkbox_wired") is not True:
            raise RuntimeError(
                "pauseAgentWrites checkbox is not wired - hard setup failure"
            )
        rpc = coordinator.authenticate_typed_session()
        actor = payload["environment"]["remote_actor"]
        actor["holds_rpc_session_in_coordinator_process"] = coordinator.holds_rpc_session()
        context = StageContext(
            definition=definition,
            payload=payload,
            evidence_path=coordinator.evidence_path,
            rpc=rpc,
            local=handoff.local_driver,
            launcher_module=coordinator._launcher_module,
            documents_dir=documents_dir,
            copies_dir=copies_dir,
            primary=f"Part3Stage{definition.stage}Primary{suffix}",
            secondary=f"Part3Stage{definition.stage}Secondary{suffix}",
        )
        _verify_isolation(context, coordinator)
        _persist(context)
        _run_cycle_program(context)
        _record_stage_aggregates(context)
        _record_artifact_snapshot(context)
        _persist(context)
        stage_ok = not payload.get("failed_checks")
    except StageCheckFailed as exc:
        print(f"stage {definition.stage} failed: {exc}", file=sys.stderr)
    except Exception as exc:
        record_check(payload, "stage_program_completed", False, str(exc))
        write_evidence(coordinator.evidence_path, payload)
        print(f"stage {definition.stage} aborted: {exc}", file=sys.stderr)
        fatal = context is None
    finally:
        if context is not None and not stage_ok:
            # An aborted stage still states, in its own artifact, that it
            # never reached the ADR §13 counts (GRK-P3-103). Each block
            # persists its own work: sequencing the only write after the
            # artifact walk meant that a raise out of that walk - and a live
            # FreeCAD is still creating and removing files under
            # documents_dir while this runs - silently dropped the counts
            # from the artifact, returning GRK-P3-103's defect in the exact
            # scenario it was filed about (GRK-P3-107).
            with contextlib.suppress(Exception):
                _record_stage_counts(context)
                _persist(context)
            with contextlib.suppress(Exception):
                _record_artifact_snapshot(context)
                _persist(context)
        shutdown = coordinator.shutdown_launcher(
            success_verdict="PASSED" if stage_ok else "FAILED"
        )

    final_payload = json.loads(coordinator.evidence_path.read_text(encoding="utf-8"))
    _record_post_shutdown_artifact_scan(final_payload, documents_dir)
    shutdown_record = final_payload.get("shutdown") or {}
    # ``graceful_shutdown_owned_session`` clears ``forced`` and
    # ``stalled_stage`` whenever the owned process exits inside the deadline,
    # even when the ordered phase ABORTED and the shutdown verb was never
    # sent. Reading only those two fields recorded a shutdown that never
    # completed as a completed one, on the most load-bearing check in Part 3
    # (GRK-P3-106). Adding one failure-marker conjunct per reviewed route
    # closed the RPC phase and would have closed the window-close branch
    # (GRK-P3-109) while leaving the route that records no marker at all
    # (GRK-P3-112) wide open. The gate now asks the shared allowlist
    # predicate - the same question ``_run_preflight_only`` asks.
    shutdown_ok = ordered_shutdown_completed(shutdown, shutdown_record)
    record_check(
        final_payload,
        "graceful_shutdown_completed_without_forced_termination",
        shutdown_ok,
        shutdown_record,
    )
    verdict = stage_verdict(final_payload, stage_ok=stage_ok, shutdown_ok=shutdown_ok)
    final_payload = finalize_evidence(final_payload, verdict=verdict)
    write_evidence(coordinator.evidence_path, final_payload)

    if verdict != "PASSED":
        if fatal:
            print_verdict_line("FAILED")
            return 2
        print_verdict_line("FAILED")
        return 1
    print_verdict_line("PASSED")
    return 0


def _run_preflight_only() -> int:
    """Provision, launch, preflight, tear down; verdict derived from the checks.

    GRK-P3-082: the verdict comes from ``verdict_from_checks`` over the envelope
    this run wrote, and that verdict is handed to ``shutdown_launcher`` instead
    of letting its ``success_verdict="PASSED"`` default stamp the artifact. A
    preflight whose own check failed therefore prints ``PART3_RESULT: FAILED``
    and exits non-zero; a green line is impossible while any check is false.
    """

    freecad_exe = default_freecad_exe(REPO_ROOT)
    if freecad_exe is None:
        print(
            "FreeCAD GUI binary not found via launcher overrides or "
            "under build/release or build/debug",
            file=sys.stderr,
        )
        return 2

    coordinator = StressCoordinator()
    # Fail closed: the only assignment that can make this True is the shared
    # predicate in the ``finally`` below.
    shutdown_ok = False
    shutdown_record: dict[str, Any] = {}
    preflight_verdict = "FAILED"
    try:
        coordinator.provision()
        _stamp_evidence_mode(coordinator.evidence_path, "preflight_only")
        coordinator.launch_freecad()
        try:
            coordinator.wait_for_launcher_ready()
            remote = coordinator.spawn_remote_agent_child()
            if remote.returncode != 0:
                print(remote.stdout or remote.stderr, file=sys.stderr)
                print_verdict_line("FAILED")
                return 1
            handoff = coordinator.connect_local_driver()
            preflight = coordinator.run_preflight(handoff)
            coordinator.record_preflight_check(preflight)
            preflight_verdict = verdict_from_checks(
                json.loads(coordinator.evidence_path.read_text(encoding="utf-8"))
            )
        finally:
            # GRK-P3-110: this path gated on the returned ``success`` alone,
            # so an ordered shutdown that ABORTED at ``close_document`` - and
            # returns ``success: True`` whenever the owned process happens to
            # exit inside the deadline - printed PART3_RESULT: PASSED with
            # exit 0 over an artifact carrying ``failed_step:
            # "document_close"`` and three null phase timestamps. It asks the
            # SAME predicate ``run_stage`` asks, of the block that was really
            # persisted rather than of the return value alone.
            shutdown_result = coordinator.shutdown_launcher(
                success_verdict=preflight_verdict
            )
            shutdown_record = _load_shutdown_payload(coordinator.evidence_path)
            shutdown_ok = ordered_shutdown_completed(shutdown_result, shutdown_record)
    except Exception as exc:
        print(f"preflight failed: {exc}", file=sys.stderr)
        print_verdict_line("FAILED")
        return 2

    # Unlike ``run_stage``, nothing downstream rewrote this envelope, so the
    # verdict stamped mid-shutdown was the one that survived on disk. Record
    # the teardown check and recompute, so the artifact can never say PASSED
    # over a shutdown this very run refused (GRK-P3-110).
    verdict = "PASSED" if preflight_verdict == "PASSED" and shutdown_ok else "FAILED"
    final_payload = json.loads(coordinator.evidence_path.read_text(encoding="utf-8"))
    record_check(
        final_payload,
        "graceful_shutdown_completed_without_forced_termination",
        shutdown_ok,
        shutdown_record,
    )
    write_evidence(
        coordinator.evidence_path, finalize_evidence(final_payload, verdict=verdict)
    )

    print_verdict_line(verdict)
    return 0 if verdict == "PASSED" else 1


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--stage",
        help="Run Stage A or Stage B (ADR section 13). Stage C belongs to P3-WP11; refused.",
    )
    parser.add_argument(
        "--preflight-only",
        action="store_true",
        help="Provision, launch, preflight, write evidence; do not run stress loops",
    )
    args = parser.parse_args(argv)

    if args.stage:
        try:
            definition = resolve_executable_stage(args.stage)
        except ValueError as exc:
            print(str(exc), file=sys.stderr)
            return 2
        if args.preflight_only:
            print(
                "--stage and --preflight-only are mutually exclusive: a "
                "preflight-only run executes zero cycles and must never be "
                f"reported as a stage result. Run either --stage {args.stage} "
                "or --preflight-only, not both.",
                file=sys.stderr,
            )
            return 2
        return run_stage(definition)

    if args.preflight_only:
        return _run_preflight_only()

    parser.print_help()
    return 2


if __name__ == "__main__":
    raise SystemExit(main())
